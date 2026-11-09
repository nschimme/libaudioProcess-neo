/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <math.h>

#include "audio_process.h"
#include "biquad.h"
#include "util.h"
#include <string.h>

/*
 * Second-order Butterworth high-pass filter.
 *
 * libimp manages the state buffer and passes it to hpf_process. We overlay
 * a float biquad struct (28 bytes) onto libimp's 32-byte allocation. The
 * filter is designed on first process call since hpf_create only does memset.
 *
 * Cutoff is 300 Hz -- standard for speech HPF, removes DC offset and
 * low-frequency rumble from the mic signal.
 */

#define HPF_CUTOFF_HZ 300
#define HPF_DEFAULT_FS 16000

static inline void set_w16(int16_t *ptr, int16_t val, int count)
{
	if (!ptr || count <= 0)
		return;
	for (int i = 0; i < count; i++)
		ptr[i] = val;
}

void audio_process_hpf_create(int16_t *state_x, int16_t *state_y,
			      int16_t init_x, int16_t init_y,
			      int count_x, int count_y)
{
	set_w16(state_x, init_x, count_x);
	set_w16(state_y, init_y, count_y);
}

int audio_process_hpf_process(int16_t *state, int16_t *data, int num_samples)
{
	if (!state || !data)
		return -1;

	struct biquad *bq = (struct biquad *)state;

	if (bq->b0 == 0.0f) {
		float fs = (float)HPF_DEFAULT_FS;
		float fc = (float)HPF_CUTOFF_HZ;
		float w0 = 2.0f * (float)M_PI * fc / fs;
		float cw = cosf(w0);
		float alpha = sinf(w0) / (2.0f * 0.7071f);

		float a0_inv = 1.0f / (1.0f + alpha);
		bq->b0 = ((1.0f + cw) / 2.0f) * a0_inv;
		bq->b1 = -(1.0f + cw) * a0_inv;
		bq->b2 = bq->b0;
		bq->a1 = (-2.0f * cw) * a0_inv;
		bq->a2 = (1.0f - alpha) * a0_inv;
		bq->z1 = 0.0f;
		bq->z2 = 0.0f;
	}

	biquad_process_block_i16(bq, data, data, num_samples);

	return 0;
}

void audio_process_hpf_free(void)
{
}
