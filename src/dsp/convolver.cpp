#include "dsp/convolver.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace rc {

// FFTW planning is not thread-safe; plan creation happens rarely, so one
// global lock is fine.
static std::mutex g_fftw_plan_mutex;

Convolver::Convolver(int block, int max_taps)
    : B_(block), N_(2 * block), bins_(block + 1), max_parts_((max_taps + block - 1) / block) {
  time_ = fftwf_alloc_real(size_t(N_));
  ifft_out_ = fftwf_alloc_real(size_t(N_));
  freq_ = fftwf_alloc_complex(size_t(bins_));
  {
    std::lock_guard<std::mutex> lock(g_fftw_plan_mutex);
    fwd_ = fftwf_plan_dft_r2c_1d(N_, time_, freq_, FFTW_MEASURE);
    inv_ = fftwf_plan_dft_c2r_1d(N_, freq_, ifft_out_, FFTW_MEASURE);
  }
  std::fill(time_, time_ + N_, 0.f);
  fdl_re_.assign(size_t(max_parts_) * bins_, 0.f);
  fdl_im_.assign(size_t(max_parts_) * bins_, 0.f);
  acc_re_.assign(bins_, 0.f);
  acc_im_.assign(bins_, 0.f);
  acc2_re_.assign(bins_, 0.f);
  acc2_im_.assign(bins_, 0.f);
  tmp_out_.assign(B_, 0.f);
  current_ = make_spectrum({}).release();
}

Convolver::~Convolver() {
  delete current_;
  delete pending_.exchange(nullptr);
  delete retired_.exchange(nullptr);
  std::lock_guard<std::mutex> lock(g_fftw_plan_mutex);
  fftwf_destroy_plan(fwd_);
  fftwf_destroy_plan(inv_);
  fftwf_free(time_);
  fftwf_free(ifft_out_);
  fftwf_free(freq_);
}

std::unique_ptr<Convolver::Spectrum> Convolver::make_spectrum(const std::vector<float>& ir_in) {
  std::vector<float> ir = ir_in;
  if (ir.empty()) ir = {1.f};
  if (int(ir.size()) > max_parts_ * B_) ir.resize(size_t(max_parts_) * B_);

  auto s = std::make_unique<Spectrum>();
  s->parts = int((ir.size() + B_ - 1) / B_);
  s->re.assign(size_t(s->parts) * bins_, 0.f);
  s->im.assign(size_t(s->parts) * bins_, 0.f);

  // Private buffers and plan so this can run while the audio thread uses the
  // shared ones.
  float* t = fftwf_alloc_real(size_t(N_));
  fftwf_complex* f = fftwf_alloc_complex(size_t(bins_));
  fftwf_plan plan;
  {
    std::lock_guard<std::mutex> lock(g_fftw_plan_mutex);
    plan = fftwf_plan_dft_r2c_1d(N_, t, f, FFTW_ESTIMATE);
  }
  // Fold the 1/N inverse-FFT normalisation into the filter.
  const float scale = 1.f / float(N_);
  for (int p = 0; p < s->parts; ++p) {
    std::fill(t, t + N_, 0.f);
    size_t off = size_t(p) * B_;
    size_t n = std::min<size_t>(B_, ir.size() - off);
    for (size_t i = 0; i < n; ++i) t[i] = ir[off + i] * scale;
    fftwf_execute(plan);
    for (int k = 0; k < bins_; ++k) {
      s->re[size_t(p) * bins_ + k] = f[k][0];
      s->im[size_t(p) * bins_ + k] = f[k][1];
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_fftw_plan_mutex);
    fftwf_destroy_plan(plan);
  }
  fftwf_free(t);
  fftwf_free(f);
  return s;
}

void Convolver::collect() { delete retired_.exchange(nullptr); }

void Convolver::set_filter(const std::vector<float>& ir) {
  collect();
  Spectrum* s = make_spectrum(ir).release();
  // If the previous filter was never picked up, it was never seen by the
  // audio thread, so it is ours to free.
  delete pending_.exchange(s);
}

void Convolver::accumulate(const Spectrum& h, float* __restrict acc_re, float* __restrict acc_im) {
  std::fill(acc_re, acc_re + bins_, 0.f);
  std::fill(acc_im, acc_im + bins_, 0.f);
  for (int p = 0; p < h.parts; ++p) {
    // Partition p pairs with the input spectrum from p blocks ago.
    int slot = fdl_pos_ - p;
    if (slot < 0) slot += max_parts_;
    const float* __restrict xr = &fdl_re_[size_t(slot) * bins_];
    const float* __restrict xi = &fdl_im_[size_t(slot) * bins_];
    const float* __restrict hr = &h.re[size_t(p) * bins_];
    const float* __restrict hi = &h.im[size_t(p) * bins_];
    for (int k = 0; k < bins_; ++k) {
      acc_re[k] += xr[k] * hr[k] - xi[k] * hi[k];
      acc_im[k] += xr[k] * hi[k] + xi[k] * hr[k];
    }
  }
}

void Convolver::inverse(const float* acc_re, const float* acc_im, float* out) {
  for (int k = 0; k < bins_; ++k) {
    freq_[k][0] = acc_re[k];
    freq_[k][1] = acc_im[k];
  }
  fftwf_execute(inv_);
  // Overlap-save: the second half is the valid linear-convolution output.
  std::memcpy(out, ifft_out_ + B_, sizeof(float) * size_t(B_));
}

void Convolver::process(const float* in, float* out) {
  // Slide the input window and transform it into the delay line.
  std::memmove(time_, time_ + B_, sizeof(float) * size_t(B_));
  std::memcpy(time_ + B_, in, sizeof(float) * size_t(B_));
  fftwf_execute(fwd_);
  fdl_pos_ = (fdl_pos_ + 1) % max_parts_;
  float* xr = &fdl_re_[size_t(fdl_pos_) * bins_];
  float* xi = &fdl_im_[size_t(fdl_pos_) * bins_];
  for (int k = 0; k < bins_; ++k) {
    xr[k] = freq_[k][0];
    xi[k] = freq_[k][1];
  }

  // Only swap once the previous retired filter has been freed, so the audio
  // thread never has to free (or leak) anything itself.
  Spectrum* next = nullptr;
  if (pending_.load(std::memory_order_acquire) && !retired_.load(std::memory_order_acquire))
    next = pending_.exchange(nullptr, std::memory_order_acq_rel);

  accumulate(*current_, acc_re_.data(), acc_im_.data());
  if (!next) {
    inverse(acc_re_.data(), acc_im_.data(), out);
    return;
  }

  accumulate(*next, acc2_re_.data(), acc2_im_.data());
  inverse(acc_re_.data(), acc_im_.data(), tmp_out_.data());
  inverse(acc2_re_.data(), acc2_im_.data(), out);
  for (int i = 0; i < B_; ++i) {
    float w = float(i + 1) / float(B_);
    out[i] = tmp_out_[i] * (1.f - w) + out[i] * w;
  }
  retired_.store(current_, std::memory_order_release);
  current_ = next;
}

}  // namespace rc
