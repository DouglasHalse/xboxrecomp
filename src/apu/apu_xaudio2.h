/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Output channels: 2, or 6 (5.1: FL FR C LFE BL BR) with
 * RECOMP_AUDIO_CHANNELS=6 on Windows. With 6, xa2_submit_samples still takes
 * the front pair; the other four come from xa2_surround_push, in the same
 * sample order (a FIFO on the frame thread). */
int  xa2_channels(void);
void xa2_surround_push(const int16_t *c_lfe_bl_br, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);
int xa2_queued(void);   /* buffers queued for the device */
void xa2_take_stats(int *underruns, int *drops, int *min_queued, int *waits, int *refills);

#endif /* APU_XAUDIO2_H */
