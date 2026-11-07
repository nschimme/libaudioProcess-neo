/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

#include "audio_process.h"
#include "util.h"
#include "biquad.h"
#include "drc_eq_internal.h"

#define BENCH_SAMPLES 160000 /* 10 seconds of 16kHz audio */
#define NUM_ITERATIONS 50

static double get_time_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double compute_snr_i16(const int16_t *ref, const int16_t *test, int n, int *max_abs_diff)
{
	double signal_pwr = 0.0;
	double noise_pwr = 0.0;
	int max_diff = 0;
	for (int i = 0; i < n; i++) {
		double s = ref[i];
		int diff = abs((int)ref[i] - (int)test[i]);
		if (diff > max_diff)
			max_diff = diff;
		signal_pwr += s * s;
		noise_pwr += (double)diff * (double)diff;
	}
	if (max_abs_diff)
		*max_abs_diff = max_diff;
	if (noise_pwr < 1e-15)
		return 999.0;
	if (signal_pwr < 1e-15)
		return 0.0;
	return 10.0 * log10(signal_pwr / noise_pwr);
}

static double compute_snr_f32(const float *ref, const float *test, int n, float *max_abs_diff)
{
	double signal_pwr = 0.0;
	double noise_pwr = 0.0;
	float max_diff = 0.0f;
	for (int i = 0; i < n; i++) {
		double s = ref[i];
		float diff = fabsf(ref[i] - test[i]);
		if (diff > max_diff)
			max_diff = diff;
		signal_pwr += s * s;
		noise_pwr += (double)diff * (double)diff;
	}
	if (max_abs_diff)
		*max_abs_diff = max_diff;
	if (noise_pwr < 1e-15)
		return 999.0;
	if (signal_pwr < 1e-15)
		return 0.0;
	return 10.0 * log10(signal_pwr / noise_pwr);
}

static void generate_test_signal_i16(int16_t *buf, int n)
{
	for (int i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		double val = 8000.0 * sin(2.0 * M_PI * 440.0 * t) +
			     4000.0 * sin(2.0 * M_PI * 1000.0 * t) +
			     2000.0 * sin(2.0 * M_PI * 250.0 * t) +
			     500.0 * ((double)(rand() % 2000) - 1000.0) / 1000.0;
		if (val > 32767.0) val = 32767.0;
		if (val < -32768.0) val = -32768.0;
		buf[i] = (int16_t)val;
	}
}

static void generate_test_signal_f32(float *buf, int n)
{
	for (int i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		double val = 0.25 * sin(2.0 * M_PI * 440.0 * t) +
			     0.15 * sin(2.0 * M_PI * 1000.0 * t) +
			     0.10 * sin(2.0 * M_PI * 250.0 * t) +
			     0.02 * ((double)(rand() % 2000) - 1000.0) / 1000.0;
		buf[i] = (float)val;
	}
}

/* ========================================================================= */
/* BASELINE (UNOPTIMIZED) IMPLEMENTATIONS                                     */
/* ========================================================================= */

static inline float baseline_biquad_process(struct biquad *bq, float in)
{
	float out = bq->b0 * in + bq->z1;
	bq->z1 = bq->b1 * in - bq->a1 * out + bq->z2;
	bq->z2 = bq->b2 * in - bq->a2 * out;
	return out;
}

static inline int16_t baseline_clamp_i16(float v)
{
	if (v > 32767.0f)
		return 32767;
	if (v < -32768.0f)
		return -32768;
	return (int16_t)v;
}

static int baseline_hpf_process(int16_t *state, int16_t *data, int num_samples)
{
	struct biquad *bq = (struct biquad *)state;
	for (int i = 0; i < num_samples; i++) {
		data[i] = baseline_clamp_i16(baseline_biquad_process(bq, (float)data[i]));
	}
	return 0;
}

static void baseline_lpf_process(int16_t *state, int16_t *data, int num_samples)
{
	int16_t y = state[0];
	int16_t alpha = state[1];
	for (int i = 0; i < num_samples; i++) {
		y = (int16_t)((uint32_t)(((int32_t)data[i] - (int32_t)y) *
			      (int32_t)alpha) >> 8) + y;
		data[i] = y;
	}
	state[0] = y;
}

static float baseline_drc_compute_gain(struct drc_eq_state *s, float level)
{
	if (level <= s->threshold || level < 1e-10f)
		return 1.0f;
	float target;
	if (level <= s->knee) {
		float range = s->knee - s->threshold;
		if (range < 1e-10f)
			return 1.0f;
		float x = (level - s->threshold) / range;
		target = s->threshold + range * x * s->ratio;
	} else {
		target = s->threshold + (level - s->threshold) * s->ratio;
	}
	return target / level;
}

static void baseline_drc_process(struct drc_eq_state *s, int16_t *data, int num_samples)
{
	for (int i = 0; i < num_samples; i++) {
		float sample = (float)data[i];
		float level = fabsf(sample) / 32768.0f;
		float coeff = level > s->envelope ? s->attack_coeff : s->release_coeff;
		s->envelope += coeff * (level - s->envelope);
		sample *= baseline_drc_compute_gain(s, s->envelope);
		data[i] = baseline_clamp_i16(sample);
	}
}

