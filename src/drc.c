/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

#include "audio_process.h"
#include "drc_eq_internal.h"
#include "util.h"

static inline float drc_gain_inline(float threshold, float knee, float ratio,
				    float knee_range_inv, float level)
{
	if (level <= threshold || level < 1e-10f)
		return 1.0f;

	float target;
	if (level <= knee) {
		float x = (level - threshold) * knee_range_inv;
		target = threshold + (knee - threshold) * x * ratio;
	} else {
		target = threshold + (level - threshold) * ratio;
	}

	return target / level;
}

float drc_compute_gain(struct drc_eq_state *s, float level)
{
	float range = s->knee - s->threshold;
	float range_inv = range > 1e-10f ? (1.0f / range) : 0.0f;
	return drc_gain_inline(s->threshold, s->knee, s->ratio, range_inv, level);
}

void drc_process(struct drc_eq_state *s, float *buf, int n)
{
	const float inv_scale = 1.0f / 32768.0f;
	const float attack_coeff = s->attack_coeff;
	const float release_coeff = s->release_coeff;
	const float threshold = s->threshold;
	const float knee = s->knee;
	const float ratio = s->ratio;
	float range = knee - threshold;
	float range_inv = range > 1e-10f ? (1.0f / range) : 0.0f;
	float env = s->envelope;

	float *p = buf;
	for (int i = 0; i < n; i++) {
		float val = *p;
		float level = fabsf(val) * inv_scale;
		float coeff = (level > env) ? attack_coeff : release_coeff;
		env += coeff * (level - env);
		*p++ = val * drc_gain_inline(threshold, knee, ratio, range_inv, env);
	}
	s->envelope = env;
}

struct drc_eq_state *drc_eq_alloc(int sample_rate)
{
	struct drc_eq_state *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;

	s->sample_rate = sample_rate > 0 ? sample_rate : 16000;
	s->frame_size = s->sample_rate / 100;
	s->threshold = 0.5f;
	s->knee = 0.7f;
	s->ratio = 0.5f;

	float attack_ms = 5.0f;
	float release_ms = 50.0f;
	s->attack_coeff = 1.0f - expf(-1000.0f /
				      ((float)s->sample_rate * attack_ms));
	s->release_coeff = 1.0f - expf(-1000.0f /
				       ((float)s->sample_rate * release_ms));
	return s;
}

/* T41 ZRT API */

void *audio_process_drc_create(int sample_rate, int channels)
{
	(void)channels;
	struct drc_eq_state *s = drc_eq_alloc(sample_rate);
	if (s)
		s->enabled_drc = 1;
	return s;
}

int audio_process_drc_set_config(void *handle, int threshold, int knee,
				 int ratio, int makeup)
{
	struct drc_eq_state *s = handle;
	if (!s)
		return -1;

	(void)makeup;
	s->threshold = powf(10.0f, (float)threshold / -20.0f);
	s->knee = powf(10.0f, (float)knee / -20.0f);
	if (ratio > 0)
		s->ratio = 1.0f / (float)ratio;
	return 0;
}

void audio_process_drc_process(void *handle, int16_t *data, int num_samples)
{
	struct drc_eq_state *s = handle;
	if (!s || !data || num_samples <= 0)
		return;

	const float inv_scale = 1.0f / 32768.0f;
	const float attack_coeff = s->attack_coeff;
	const float release_coeff = s->release_coeff;
	const float threshold = s->threshold;
	const float knee = s->knee;
	const float ratio = s->ratio;
	float range = knee - threshold;
	float range_inv = range > 1e-10f ? (1.0f / range) : 0.0f;
	float env = s->envelope;

	int16_t *p = data;
	for (int i = 0; i < num_samples; i++) {
		float sample = (float)*p;
		float level = fabsf(sample) * inv_scale;
		float coeff = (level > env) ? attack_coeff : release_coeff;
		env += coeff * (level - env);
		sample *= drc_gain_inline(threshold, knee, ratio, range_inv, env);
		*p++ = clamp_i16(sample);
	}
	s->envelope = env;
}

void audio_process_drc_free(void *handle)
{
	free(handle);
}
