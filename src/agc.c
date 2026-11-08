/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "audio_process.h"
#include "util.h"
#include "webrtc/modules/audio_processing/agc/legacy/gain_control.h"

#define AGC_MIN(a, b) ((a) < (b) ? (a) : (b))
#define AGC_MAX(a, b) ((a) > (b) ? (a) : (b))

struct modern_agc {
	int min_level;
	int max_level;
	int mode;
	int sample_rate;
	WebRtcAgcConfig config;

	/* Precomputed gain parameters in Q12 */
	int32_t target_lvl_q12;     /* Linear target peak in Q12 */
	int32_t max_gain_q12;       /* Linear max boost in Q12: 10^(compressionGaindB/20) * 4096 */

	/* Adaptive envelope & gain tracking state */
	int32_t current_gain_q12;   /* Current linear gain factor in Q12 */
	int32_t env_level;          /* Smoothed subframe peak envelope */
};

void *audio_process_agc_create(void)
{
	struct modern_agc *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	st->min_level = 0;
	st->max_level = 255;
	st->mode = kAgcModeAdaptiveDigital;
	st->sample_rate = 16000;
	st->config.targetLevelDbfs = 3;   /* -3 dBov */
	st->config.compressionGaindB = 9; /* 9 dB boost */
	st->config.limiterEnable = 1;     /* Limiter ON */

	float target_lin = (32767.0f / sqrtf(2.0f)) * powf(10.0f, -3.0f / 20.0f);
	st->target_lvl_q12 = (int32_t)target_lin;
	st->max_gain_q12 = (int32_t)(powf(10.0f, 9.0f / 20.0f) * 4096.0f);
	st->current_gain_q12 = st->max_gain_q12;
	st->env_level = 0;

	return st;
}

int audio_process_agc_set_config(void *agc, int min_level, int max_level,
				 int mode, int sample_rate,
				 WebRtcAgcConfig config)
{
	struct modern_agc *st = (struct modern_agc *)agc;
	if (!st)
		return -1;

	st->min_level = min_level;
	st->max_level = max_level;
	st->mode = mode;
	st->sample_rate = sample_rate > 0 ? sample_rate : 16000;
	st->config = config;

	/* Validate parameters */
	if (st->config.targetLevelDbfs < 0)
		st->config.targetLevelDbfs = 0;
	if (st->config.targetLevelDbfs > 31)
		st->config.targetLevelDbfs = 31;
	if (st->config.compressionGaindB < 0)
		st->config.compressionGaindB = 0;
	if (st->config.compressionGaindB > 90)
		st->config.compressionGaindB = 90;

	float target_lin = (32767.0f / sqrtf(2.0f)) * powf(10.0f, -(float)st->config.targetLevelDbfs / 20.0f);
	st->target_lvl_q12 = (int32_t)target_lin;
	st->max_gain_q12 = (int32_t)(powf(10.0f, (float)st->config.compressionGaindB / 20.0f) * 4096.0f);
	st->current_gain_q12 = st->max_gain_q12;

	return 0;
}

void audio_process_agc_get_config(void *agc, WebRtcAgcConfig *out)
{
	struct modern_agc *st = (struct modern_agc *)agc;
	if (st && out)
		*out = st->config;
}

static inline int16_t soft_limiter_smooth(int32_t val)
{
	const int32_t thr = 32000;
	if (val > thr) {
		val = thr + ((val - thr) >> 3);
		if (val > 32767) val = 32767;
	} else if (val < -thr) {
		val = -(thr + ((-val - thr) >> 3));
		if (val < -32768) val = -32768;
	}
	return (int16_t)val;
}