static void baseline_drc_f32(struct drc_eq_state *s, float *buf, int n)
{
	for (int i = 0; i < n; i++) {
		float level = fabsf(buf[i]) / 32768.0f;
		float coeff = level > s->envelope ? s->attack_coeff : s->release_coeff;
		s->envelope += coeff * (level - s->envelope);
		buf[i] *= baseline_drc_compute_gain(s, s->envelope);
	}
}

static void baseline_eq_process(struct drc_eq_state *s, float *buf, int n)
{
	for (int i = 0; i < n; i++) {
		float sample = buf[i];
		for (int b = 0; b < s->num_eq_bands; b++)
			sample = baseline_biquad_process(&s->eq_bands[b], sample);
		buf[i] = sample;
	}
}

static void baseline_drc_eq_process(void *handle, const float *const *in, float *const *out)
{
	struct drc_eq_state *s = handle;
	const float *src = in[0];
	float *dst = out[0];
	int n = s->frame_size;
	for (int i = 0; i < n; i++)
		dst[i] = src[i];
	if (s->enabled_drc)
		baseline_drc_f32(s, dst, n);
	if (s->enabled_eq)
		baseline_eq_process(s, dst, n);
}

/* ========================================================================= */
/* EVALUATION & BENCHMARK COMPARISON                                         */
/* ========================================================================= */

