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
 * Provides optimized real FFT transforms matching Ooura's RDFT frequency packing:
 *   a[0] = DC real
 *   a[1] = Nyquist real
 *   a[2*k] = Re(X[k])
 *   a[2*k + 1] = -Im(X[k])   (for k = 1 .. N/2 - 1)
 *
 * Sizes supported: 128, 256, 512.
 */

void audio_process_rdft(size_t n, int isgn, float *a);

/* ABI drop-in replacements for WebRTC legacy symbols */
void WebRtc_rdft(size_t n, int isgn, float *a, size_t *ip, float *w);
void aec_rdft_init(void);
void aec_rdft_forward_128(float *a);
void aec_rdft_inverse_128(float *a);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PROCESS_FFT_H */
