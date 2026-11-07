/* SPDX-License-Identifier: MIT */
#include "drc_eq_internal.h"
#include "biquad.h"

void eq_process(struct drc_eq_state *s, float *buf, int n)
{
	int num_bands = s->num_eq_bands;
	if (num_bands <= 0)
		return;

	/* If 1 band, directly process block */
	if (num_bands == 1) {
		biquad_process_block_f32(&s->eq_bands[0], buf, buf, n);
		return;
	}

	/* Process sample-by-sample with bands cached or block-by-block depending on size */
	if (n <= 256) {
		/* Small frames (e.g. 10ms frame_size = 160 samples): keep sample in register across bands */
		struct biquad *bands = s->eq_bands;
		for (int i = 0; i < n; i++) {
			float sample = buf[i];
			for (int b = 0; b < num_bands; b++) {
				struct biquad *bq = &bands[b];
				float out = bq->b0 * sample + bq->z1;
				bq->z1 = bq->b1 * sample - bq->a1 * out + bq->z2;
				bq->z2 = bq->b2 * sample - bq->a2 * out;
				sample = out;
			}
			buf[i] = sample;
		}
	} else {
		for (int b = 0; b < num_bands; b++) {
			biquad_process_block_f32(&s->eq_bands[b], buf, buf, n);
		}
	}
}
