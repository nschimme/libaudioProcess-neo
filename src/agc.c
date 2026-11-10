/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "audio_process.h"
#include "util.h"

#define kAgcModeAdaptiveDigital 2

/*
 * Digital AGC: a streaming peak-envelope follower with a smoothed gain, in
 * fixed point (the sample loop uses integers only, because the target SoCs
 * have slow or emulated floating point).
 *
 * Everything advances sample by sample, so the output does not depend on how
 * the stream is cut into calls. The gain is recomputed once per sub_len
 * samples (about a millisecond) and ramps linearly to its new value across
 * the next block, which keeps the trajectory smooth and free of steps.
 *
 *  - The target is a PEAK level, 32767 * 10^(-targetLevelDbfs / 20): a sine
 *    settles with its peak at -target dBFS, as the shipped WebRTC AGC does.
 *  - The gain starts at unity and is never above 10^(compressionGaindB / 20)
 *    (internally limited to 54 dB).
 *  - Gain is held (not raised) while the envelope is at the noise floor or
 *    without speech, and pauses get a slow -2.3 dB attenuation, so steady
 *    noise is never boosted.
 *  - With the limiter on, the output is bent smoothly below full scale; it
 *    never reaches 32767. With the limiter off it is clamped.
 *
 * Number formats: envelope, speech reference and floor are peak counts in
 * Q15; the block goal is Q16 and the running per-sample gain is Q22 (so the
 * gain is limited internally to 54 dB). The envelope coefficients are
 * powers of two, and the sample multiply stays within 32 bits up to 30 dB of
 * gain; above that a 64-bit multiply is used.
 */

#define AGC_ATTACK_MS    1.0f    /* envelope rises towards a louder sample */
#define AGC_RELEASE_MS   300.0f  /* envelope falls after a peak */
#define AGC_GAIN_DOWN_Q  19661   /* 0.30 of a gain reduction taken per block (Q16) */
#define AGC_GAIN_UP_Q    655     /* 0.01 of a gain increase per block (Q16) */
#define AGC_GAIN_IDLE_Q  131     /* 0.002 per block while easing back to unity (Q16) */
#define AGC_FLOOR_Q15    (32 << 15) /* envelope (peak counts) below which gain holds */
#define AGC_REF_DECAY_Q  21      /* speech reference falls by 21/65536 per block (about 3 s) */
#define AGC_FLOOR_RISE_Q 20      /* the noise floor may rise by 20/65536 per block (2.6 dB/s) */
#define AGC_FLOOR_ADD    328     /* plus 0.01 count in Q15 so it can leave zero */
#define AGC_SPEECH_RATIO 5       /* speech reference at least 5 times the floor = speech */
#define AGC_PAUSE_PCT    22      /* envelope below 22% of the reference = pause */
#define AGC_PAUSE_ATTEN_Q 50463  /* 0.77 (-2.3 dB) in Q16 */
#define AGC_PAUSE_IN_Q   655     /* 0.010 per block towards the pause factor */
#define AGC_PAUSE_OUT_Q  1311    /* 0.020 per block back to unity */
#define AGC_LIM_THR      29500
#define AGC_CHUNK        256
#define AGC_MAX_GAIN_Q16 (500 << 16)  /* 54 dB */

struct modern_agc {
	int min_level;
	int max_level;
	int mode;
	int sample_rate;
	WebRtcAgcConfig config;

	/* parameters derived from the configuration */
	int32_t target_peak;  /* peak count the gain steers towards */
	int32_t max_gain_q16; /* 10^(compressionGaindB / 20), at most 60 dB */
	int att_shift;        /* envelope attack: one share of 2^-att_shift per sample */
	int rel_shift;        /* envelope release: one share of 2^-rel_shift per sample */
	int sub_len;          /* samples per gain update, a power of two */
	int sub_shift;        /* log2(sub_len) */

	/* streaming state */
	int32_t env;          /* peak envelope, Q15 */
	int32_t gain;         /* gain applied to the next sample, Q22 */
	int32_t gain_step;    /* per-sample ramp towards the block goal, Q22 */
	int32_t gain_goal;    /* gain at the end of the current block, Q16 */
	int32_t speech_ref;   /* slow peak of the envelope, Q15 */
	int32_t floor_est;    /* envelope minimum, Q15 */
	int32_t pause;        /* extra gain factor in pauses (Q16, 65536 in speech) */
	int32_t pause_applied;/* the pause factor in force at the start of this block */
	int sub_pos;
};

