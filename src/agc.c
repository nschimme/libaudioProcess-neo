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

	/* Precomputed gain parameters */
	float target_linear;     /* Linear target level: (32767/sqrt(2)) * 10^(-targetDbfs/20) */
	float max_gain_linear;   /* Linear max boost: 10^(compressionGaindB/20) */

	/* Adaptive envelope & gain tracking state */
	float current_gain;      /* Current linear gain factor */
	float energy_env;        /* Smoothed signal RMS energy */
	float peak_env;          /* Fast peak tracking */
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
	st->config.targetLevelDbfs = 3;   /* -3 dBFS */
	st->config.compressionGaindB = 9; /* 9 dB boost */
	st->config.limiterEnable = 1;     /* Limiter ON */

	st->target_linear = (32767.0f / sqrtf(2.0f)) * powf(10.0f, -3.0f / 20.0f);
	st->max_gain_linear = powf(10.0f, 9.0f / 20.0f);
	st->current_gain = 1.0f;
	st->energy_env = 0.0f;
	st->peak_env = 0.0f;

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

	/* Target linear RMS amplitude matching oracle */
	st->target_linear = (32767.0f / sqrtf(2.0f)) * powf(10.0f, -(float)st->config.targetLevelDbfs / 20.0f);
	st->max_gain_linear = powf(10.0f, (float)st->config.compressionGaindB / 20.0f);

	return 0;
}

void audio_process_agc_get_config(void *agc, WebRtcAgcConfig *out)
{
	struct modern_agc *st = (struct modern_agc *)agc;
	if (st && out)
		*out = st->config;
}