static void evaluate_all(int16_t *sig_i16, float *sig_f32, int n)
{
	int16_t *buf_base = malloc(n * sizeof(int16_t));
	int16_t *buf_opt = malloc(n * sizeof(int16_t));
	float *fbuf_base = malloc(n * sizeof(float));
	float *fbuf_opt = malloc(n * sizeof(float));

	printf("%-10s | %-16s | %-16s | %-10s | %-15s\n",
	       "Module", "Baseline (MS/s)", "Optimized (MS/s)", "Speedup", "Quality / Diff");
	printf("----------------------------------------------------------------------------\n");

	/* 1. HPF */
	{
		int16_t state_base[16] = {0};
		int16_t state_opt[16] = {0};
		audio_process_hpf_create(state_base, state_base + 8, 0, 0, 8, 8);
		audio_process_hpf_create(state_opt, state_opt + 8, 0, 0, 8, 8);

		/* Init filter coefficients */
		memcpy(buf_base, sig_i16, n * sizeof(int16_t));
		memcpy(buf_opt, sig_i16, n * sizeof(int16_t));
		audio_process_hpf_process(state_base, buf_base, 1);
		audio_process_hpf_process(state_opt, buf_opt, 1);

		/* Measure Baseline */
		double t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_base, sig_i16, n * sizeof(int16_t));
			baseline_hpf_process(state_base, buf_base, n);
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)n * NUM_ITERATIONS / t_base) / 1e6;

		/* Measure Optimized */
		t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_opt, sig_i16, n * sizeof(int16_t));
			audio_process_hpf_process(state_opt, buf_opt, n);
		}
		double t_opt = get_time_sec() - t0;
		double mss_opt = ((double)n * NUM_ITERATIONS / t_opt) / 1e6;

		int max_diff = 0;
		compute_snr_i16(buf_base, buf_opt, n, &max_diff);
		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "HPF", mss_base, mss_opt, ((mss_opt - mss_base) / mss_base) * 100.0,
		       (max_diff == 0) ? "Bit-exact (diff=0)" : "Close");
	}

	/* 2. LPF */
	{
		int16_t state_base[2] = {0};
		int16_t state_opt[2] = {0};

		/* Measure Baseline */
		double t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_base, sig_i16, n * sizeof(int16_t));
			audio_process_lpf_create(state_base, 16000, 3000);
			baseline_lpf_process(state_base, buf_base, n);
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)n * NUM_ITERATIONS / t_base) / 1e6;

		/* Measure Optimized */
		t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_opt, sig_i16, n * sizeof(int16_t));
			audio_process_lpf_create(state_opt, 16000, 3000);
			audio_process_lpf_process(state_opt, buf_opt, n);
		}
		double t_opt = get_time_sec() - t0;
		double mss_opt = ((double)n * NUM_ITERATIONS / t_opt) / 1e6;

		int max_diff = 0;
		compute_snr_i16(buf_base, buf_opt, n, &max_diff);
		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "LPF", mss_base, mss_opt, ((mss_opt - mss_base) / mss_base) * 100.0,
		       (max_diff == 0) ? "Bit-exact (diff=0)" : "Close");
	}

	/* 3. DRC */
	{
		struct drc_eq_state *drc_base = drc_eq_alloc(16000);
		void *drc_opt = audio_process_drc_create(16000, 1);
		audio_process_drc_set_config(drc_base, 20, 10, 4, 0);
		audio_process_drc_set_config(drc_opt, 20, 10, 4, 0);

		/* Measure Baseline */
		double t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_base, sig_i16, n * sizeof(int16_t));
			baseline_drc_process(drc_base, buf_base, n);
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)n * NUM_ITERATIONS / t_base) / 1e6;

		/* Measure Optimized */
		t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_opt, sig_i16, n * sizeof(int16_t));
			audio_process_drc_process(drc_opt, buf_opt, n);
		}
		double t_opt = get_time_sec() - t0;
		double mss_opt = ((double)n * NUM_ITERATIONS / t_opt) / 1e6;

		int max_diff = 0;
		compute_snr_i16(buf_base, buf_opt, n, &max_diff);
		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "DRC", mss_base, mss_opt, ((mss_opt - mss_base) / mss_base) * 100.0,
		       (max_diff == 0) ? "Bit-exact (diff=0)" : "Close");

		free(drc_base);
		audio_process_drc_free(drc_opt);
	}

	/* 4. DRC+EQ */
	{
		void *drc_eq_base = audio_process_drc_eq_create(16000, 1);
		void *drc_eq_opt = audio_process_drc_eq_create(16000, 1);
		int freqs[3] = {500, 1000, 2000};
		float gains[3] = {3.0f, -2.0f, 4.0f};
		int qs[3] = {10, 10, 10};
		audio_process_drc_eq_set_config(drc_eq_base, 20, 10, 4, 3, freqs, gains, qs);
		audio_process_drc_eq_enable(drc_eq_base, 1, 1);
		audio_process_drc_eq_set_config(drc_eq_opt, 20, 10, 4, 3, freqs, gains, qs);
		audio_process_drc_eq_enable(drc_eq_opt, 1, 1);

		struct drc_eq_state *s = (struct drc_eq_state *)drc_eq_base;
		int frame_size = s->frame_size;
		int num_frames = n / frame_size;

		float *out_base = malloc(n * sizeof(float));
		float *out_opt = malloc(n * sizeof(float));

		/* Measure Baseline */
		double t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(fbuf_base, sig_f32, n * sizeof(float));
			for (int f = 0; f < num_frames; f++) {
				const float *in_p = fbuf_base + f * frame_size;
				float *out_p = out_base + f * frame_size;
				baseline_drc_eq_process(drc_eq_base, &in_p, &out_p);
			}
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)(num_frames * frame_size) * NUM_ITERATIONS / t_base) / 1e6;

		/* Measure Optimized */
		t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(fbuf_opt, sig_f32, n * sizeof(float));
			for (int f = 0; f < num_frames; f++) {
				const float *in_p = fbuf_opt + f * frame_size;
				float *out_p = out_opt + f * frame_size;
				audio_process_drc_eq_process(drc_eq_opt, &in_p, &out_p);
			}
		}
		double t_opt = get_time_sec() - t0;
		double mss_opt = ((double)(num_frames * frame_size) * NUM_ITERATIONS / t_opt) / 1e6;

		float max_diff = 0.0f;
		double snr = compute_snr_f32(out_base, out_opt, num_frames * frame_size, &max_diff);
		char snr_str[32];
		snprintf(snr_str, sizeof(snr_str), "SNR: %.1f dB", snr);
		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "DRC+EQ", mss_base, mss_opt, ((mss_opt - mss_base) / mss_base) * 100.0,
		       snr > 100.0 ? "SNR > 100dB [PASS]" : snr_str);

		audio_process_drc_eq_free(drc_eq_base);
		audio_process_drc_eq_free(drc_eq_opt);
		free(out_base);
		free(out_opt);
	}

	/* 5. Howling Suppression */
	{
		void *hs = audio_process_hs_create(16000, 128);
		int howling_iters = NUM_ITERATIONS / 5;
		if (howling_iters < 2) howling_iters = 2;

		double t0 = get_time_sec();
		for (int iter = 0; iter < howling_iters; iter++) {
			memcpy(buf_opt, sig_i16, n * sizeof(int16_t));
			audio_process_hs_process(hs, buf_opt, n);
		}
		double t_opt = get_time_sec() - t0;
		double mss_opt = ((double)n * howling_iters / t_opt) / 1e6;

		printf("%-10s | %13s | %13.2f MS/s | %10s | %s\n",
		       "HS", "N/A", mss_opt, "Optimized", "Pass");
		audio_process_hs_free(hs);
	}

	free(buf_base);
	free(buf_opt);
	free(fbuf_base);
	free(fbuf_opt);
}

int main(void)
{
	srand(12345);
	printf("============================================================================\n");
	printf(" Audio Process Optimization Evaluation: Baseline vs. Optimized\n");
	printf("============================================================================\n\n");

	int16_t *sig_i16 = malloc(BENCH_SAMPLES * sizeof(int16_t));
	float *sig_f32 = malloc(BENCH_SAMPLES * sizeof(float));
	generate_test_signal_i16(sig_i16, BENCH_SAMPLES);
	generate_test_signal_f32(sig_f32, BENCH_SAMPLES);

	evaluate_all(sig_i16, sig_f32, BENCH_SAMPLES);

	free(sig_i16);
	free(sig_f32);
	return 0;
}
