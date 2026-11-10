/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "audio_process.h"
#include "util.h"

#define kAgcModeAdaptiveDigital 2

/*
 * Digital AGC: a streaming peak-envelope follower with a smoothed gain.
 *
 * Everything advances sample by sample, so the output does not depend on how
 * the stream is cut into calls. The gain is recomputed once per millisecond
 * (sub_len samples) and ramps linearly to its new value across the next
 * millisecond, which keeps the gain trajectory smooth and free of steps.
 *
 *  - The target is a PEAK level, 32767 * 10^(-targetLevelDbfs / 20): a sine
 *    settles with its peak at -target dBFS, as the shipped WebRTC AGC does.
 *  - The gain starts at unity and is never above 10^(compressionGaindB / 20).
 *  - Gain is held (not raised) while the envelope is at the noise floor, so a
 *    pause does not pump the background noise up.
 *  - With the limiter on, the output is bent smoothly below full scale; it
 *    never reaches 32767. With the limiter off it is clamped.
 */

#define AGC_ATTACK_MS   1.0f   /* envelope rises towards a louder sample */
#define AGC_RELEASE_MS  300.0f /* envelope falls after a peak */
#define AGC_GAIN_DOWN   0.30f  /* share of a gain reduction taken per ms */
#define AGC_GAIN_UP     0.01f  /* share of a gain increase taken per ms */
#define AGC_FLOOR       32.0f  /* envelope (peak counts) below which gain holds */
#define AGC_REF_DECAY   0.99967f /* per ms: speech reference falls over about 3 s */
#define AGC_PAUSE_RATIO 0.22f  /* envelope below this share of the reference = pause */
#define AGC_PAUSE_ATTEN 0.77f  /* gain factor reached in a long pause (-2.3 dB) */
#define AGC_PAUSE_IN    0.010f /* share of the way to the pause factor per ms */
#define AGC_PAUSE_OUT   0.02f  /* share of the way back to unity per ms */
#define AGC_LIM_THR     29500.0f
#define AGC_CHUNK       256

struct modern_agc {
	int min_level;
	int max_level;
	int mode;
	int sample_rate;
	WebRtcAgcConfig config;

	/* parameters derived from the configuration */
	float target_peak;   /* peak level the gain steers towards */
	float max_gain;      /* 10^(compressionGaindB / 20) */
	float att;           /* per-sample envelope attack coefficient */
	float rel;           /* per-sample envelope release multiplier */
	int sub_len;         /* samples per gain update (1 ms) */

	/* streaming state */
	float env;           /* peak envelope */
	float gain;          /* gain applied to the next sample */
	float gain_step;     /* per-sample ramp towards the current goal */
	float gain_goal;     /* gain at the end of the current millisecond */
	float speech_ref;    /* slow peak of the envelope: what speech looks like */
	float pause;         /* extra gain factor in pauses, 1 in speech */
	float pause_applied; /* the pause factor in force at the start of this ms */
	int sub_pos;
};

static void agc_prepare(struct modern_agc *st)
{
	st->target_peak = 32767.0f * powf(10.0f, -(float)st->config.targetLevelDbfs / 20.0f);
	st->max_gain = powf(10.0f, (float)st->config.compressionGaindB / 20.0f);
	float fs = (float)st->sample_rate;
	st->att = 1.0f - expf(-1000.0f / (AGC_ATTACK_MS * fs));
	st->rel = expf(-1000.0f / (AGC_RELEASE_MS * fs));
	st->sub_len = st->sample_rate / 1000;
	if (st->sub_len < 1)
		st->sub_len = 1;
	st->env = 0.0f;
	st->gain = 1.0f;
	st->gain_goal = 1.0f;
	st->gain_step = 0.0f;
	st->speech_ref = 0.0f;
	st->pause = 1.0f;
	st->pause_applied = 1.0f;
	st->sub_pos = 0;
}

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
	agc_prepare(st);

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

	agc_prepare(st);
	return 0;
}

void audio_process_agc_get_config(void *agc, WebRtcAgcConfig *out)
{
	struct modern_agc *st = (struct modern_agc *)agc;
	if (st && out)
		*out = st->config;
}

static inline float agc_limit(float y)
{
	const float room = 32767.0f - AGC_LIM_THR;
	float a = fabsf(y);

	if (a > AGC_LIM_THR) {
		float d = a - AGC_LIM_THR;
		a = AGC_LIM_THR + room * d / (room + d);
	}
	return y < 0.0f ? -a : a;
}

static inline int16_t agc_round(float y)
{
	return clamp_i16(y < 0.0f ? y - 0.5f : y + 0.5f);
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

	const int limiter_on = st->config.limiterEnable;
	uint8_t saturated = 0;
	float gains[AGC_CHUNK];

	for (size_t base = 0; base < samples; base += AGC_CHUNK) {
		size_t n = samples - base;
		if (n > AGC_CHUNK)
			n = AGC_CHUNK;
		const int16_t *s_in = in_near[0] + base;
		int16_t *s_out = out[0] + base;

		for (size_t i = 0; i < n; i++) {
			float x = (float)s_in[i];
			float a = fabsf(x);

			if (a > st->env)
				st->env += (a - st->env) * st->att;
			else
				st->env *= st->rel;

			float g = st->gain;
			gains[i] = g;
			st->gain = g + st->gain_step;

			float y = x * g;
			if (fabsf(y) > 32000.0f)
				saturated = 1;
			s_out[i] = agc_round(limiter_on ? agc_limit(y) : y);

			if (++st->sub_pos == st->sub_len) {
				st->sub_pos = 0;

				st->speech_ref *= AGC_REF_DECAY;
				if (st->env > st->speech_ref)
					st->speech_ref = st->env;
				int pause = st->env < AGC_PAUSE_RATIO * st->speech_ref;

				float cur = st->gain_goal;
				float desired = cur;
				if (st->env > AGC_FLOOR) {
					desired = st->target_peak / st->env;
					if (desired > st->max_gain)
						desired = st->max_gain;
					/* never raise the gain in a pause: that lifts the noise */
					if (pause && desired > cur)
						desired = cur;
				}
				float coef = desired < cur ? AGC_GAIN_DOWN : AGC_GAIN_UP;
				float goal = cur + (desired - cur) * coef;

				/* slow extra attenuation in pauses, quick recovery in speech */
				if (pause)
					st->pause += (AGC_PAUSE_ATTEN - st->pause) * AGC_PAUSE_IN;
				else
					st->pause += (1.0f - st->pause) * AGC_PAUSE_OUT;

				st->gain_goal = goal;
				st->gain = cur * st->pause_applied;
				st->gain_step = (goal * st->pause - st->gain) / (float)st->sub_len;
				st->pause_applied = st->pause;
			}
		}

		/* further bands follow the same gain trajectory */
		for (size_t b = 1; b < num_bands; b++) {
			const int16_t *bi = in_near[b] + base;
			int16_t *bo = out[b] + base;
			for (size_t i = 0; i < n; i++) {
				float y = (float)bi[i] * gains[i];
				bo[i] = agc_round(limiter_on ? agc_limit(y) : y);
			}
		}
	}

	if (saturation_warning)
		*saturation_warning = saturated;

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