int audio_process_agc_process(void *agc, const int16_t *const *in_near,
			      size_t num_bands, size_t samples,
			      int16_t *const *out, int32_t in_mic_level,
			      int32_t *out_mic_level, int16_t echo,
			      uint8_t *saturation_warning)
{
	(void)echo;
	struct modern_agc *st = (struct modern_agc *)agc;
	if (!st || !in_near || !*in_near || !out || !*out || samples == 0)
		return -1;

	if (out_mic_level)
		*out_mic_level = in_mic_level;
	if (saturation_warning)
		*saturation_warning = 0;

	const int16_t *__restrict__ p_in = in_near[0];
	int16_t *__restrict__ p_out = out[0];

	/* Divide 10ms frame into 1ms subframes (16 samples at 16kHz, L = sample_rate / 1000) */
	size_t sub_len = samples / 10;
	if (sub_len < 4) sub_len = samples;

	size_t num_subs = samples / sub_len;
	int limiter_on = st->config.limiterEnable;
	int32_t max_g = st->max_gain_q12;
	int32_t target_peak = st->target_lvl_q12;
	uint8_t saturated = 0;

	for (size_t sub = 0; sub < num_subs; sub++) {
		const int16_t *s_in = p_in + sub * sub_len;
		int16_t *s_out = p_out + sub * sub_len;

		/* Measure subframe peak */
		int32_t peak = 0;
		size_t j = 0;
		for (; j + 3 < sub_len; j += 4) {
			int32_t a0 = abs(s_in[j]);
			int32_t a1 = abs(s_in[j+1]);
			int32_t a2 = abs(s_in[j+2]);
			int32_t a3 = abs(s_in[j+3]);
			if (a0 > peak) peak = a0;
			if (a1 > peak) peak = a1;
			if (a2 > peak) peak = a2;
			if (a3 > peak) peak = a3;
		}
		for (; j < sub_len; j++) {
			int32_t a = abs(s_in[j]);
			if (a > peak) peak = a;
		}

		/* Asymmetric envelope smoothing */
		if (peak > st->env_level)
			st->env_level += (peak - st->env_level) >> 2; /* Fast 1ms attack */
		else
			st->env_level += (peak - st->env_level) >> 11; /* Smooth release */

		/* Subframe gain mapping */
		int32_t desired_g = max_g;
		if (st->env_level > 500) {
			int32_t g = (target_peak << 12) / st->env_level;
			if (g < desired_g) desired_g = g;
		} else if (st->env_level < 50) {
			desired_g = AGC_MIN(desired_g, 4096);
		}

		/* Smooth gain transitions across 1ms subframes */
		st->current_gain_q12 += (desired_g - st->current_gain_q12) >> 3;
		int32_t g = st->current_gain_q12;

		/* Apply gain with soft limiter */
		if (limiter_on) {
			size_t k = 0;
			for (; k + 3 < sub_len; k += 4) {
				int32_t y0 = ((int32_t)s_in[k] * g) >> 12;
				int32_t y1 = ((int32_t)s_in[k+1] * g) >> 12;
				int32_t y2 = ((int32_t)s_in[k+2] * g) >> 12;
				int32_t y3 = ((int32_t)s_in[k+3] * g) >> 12;

				s_out[k] = soft_limiter_smooth(y0);
				s_out[k+1] = soft_limiter_smooth(y1);
				s_out[k+2] = soft_limiter_smooth(y2);
				s_out[k+3] = soft_limiter_smooth(y3);

				if (y0 > 32000 || y0 < -32000 || y1 > 32000 || y1 < -32000 ||
				    y2 > 32000 || y2 < -32000 || y3 > 32000 || y3 < -32000)
					saturated = 1;
			}
			for (; k < sub_len; k++) {
				int32_t y = ((int32_t)s_in[k] * g) >> 12;
				if (y > 32000 || y < -32000) saturated = 1;
				s_out[k] = soft_limiter_smooth(y);
			}
		} else {
			size_t k = 0;
			for (; k + 3 < sub_len; k += 4) {
				s_out[k] = clamp_i16(((int32_t)s_in[k] * g) >> 12);
				s_out[k+1] = clamp_i16(((int32_t)s_in[k+1] * g) >> 12);
				s_out[k+2] = clamp_i16(((int32_t)s_in[k+2] * g) >> 12);
				s_out[k+3] = clamp_i16(((int32_t)s_in[k+3] * g) >> 12);
			}
			for (; k < sub_len; k++) {
				s_out[k] = clamp_i16(((int32_t)s_in[k] * g) >> 12);
			}
		}
	}

	/* Process any additional high-frequency bands with the same gain trajectory */
	for (size_t b = 1; b < num_bands; b++) {
		const int16_t *__restrict__ in_b = in_near[b];
		int16_t *__restrict__ out_b = out[b];
		int32_t g = st->current_gain_q12;
		for (size_t j = 0; j < samples; j++) {
			out_b[j] = clamp_i16(((int32_t)in_b[j] * g) >> 12);
		}
	}

	if (saturation_warning && saturated)
		*saturation_warning = 1;

	return 0;
}

int audio_process_agc_free(void *agc)
{
	if (!agc)
		return -1;
	free(agc);
	return 0;
}

/*
 * Compatibility aliases for internal WebRtcAgc_* calls
 */
void *WebRtcAgc_Create(void)
{
	return audio_process_agc_create();
}

void WebRtcAgc_Free(void *agcInst)
{
	audio_process_agc_free(agcInst);
}

int WebRtcAgc_Init(void *agcInst, int32_t minLevel, int32_t maxLevel,
		   int16_t agcMode, uint32_t fs)
{
	struct modern_agc *st = (struct modern_agc *)agcInst;
	if (!st)
		return -1;
	return audio_process_agc_set_config(st, minLevel, maxLevel, agcMode, (int)fs, st->config);
}

int WebRtcAgc_set_config(void *agcInst, WebRtcAgcConfig config)
{
	struct modern_agc *st = (struct modern_agc *)agcInst;
	if (!st)
		return -1;
	return audio_process_agc_set_config(st, st->min_level, st->max_level, st->mode, st->sample_rate, config);
}

int WebRtcAgc_get_config(void *agcInst, WebRtcAgcConfig *config)
{
	struct modern_agc *st = (struct modern_agc *)agcInst;
	if (!st || !config)
		return -1;
	audio_process_agc_get_config(st, config);
	return 0;
}

int WebRtcAgc_Process(void *agcInst, const int16_t *const *inNear,
		      size_t num_bands, size_t samples,
		      int16_t *const *out, int32_t inMicLevel,
		      int32_t *outMicLevel, int16_t echo,
		      uint8_t *saturationWarning)
{
	return audio_process_agc_process(agcInst, inNear, num_bands, samples, out,
					 inMicLevel, outMicLevel, echo, saturationWarning);
}

int WebRtcAgc_AddMic(void *state, int16_t *const *in_mic, size_t num_bands, size_t samples)
{
	(void)state; (void)in_mic; (void)num_bands; (void)samples;
	return 0;
}

int WebRtcAgc_AddFarend(void *state, const int16_t *in_far, size_t samples)
{
	(void)state; (void)in_far; (void)samples;
	return 0;
}

int WebRtcAgc_VirtualMic(void *agcInst, int16_t *const *in_near,
			 size_t num_bands, size_t samples,
			 int32_t micLevelIn, int32_t *micLevelOut)
{
	(void)agcInst; (void)in_near; (void)num_bands; (void)samples;
	if (micLevelOut)
		*micLevelOut = micLevelIn;
	return 0;
}
