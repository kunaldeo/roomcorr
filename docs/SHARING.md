# Sharing roomcorr's audio and state with other programs

roomcorr publishes what it is doing so other tools (music visualizers, LED
matrices, meters, recorders) can use it without touching the audio path.

| What | Where | How |
|---|---|---|
| Live audio (input and corrected outputs) | `/dev/shm/roomcorr-audio` | lock-free shared-memory ring, [`include/roomcorr/shared_audio.h`](../include/roomcorr/shared_audio.h) |
| Engine state, meters, 1/6-octave spectra | `$XDG_RUNTIME_DIR/roomcorr.sock` | newline-delimited JSON (`get`, `status`, `subscribe`, …; see [DETAILS.md](DETAILS.md)) |

## Live audio: `/dev/shm/roomcorr-audio`

A POSIX shared-memory object (`shm_open("/roomcorr-audio")`) created by the
engine at start-up and removed when it stops. The engine writes every audio
block into it from the realtime thread (one memcpy per channel); any number of
readers map it read-only and never block the engine.

| Channel | Name | Contents |
|---|---|---|
| 0 | `in_l` | what the player sent, left (5.1 content folded to stereo) |
| 1 | `in_r` | ... right |
| 2 | `out_l` | after room correction and crossover: left speaker |
| 3 | `out_r` | ... right speaker |
| 4 | `out_sub` | ... subwoofer |

32-bit float, 48 kHz, one ring of 65,536 frames (1.37 s) per channel.
The header carries the version, sample rate, channel count and names, ring
capacity, the total number of frames written (`write_frames`, atomic), the time
of the last write (`update_ns`, `CLOCK_MONOTONIC`) and flags (correction
enabled, muted). When `update_ns` stops moving, the engine is idle or gone.

Reading the newest 2048 frames of the left input:

```c
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "roomcorr/shared_audio.h"

int fd = shm_open(RC_SHARED_AUDIO_NAME, O_RDONLY, 0);
struct stat st;
fstat(fd, &st);
const rc_shared_audio_header* h = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
close(fd);

float buf[2048];
if (rc_shared_audio_read_latest(h, RC_AUDIO_IN_L, buf, 2048)) {
    /* buf holds the most recent 2048 samples, oldest first */
}
```

Rules for readers:

- Check `magic` and `version` before trusting the layout.
- Read at most `capacity / 2` frames at a time (the helper enforces it).
- Samples are published in 256-frame blocks; poll at your frame rate and read
  the newest window each time.
- The audio is written when the engine processes it, a few tens of
  milliseconds before it leaves the speakers (PipeWire and device buffers).
  Visualizers that want perfect sync can delay their display by that much.

The format is versioned: new fields go into the reserved space and new
channels at the end, so version-1 readers keep working.
