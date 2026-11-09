/* SPDX-License-Identifier: MIT */
#ifndef AUDIO_PROCESS_FFT_H
#define AUDIO_PROCESS_FFT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modern Clean-Room Radix-4/2 Real-Valued FFT / IFFT (RDFT)
 *
 * Supported sizes: 128, 256, 512, 1024.
 *
 * Natural Split-Complex Interface (clean-room, direct split-complex buffers):
 *   audio_process_rdft_forward(n, in_time, out_re, out_im);
 *   audio_process_rdft_inverse(n, in_re, in_im, out_time);
 *
 * Legacy packed interface (Ooura format compatibility):
 *   audio_process_rdft(n, isgn, a);
 */

void audio_process_rdft(size_t n, int isgn, float *a);

void audio_process_rdft_forward(size_t n, const float *in_time, float *out_re, float *out_im);
void audio_process_rdft_inverse(size_t n, const float *in_re, const float *in_im, float *out_time);

/* ABI drop-in replacements for WebRTC legacy symbols */
void WebRtc_rdft(size_t n, int isgn, float *a, size_t *ip, float *w);
void aec_rdft_init(void);
void aec_rdft_forward_128(float *a);
void aec_rdft_inverse_128(float *a);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PROCESS_FFT_H */