static inline int16_t soft_limiter_fast(int32_t val)
{
	const int32_t thr = 27000;
	if (val > thr) {
		int32_t diff = val - thr;
		val = thr + (diff >> 2);
		if (val > 32767) val = 32767;
	} else if (val < -thr) {
		int32_t diff = -val - thr;
		val = -(thr + (diff >> 2));
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

	const int16_t *__restrict__ in = in_near[0];
	int16_t *__restrict__ out_buf = out[0];

	/* Fast 4x unrolled integer energy and peak estimation */
	int64_t sum_sq = 0;
	int16_t frame_peak = 0;
	size_t i = 0;
	for (; i + 3 < samples; i += 4) {
		int32_t s0 = in[i];
		int32_t s1 = in[i + 1];
		int32_t s2 = in[i + 2];
		int32_t s3 = in[i + 3];
		sum_sq += s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3;

		int16_t a0 = (s0 >= 0) ? (int16_t)s0 : (int16_t)(-s0);
		int16_t a1 = (s1 >= 0) ? (int16_t)s1 : (int16_t)(-s1);
		int16_t a2 = (s2 >= 0) ? (int16_t)s2 : (int16_t)(-s2);
		int16_t a3 = (s3 >= 0) ? (int16_t)s3 : (int16_t)(-s3);
		if (a0 > frame_peak) frame_peak = a0;
		if (a1 > frame_peak) frame_peak = a1;
		if (a2 > frame_peak) frame_peak = a2;
		if (a3 > frame_peak) frame_peak = a3;
	}
	for (; i < samples; i++) {
		int32_t s = in[i];
		sum_sq += s * s;
		int16_t abs_s = (s >= 0) ? (int16_t)s : (int16_t)(-s);
		if (abs_s > frame_peak) frame_peak = abs_s;
	}

	float frame_rms = sqrtf((float)sum_sq / (float)samples);

	/* Dual envelope update (attack 10ms, release 300ms) */
	float alpha_rms = (frame_rms > st->energy_env) ? 0.35f : 0.035f;
	st->energy_env += alpha_rms * (frame_rms - st->energy_env);

	float f_peak = (float)frame_peak;
	float alpha_peak = (f_peak > st->peak_env) ? 0.5f : 0.05f;
	st->peak_env += alpha_peak * (f_peak - st->peak_env);

	/* Compute target gain based on tracked RMS level */
	float desired_gain = st->max_gain_linear;
	if (st->energy_env > 10.0f) {
		float calc_gain = st->target_linear / st->energy_env;
		desired_gain = AGC_MIN(st->max_gain_linear, calc_gain);
	}

	/* Peak ceiling protection: ensure peak * gain does not blow past full scale */
	if (st->peak_env > 10.0f && st->config.limiterEnable) {
		float peak_gain_cap = 30000.0f / st->peak_env;
		if (peak_gain_cap < desired_gain)
			desired_gain = peak_gain_cap;
	}

	/* Prevent digital expansion on absolute noise floor */
	if (st->energy_env < 30.0f) {
		desired_gain = AGC_MIN(desired_gain, 1.0f);
	}

	/* Gain smoothing across frames */
	float alpha_gain = (desired_gain < st->current_gain) ? 0.25f : 0.05f;
	float start_gain = st->current_gain;
	float end_gain = start_gain + alpha_gain * (desired_gain - start_gain);
	st->current_gain = end_gain;

	/* Fixed-point Q16 gain interpolation */
	int32_t g_q16 = (int32_t)(start_gain * 65536.0f);
	int32_t end_g_q16 = (int32_t)(end_gain * 65536.0f);
	int32_t step_q16 = (end_g_q16 - g_q16) / (int32_t)samples;

	int limiter_on = st->config.limiterEnable;
	uint8_t saturated = 0;

	if (limiter_on) {
		i = 0;
		for (; i + 3 < samples; i += 4) {
			g_q16 += step_q16;
			int32_t y0 = (int32_t)(((int64_t)in[i] * g_q16) >> 16);
			out_buf[i] = soft_limiter_fast(y0);

			g_q16 += step_q16;
			int32_t y1 = (int32_t)(((int64_t)in[i + 1] * g_q16) >> 16);
			out_buf[i + 1] = soft_limiter_fast(y1);

			g_q16 += step_q16;
			int32_t y2 = (int32_t)(((int64_t)in[i + 2] * g_q16) >> 16);
			out_buf[i + 2] = soft_limiter_fast(y2);

			g_q16 += step_q16;
			int32_t y3 = (int32_t)(((int64_t)in[i + 3] * g_q16) >> 16);
			out_buf[i + 3] = soft_limiter_fast(y3);

			if (y0 > 32000 || y0 < -32000 || y1 > 32000 || y1 < -32000 ||
			    y2 > 32000 || y2 < -32000 || y3 > 32000 || y3 < -32000) {
				saturated = 1;
			}
		}
		for (; i < samples; i++) {
			g_q16 += step_q16;
			int32_t y = (int32_t)(((int64_t)in[i] * g_q16) >> 16);
			if (y > 32000 || y < -32000)
				saturated = 1;
			out_buf[i] = soft_limiter_fast(y);
		}
	} else {
		i = 0;
		for (; i + 3 < samples; i += 4) {
			g_q16 += step_q16;
			int32_t y0 = (int32_t)(((int64_t)in[i] * g_q16) >> 16);
			out_buf[i] = clamp_i16(y0);

			g_q16 += step_q16;
			int32_t y1 = (int32_t)(((int64_t)in[i + 1] * g_q16) >> 16);
			out_buf[i + 1] = clamp_i16(y1);

			g_q16 += step_q16;
			int32_t y2 = (int32_t)(((int64_t)in[i + 2] * g_q16) >> 16);
			out_buf[i + 2] = clamp_i16(y2);

			g_q16 += step_q16;
			int32_t y3 = (int32_t)(((int64_t)in[i + 3] * g_q16) >> 16);
			out_buf[i + 3] = clamp_i16(y3);

			if (y0 > 32767 || y0 < -32768 || y1 > 32767 || y1 < -32768 ||
			    y2 > 32767 || y2 < -32768 || y3 > 32767 || y3 < -32768) {
				saturated = 1;
			}
		}
		for (; i < samples; i++) {
			g_q16 += step_q16;
			int32_t y = (int32_t)(((int64_t)in[i] * g_q16) >> 16);
			if (y > 32767 || y < -32768)
				saturated = 1;
			out_buf[i] = clamp_i16(y);
		}
	}

	/* Process any additional high-frequency bands with the same gain trajectory */
	for (size_t b = 1; b < num_bands; b++) {
		const int16_t *__restrict__ in_b = in_near[b];
		int16_t *__restrict__ out_b = out[b];
		int32_t gb_q16 = (int32_t)(start_gain * 65536.0f);
		for (size_t j = 0; j < samples; j++) {
			gb_q16 += step_q16;
			int32_t y = (int32_t)(((int64_t)in_b[j] * gb_q16) >> 16);
			out_b[j] = clamp_i16(y);
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
