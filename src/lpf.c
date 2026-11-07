/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <math.h>

#include "audio_process.h"

/*
 * First-order IIR low-pass filter (T32+).
 *
 * State layout (int16_t[2], managed by libimp):
 *   [0] = previous output sample (Q0)
 *   [1] = alpha coefficient (Q8, range 0-256)
 *
 * Alpha is derived from cutoff/sample_rate:
 *   alpha = (1 - e^(-2*pi*fc/fs)) * 256
 */

void audio_process_lpf_create(int16_t *state, int sample_rate, int cutoff_freq)
{
	if (!state)
		return;
	if (sample_rate <= 0)
		sample_rate = 16000;

	float ratio = (float)cutoff_freq / (float)sample_rate;

	state[0] = 0;
	if (ratio > 0.0f && ratio < 0.5f) {
		double alpha = 1.0 - exp(-2.0 * M_PI * (double)ratio);
		state[1] = (int16_t)(alpha * 256.0);
	} else {
		state[1] = 256;
	}
}

void audio_process_lpf_process(int16_t *state, int16_t *data, int num_samples)
{
	if (!state || !data || num_samples <= 0)
		return;

	int32_t y = state[0];
	const int32_t alpha = state[1];
	int16_t *p = data;
	int n = num_samples;

	/* Unroll 4x with pointer walking */
	while (n >= 4) {
		y = (int16_t)((int16_t)((uint32_t)(((int32_t)p[0] - y) * alpha) >> 8) + y);
		p[0] = (int16_t)y;

		y = (int16_t)((int16_t)((uint32_t)(((int32_t)p[1] - y) * alpha) >> 8) + y);
		p[1] = (int16_t)y;

		y = (int16_t)((int16_t)((uint32_t)(((int32_t)p[2] - y) * alpha) >> 8) + y);
		p[2] = (int16_t)y;

		y = (int16_t)((int16_t)((uint32_t)(((int32_t)p[3] - y) * alpha) >> 8) + y);
		p[3] = (int16_t)y;

		p += 4;
		n -= 4;
	}

	while (n > 0) {
		y = (int16_t)((int16_t)((uint32_t)(((int32_t)*p - y) * alpha) >> 8) + y);
		*p++ = (int16_t)y;
		n--;
	}

	state[0] = (int16_t)y;
}

void audio_process_lpf_free(void)
{
}
