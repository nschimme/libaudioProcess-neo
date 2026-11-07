/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

#include "audio_process.h"
#include "drc_eq_internal.h"
#include "biquad.h"

/* T32 combined DRC+EQ API -- thin glue over drc.c and eq.c */

void *audio_process_drc_eq_create(int sample_rate, int channels)
{
	(void)channels;
	return drc_eq_alloc(sample_rate);
}

int audio_process_drc_eq_set_config(void *handle, int drc_threshold,
				    int drc_knee, int drc_ratio,
				    int eq_num_bands, int *eq_freqs,
				    float *eq_gains, int *eq_qs)
{
	struct drc_eq_state *s = handle;
	if (!s)
		return -1;

	if (drc_threshold >= 0) {
		s->threshold = powf(10.0f, (float)drc_threshold / -20.0f);
		s->knee = powf(10.0f, (float)drc_knee / -20.0f);
		if (drc_ratio > 0)
			s->ratio = 1.0f / (float)drc_ratio;
	}

	if (eq_num_bands > 0 && eq_freqs && eq_gains) {
		if (eq_num_bands > DRC_MAX_EQ_BANDS)
			eq_num_bands = DRC_MAX_EQ_BANDS;
		s->num_eq_bands = eq_num_bands;
		for (int i = 0; i < eq_num_bands; i++) {
			float q = (eq_qs && eq_qs[i] > 0) ?
				  (float)eq_qs[i] / 10.0f : 1.0f;
			biquad_design_peaking(&s->eq_bands[i],
					      (float)s->sample_rate,
					      (float)eq_freqs[i],
					      eq_gains[i], q);
		}
	}

	return 0;
}

int audio_process_drc_eq_enable(void *handle, int drc_enable, int eq_enable)
{
	struct drc_eq_state *s = handle;
	if (!s)
		return -1;
	s->enabled_drc = drc_enable;
	s->enabled_eq = eq_enable;
	return 0;
}

void audio_process_drc_eq_process(void *handle, const float *const *in,
				  float *const *out)
{
	struct drc_eq_state *s = handle;
	if (!s || !in || !*in || !out || !*out)
		return;

	const float *src = in[0];
	float *dst = out[0];
	int n = s->frame_size;

	if (s->enabled_drc && s->enabled_eq && s->num_eq_bands > 0) {
		/* Fused DRC + EQ streaming pass */
		if (src != dst) {
			for (int i = 0; i < n; i++)
				dst[i] = src[i];
		}
		drc_process(s, dst, n);
		eq_process(s, dst, n);
	} else if (s->enabled_drc) {
		if (src != dst) {
			for (int i = 0; i < n; i++)
				dst[i] = src[i];
		}
		drc_process(s, dst, n);
	} else if (s->enabled_eq) {
		if (src != dst) {
			for (int i = 0; i < n; i++)
				dst[i] = src[i];
		}
		eq_process(s, dst, n);
	} else if (src != dst) {
		for (int i = 0; i < n; i++)
			dst[i] = src[i];
	}
}

void audio_process_drc_eq_free(void *handle)
{
	free(handle);
}