static void agc_prepare(struct modern_agc *st)
{
	float tgt = 32767.0f * powf(10.0f, -(float)st->config.targetLevelDbfs / 20.0f);
	float mg = powf(10.0f, (float)st->config.compressionGaindB / 20.0f);
	float fs = (float)st->sample_rate;

	st->target_peak = (int32_t)(tgt + 0.5f);
	if (st->target_peak < 1)
		st->target_peak = 1;
	if (mg > 500.0f)
		mg = 500.0f;
	st->max_gain_q16 = (int32_t)(mg * 65536.0f);

	/* attack about 1 ms, release about 300 ms, rounded to powers of two */
	st->att_shift = 0;
	while ((float)(2 << st->att_shift) <= AGC_ATTACK_MS * fs / 1000.0f * 1.414f && st->att_shift < 16)
		st->att_shift++;
	st->rel_shift = 0;
	while ((float)(2 << st->rel_shift) <= AGC_RELEASE_MS * fs / 1000.0f * 1.414f && st->rel_shift < 24)
		st->rel_shift++;

	st->sub_shift = 0;
	while ((2 << st->sub_shift) <= st->sample_rate / 1000 && st->sub_shift < 10)
		st->sub_shift++;
	st->sub_len = 1 << st->sub_shift;

	st->env = 0;
	st->gain = 1 << 22;
	st->gain_goal = 1 << 16;
	st->gain_step = 0;
	st->speech_ref = 0;
	st->floor_est = 0;
	st->pause = 1 << 16;
	st->pause_applied = 1 << 16;
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

static inline int32_t agc_limit(int32_t y)
{
	const int32_t room = 32767 - AGC_LIM_THR;
	int32_t a = y < 0 ? -y : y;

	if (a > AGC_LIM_THR) {
		int32_t d = a - AGC_LIM_THR;
		if (d > (1 << 19))
			d = 1 << 19;
		a = AGC_LIM_THR + (room * d) / (room + d);
	}
	return y < 0 ? -a : a;
}

static inline int16_t agc_out(int32_t y, int limiter_on)
{
	if (limiter_on)
		y = agc_limit(y);
	return (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
}

/* gain ramp for the next block, from the state after the last sample */
static void agc_block_update(struct modern_agc *st)
{
	st->sub_pos = 0;

	/* speech reference (slow peak) and noise floor (minimum) */
	st->speech_ref -= (int32_t)(((int64_t)st->speech_ref * AGC_REF_DECAY_Q) >> 16);
	if (st->env > st->speech_ref)
		st->speech_ref = st->env;
	st->floor_est += (int32_t)(((int64_t)st->floor_est * AGC_FLOOR_RISE_Q) >> 16) + AGC_FLOOR_ADD;
	if (st->env < st->floor_est)
		st->floor_est = st->env;

	/* speech has a wide envelope range; steady noise does not */
	int speech = (int64_t)st->speech_ref > (int64_t)AGC_SPEECH_RATIO * st->floor_est;
	int pause = !speech || (int64_t)st->env * 100 < (int64_t)st->speech_ref * AGC_PAUSE_PCT;

	int32_t cur = st->gain_goal;
	int32_t desired = cur;
	if (st->env > AGC_FLOOR_Q15) {
		/* target / envelope in Q16, from the envelope in sixteenths of a count */
		uint32_t env16 = (uint32_t)(st->env >> 11);
		uint32_t d = ((uint32_t)st->target_peak << 16) / env16; /* Q12 */
		desired = d > (uint32_t)(st->max_gain_q16 >> 4) ? st->max_gain_q16 : (int32_t)(d << 4);
		/* never raise the gain in a pause: that lifts the noise */
		if (pause && desired > cur)
			desired = cur;
	}
	/* without speech nothing is boosted: drift back to unity */
	if (!speech && desired > (1 << 16))
		desired = 1 << 16;

	int32_t coef = desired < cur ? AGC_GAIN_DOWN_Q : AGC_GAIN_UP_Q;
	if (!speech && desired < cur &&
	    (int64_t)st->env * cur <= ((int64_t)st->target_peak << 31))
		coef = AGC_GAIN_IDLE_Q; /* easing back to unity must not step */
	int32_t goal = cur + (int32_t)(((int64_t)(desired - cur) * coef) >> 16);

	/* slow extra attenuation in pauses, quick recovery in speech */
	if (pause)
		st->pause += (int32_t)(((int64_t)(AGC_PAUSE_ATTEN_Q - st->pause) * AGC_PAUSE_IN_Q) >> 16);
	else
		st->pause += (int32_t)(((int64_t)((1 << 16) - st->pause) * AGC_PAUSE_OUT_Q) >> 16);

	int32_t start = (int32_t)(((int64_t)cur * st->pause_applied) >> 10); /* Q22 */
	int32_t end = (int32_t)(((int64_t)goal * st->pause) >> 10);          /* Q22 */
	st->gain_goal = goal;
	st->gain = start;
	st->gain_step = (end - start) >> st->sub_shift;
	st->pause_applied = st->pause;
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
	int32_t gains[AGC_CHUNK];

	for (size_t base = 0; base < samples; base += AGC_CHUNK) {
		size_t n = samples - base;
		if (n > AGC_CHUNK)
			n = AGC_CHUNK;
		const int16_t *s_in = in_near[0] + base;
		int16_t *s_out = out[0] + base;

		for (size_t i = 0; i < n; i++) {
			int32_t x = s_in[i];
			int32_t a = (x < 0 ? -x : x) << 15;

			if (a > st->env)
				st->env += (a - st->env) >> st->att_shift;
			else
				st->env -= st->env >> st->rel_shift;

			int32_t g = st->gain >> 6; /* Q16 for the band copies */
			gains[i] = g;
			int32_t g11 = st->gain >> 11;
			st->gain += st->gain_step;

			int32_t y;
			if (g11 <= 65535)
				y = (x * g11 + 1024) >> 11; /* fits 32 bits up to 30 dB */
			else
				y = (int32_t)(((int64_t)x * g11 + 1024) >> 11);
			if (y > 32000 || y < -32000)
				saturated = 1;
			s_out[i] = agc_out(y, limiter_on);

			if (++st->sub_pos == st->sub_len)
				agc_block_update(st);
		}

		/* further bands follow the same gain trajectory */
		for (size_t b = 1; b < num_bands; b++) {
			const int16_t *bi = in_near[b] + base;
			int16_t *bo = out[b] + base;
			for (size_t i = 0; i < n; i++) {
				int32_t y = (int32_t)(((int64_t)bi[i] * gains[i] + 32768) >> 16);
				bo[i] = agc_out(y, limiter_on);
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
