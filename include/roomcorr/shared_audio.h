/*
 * roomcorr shared audio: live audio from the roomcorr engine, published in
 * POSIX shared memory for visualizers, meters, LED matrices and other tools.
 *
 * The engine writes; any number of processes can read, without locks and
 * without talking to the engine. This header is self-contained (C99/C++11)
 * so other projects can copy it. See docs/SHARING.md.
 *
 * Layout of the shared object RC_SHARED_AUDIO_NAME (shm_open):
 *
 *   [ rc_shared_audio_header (header_size bytes) ]
 *   [ channel 0: capacity float samples ] ... [ channel N-1 ]
 *
 * Each channel is a ring buffer. Sample number n (counting from the moment
 * the engine started) of channel c lives at index (n & (capacity - 1)).
 * write_frames is the total number of frames written so far; the newest
 * frame is write_frames - 1.
 *
 * Reading the latest `count` frames:
 *   1. w = load_acquire(write_frames)
 *   2. copy frames [w - count, w)
 *   3. w2 = load_acquire(write_frames); if w2 - (w - count) > capacity the
 *      writer lapped you while copying: retry.
 * rc_shared_audio_read_latest() below does exactly this.
 */
#ifndef ROOMCORR_SHARED_AUDIO_H
#define ROOMCORR_SHARED_AUDIO_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_SHARED_AUDIO_NAME "/roomcorr-audio"
#define RC_SHARED_AUDIO_MAGIC "RCAUDIO"  /* 8 bytes including the NUL */
#define RC_SHARED_AUDIO_VERSION 1u

/* Channel indices (version 1). */
enum rc_shared_audio_channel {
  RC_AUDIO_IN_L = 0,   /* what the player sent: left (5.1 folded to stereo) */
  RC_AUDIO_IN_R = 1,   /* ... right */
  RC_AUDIO_OUT_L = 2,  /* after room correction: left speaker */
  RC_AUDIO_OUT_R = 3,  /* ... right speaker */
  RC_AUDIO_OUT_SUB = 4,/* ... subwoofer */
  RC_AUDIO_CHANNELS = 5
};

/* flags */
#define RC_AUDIO_FLAG_ENABLED 0x1u  /* correction on (off = bypass) */
#define RC_AUDIO_FLAG_MUTED 0x2u

typedef struct rc_shared_audio_header {
  char magic[8];           /* RC_SHARED_AUDIO_MAGIC */
  uint32_t version;        /* RC_SHARED_AUDIO_VERSION */
  uint32_t header_size;    /* byte offset of channel 0 */
  uint32_t sample_rate;    /* Hz */
  uint32_t channels;       /* number of channel rings */
  uint32_t capacity;       /* frames per ring, a power of two */
  uint32_t flags;          /* RC_AUDIO_FLAG_* (updated by the engine) */
  uint64_t write_frames;   /* atomic: total frames written */
  uint64_t update_ns;      /* atomic: CLOCK_MONOTONIC time of the last write */
  char channel_names[8][8];/* "in_l", "in_r", "out_l", "out_r", "out_sub" */
  uint8_t reserved[96];
} rc_shared_audio_header;

static inline const float* rc_shared_audio_channel_ptr(const rc_shared_audio_header* h, uint32_t channel) {
  return (const float*)((const uint8_t*)h + h->header_size) + (uint64_t)channel * h->capacity;
}

static inline uint64_t rc_shared_audio_write_frames(const rc_shared_audio_header* h) {
  return __atomic_load_n(&h->write_frames, __ATOMIC_ACQUIRE);
}

/* Copies the newest `count` frames of `channel` into dst (oldest first).
 * `count` may be at most capacity / 2: the engine writes a block into the
 * ring before publishing write_frames, so the oldest part of the ring can
 * be mid-overwrite at any moment.
 * Returns the frame number just after the last copied frame, or 0 when not
 * enough audio has been written yet or the reader kept getting lapped. */
static inline uint64_t rc_shared_audio_read_latest(const rc_shared_audio_header* h, uint32_t channel, float* dst,
                                                   uint32_t count) {
  if (channel >= h->channels || count > h->capacity / 2) return 0;
  const float* ring = rc_shared_audio_channel_ptr(h, channel);
  const uint64_t mask = h->capacity - 1;
  for (int attempt = 0; attempt < 4; ++attempt) {
    uint64_t w = rc_shared_audio_write_frames(h);
    if (w < count) return 0;
    uint64_t start = w - count;
    uint64_t first = start & mask;
    uint64_t n1 = count < h->capacity - first ? count : h->capacity - first;
    memcpy(dst, ring + first, n1 * sizeof(float));
    memcpy(dst + n1, ring, (count - n1) * sizeof(float));
    uint64_t w2 = rc_shared_audio_write_frames(h);
    if (w2 - start <= h->capacity / 2) return w;
  }
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* ROOMCORR_SHARED_AUDIO_H */
