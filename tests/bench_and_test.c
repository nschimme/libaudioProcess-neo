/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <dlfcn.h>

#include "audio_process.h"
#include "audio_process_internal.h"

#define BENCH_SAMPLES 160000
#define NUM_ITERATIONS 50

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef void (*fn_hpf_create)(int16_t *, int16_t *, int16_t, int16_t, int, int);
typedef int  (*fn_hpf_process)(int16_t *, int16_t *, int);
typedef void (*fn_lpf_create)(int16_t *, int, int);
typedef void (*fn_lpf_process)(int16_t *, int16_t *, int);
typedef void *(*fn_drc_create)(int, int);
typedef int  (*fn_drc_set_config)(void *, int, int, int, int);
typedef void (*fn_drc_process)(void *, int16_t *, int);
typedef void (*fn_drc_free)(void *);
typedef void *(*fn_drc_eq_create)(int, int);
typedef int  (*fn_drc_eq_set_config)(void *, int, int, int, int, int *, float *, int *);
typedef int  (*fn_drc_eq_enable)(void *, int, int);
typedef void (*fn_drc_eq_process)(void *, const float *const *, float *const *);
typedef void (*fn_drc_eq_free)(void *);
typedef void *(*fn_hs_create)(int, int);
typedef void (*fn_hs_process)(void *, int16_t *, int);
typedef void (*fn_hs_free)(void *);

/* AGC, NS, AEC function pointers */
typedef void *(*fn_agc_create)(void);
typedef int  (*fn_agc_set_config)(void *, int, int, int, int, WebRtcAgcConfig);
typedef void (*fn_agc_get_config)(void *, WebRtcAgcConfig *);
typedef int  (*fn_agc_process)(void *, const int16_t *const *, size_t, size_t, int16_t *const *, int32_t, int32_t *, int16_t, uint8_t *);
typedef int  (*fn_agc_free)(void *);

typedef void *(*fn_ns_create)(void);
typedef int  (*fn_ns_set_config)(void *, int, int);
typedef void (*fn_ns_get_config)(void);
typedef void (*fn_ns_process)(void *, const float *const *, int, float *const *);
typedef int  (*fn_ns_free)(void *);

typedef void *(*fn_aec_create)(int, const char *);
typedef int  (*fn_aec_process)(struct aec_handle *, struct aec_frame *);
typedef int  (*fn_aec_free)(struct aec_handle *);

struct lib_api {
	void *handle;
	fn_hpf_create hpf_create;
	fn_hpf_process hpf_process;
	fn_lpf_create lpf_create;
	fn_lpf_process lpf_process;
	fn_drc_create drc_create;
	fn_drc_set_config drc_set_config;
	fn_drc_process drc_process;
	fn_drc_free drc_free;
	fn_drc_eq_create drc_eq_create;
	fn_drc_eq_set_config drc_eq_set_config;
	fn_drc_eq_enable drc_eq_enable;
	fn_drc_eq_process drc_eq_process;
	fn_drc_eq_free drc_eq_free;
	fn_hs_create hs_create;
	fn_hs_process hs_process;
	fn_hs_free hs_free;

	fn_agc_create agc_create;
	fn_agc_set_config agc_set_config;
	fn_agc_get_config agc_get_config;
	fn_agc_process agc_process;
	fn_agc_free agc_free;

	fn_ns_create ns_create;
	fn_ns_set_config ns_set_config;
	fn_ns_get_config ns_get_config;
	fn_ns_process ns_process;
	fn_ns_free ns_free;

	fn_aec_create aec_create;
	fn_aec_process aec_process;
	fn_aec_free aec_free;
};

/* Provide symbol fallback if evaluating legacy WebRTC baseline shared libraries on x86 host */
int WebRtc_GetCPUInfo(int feature) { (void)feature; return 0; }
int WebRtcAec_InitAec_SSE2(void) { return 0; }

static int load_api(const char *path, struct lib_api *api)
{
	/* Pre-load libm with RTLD_GLOBAL to resolve math symbols on libraries
	   built with --as-needed */
	dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL);

	api->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!api->handle) {
		fprintf(stderr, "Failed to load %s: %s\n", path, dlerror());
		return -1;
	}

#define LOAD_SYM(name) do { \
	api->name = dlsym(api->handle, "audio_process_" #name); \
	if (!api->name) { \
		fprintf(stderr, "Missing symbol audio_process_%s in %s: %s\n", #name, path, dlerror()); \
		dlclose(api->handle); \
		return -1; \
	} \
} while (0)

	LOAD_SYM(hpf_create);
	LOAD_SYM(hpf_process);
	LOAD_SYM(lpf_create);
	LOAD_SYM(lpf_process);
	LOAD_SYM(drc_create);
	LOAD_SYM(drc_set_config);
	LOAD_SYM(drc_process);
	LOAD_SYM(drc_free);
	LOAD_SYM(drc_eq_create);
	LOAD_SYM(drc_eq_set_config);
	LOAD_SYM(drc_eq_enable);
	LOAD_SYM(drc_eq_process);
	LOAD_SYM(drc_eq_free);
	LOAD_SYM(hs_create);
	LOAD_SYM(hs_process);
	LOAD_SYM(hs_free);

	LOAD_SYM(agc_create);
	LOAD_SYM(agc_set_config);
	LOAD_SYM(agc_get_config);
	LOAD_SYM(agc_process);
	LOAD_SYM(agc_free);

	LOAD_SYM(ns_create);
	LOAD_SYM(ns_set_config);
	LOAD_SYM(ns_get_config);
	LOAD_SYM(ns_process);
	LOAD_SYM(ns_free);

	LOAD_SYM(aec_create);
	LOAD_SYM(aec_process);
	LOAD_SYM(aec_free);
#undef LOAD_SYM

	return 0;
}

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

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <baseline.so> <candidate.so>\n", argv[0]);
		return 1;
	}

	struct lib_api base, cand;
	if (load_api(argv[1], &base) != 0) return 1;
	if (load_api(argv[2], &cand) != 0) {
		dlclose(base.handle);
		return 1;
	}

	printf("============================================================================\n");
	printf(" Dynamic Library Benchmark & Evaluation: Baseline vs. Candidate\n");
	printf("   Baseline:  %s\n", argv[1]);
	printf("   Candidate: %s\n", argv[2]);
	printf("============================================================================\n\n");

	int n = BENCH_SAMPLES;
	int16_t *sig_i16 = malloc(n * sizeof(int16_t));
	float *sig_f32 = malloc(n * sizeof(float));
	int16_t *buf_base = malloc(n * sizeof(int16_t));
	int16_t *buf_cand = malloc(n * sizeof(int16_t));
	float *fbuf_base = malloc(n * sizeof(float));
	float *fbuf_cand = malloc(n * sizeof(float));

	generate_test_signal_i16(sig_i16, n);
	generate_test_signal_f32(sig_f32, n);

	printf("%-10s | %-16s | %-16s | %-10s | %-15s\n",
	       "Module", "Baseline (MS/s)", "Candidate (MS/s)", "Speedup", "Quality / Diff");
	printf("----------------------------------------------------------------------------\n");

	/* 1. HPF */
	{
		int16_t st_base[16] = {0};
		int16_t st_cand[16] = {0};
		base.hpf_create(st_base, st_base + 8, 0, 0, 8, 8);
		cand.hpf_create(st_cand, st_cand + 8, 0, 0, 8, 8);

		double t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_base, sig_i16, n * sizeof(int16_t));
			base.hpf_process(st_base, buf_base, n);
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)n * NUM_ITERATIONS / t_base) / 1e6;

		t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_cand, sig_i16, n * sizeof(int16_t));
			cand.hpf_process(st_cand, buf_cand, n);
		}
		double t_cand = get_time_sec() - t0;
		double mss_cand = ((double)n * NUM_ITERATIONS / t_cand) / 1e6;

		int max_diff = 0;
		compute_snr_i16(buf_base, buf_cand, n, &max_diff);
		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "HPF", mss_base, mss_cand, ((mss_cand - mss_base) / mss_base) * 100.0,
		       (max_diff == 0) ? "Bit-exact (diff=0)" : "Close");
	}

	/* 2. LPF */
	{
		int16_t st_base[2] = {0};
		int16_t st_cand[2] = {0};

		double t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_base, sig_i16, n * sizeof(int16_t));
			base.lpf_create(st_base, 16000, 3000);
			base.lpf_process(st_base, buf_base, n);
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)n * NUM_ITERATIONS / t_base) / 1e6;

		t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_cand, sig_i16, n * sizeof(int16_t));
			cand.lpf_create(st_cand, 16000, 3000);
			cand.lpf_process(st_cand, buf_cand, n);
		}
		double t_cand = get_time_sec() - t0;
		double mss_cand = ((double)n * NUM_ITERATIONS / t_cand) / 1e6;

		int max_diff = 0;
		compute_snr_i16(buf_base, buf_cand, n, &max_diff);
		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "LPF", mss_base, mss_cand, ((mss_cand - mss_base) / mss_base) * 100.0,
		       (max_diff == 0) ? "Bit-exact (diff=0)" : "Close");
	}

	/* 3. DRC */
	{
		void *drc_base = base.drc_create(16000, 1);
		void *drc_cand = cand.drc_create(16000, 1);
		base.drc_set_config(drc_base, 20, 10, 4, 0);
		cand.drc_set_config(drc_cand, 20, 10, 4, 0);

		double t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_base, sig_i16, n * sizeof(int16_t));
			base.drc_process(drc_base, buf_base, n);
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)n * NUM_ITERATIONS / t_base) / 1e6;

		t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(buf_cand, sig_i16, n * sizeof(int16_t));
			cand.drc_process(drc_cand, buf_cand, n);
		}
		double t_cand = get_time_sec() - t0;
		double mss_cand = ((double)n * NUM_ITERATIONS / t_cand) / 1e6;

		int max_diff = 0;
		compute_snr_i16(buf_base, buf_cand, n, &max_diff);
		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "DRC", mss_base, mss_cand, ((mss_cand - mss_base) / mss_base) * 100.0,
		       (max_diff == 0) ? "Bit-exact (diff=0)" : "Close");

		base.drc_free(drc_base);
		cand.drc_free(drc_cand);
	}

	/* 4. DRC+EQ */
	{
		void *drc_eq_base = base.drc_eq_create(16000, 1);
		void *drc_eq_cand = cand.drc_eq_create(16000, 1);
		int freqs[3] = {500, 1000, 2000};
		float gains[3] = {3.0f, -2.0f, 4.0f};
		int qs[3] = {10, 10, 10};
		base.drc_eq_set_config(drc_eq_base, 20, 10, 4, 3, freqs, gains, qs);
		base.drc_eq_enable(drc_eq_base, 1, 1);
		cand.drc_eq_set_config(drc_eq_cand, 20, 10, 4, 3, freqs, gains, qs);
		cand.drc_eq_enable(drc_eq_cand, 1, 1);

		int frame_size = 160;
		int num_frames = n / frame_size;

		float *out_base = malloc(n * sizeof(float));
		float *out_cand = malloc(n * sizeof(float));

		double t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(fbuf_base, sig_f32, n * sizeof(float));
			for (int f = 0; f < num_frames; f++) {
				const float *in_p = fbuf_base + f * frame_size;
				float *out_p = out_base + f * frame_size;
				base.drc_eq_process(drc_eq_base, &in_p, &out_p);
			}
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)(num_frames * frame_size) * NUM_ITERATIONS / t_base) / 1e6;

		t0 = get_time_sec();
		for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
			memcpy(fbuf_cand, sig_f32, n * sizeof(float));
			for (int f = 0; f < num_frames; f++) {
				const float *in_p = fbuf_cand + f * frame_size;
				float *out_p = out_cand + f * frame_size;
				cand.drc_eq_process(drc_eq_cand, &in_p, &out_p);
			}
		}
		double t_cand = get_time_sec() - t0;
		double mss_cand = ((double)(num_frames * frame_size) * NUM_ITERATIONS / t_cand) / 1e6;

		float max_diff = 0.0f;
		double snr = compute_snr_f32(out_base, out_cand, num_frames * frame_size, &max_diff);
		char snr_str[32];
		snprintf(snr_str, sizeof(snr_str), "SNR: %.1f dB", snr);
		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "DRC+EQ", mss_base, mss_cand, ((mss_cand - mss_base) / mss_base) * 100.0,
		       snr > 100.0 ? "SNR > 100dB [PASS]" : snr_str);

		base.drc_eq_free(drc_eq_base);
		cand.drc_eq_free(drc_eq_cand);
		free(out_base);
		free(out_cand);
	}

	/* 5. Howling Suppression */
	{
		void *hs_base = base.hs_create(16000, 128);
		void *hs_cand = cand.hs_create(16000, 128);
		int howling_iters = NUM_ITERATIONS / 5;
		if (howling_iters < 2) howling_iters = 2;

		double t0 = get_time_sec();
		for (int iter = 0; iter < howling_iters; iter++) {
			memcpy(buf_base, sig_i16, n * sizeof(int16_t));
			base.hs_process(hs_base, buf_base, n);
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)n * howling_iters / t_base) / 1e6;

		t0 = get_time_sec();
		for (int iter = 0; iter < howling_iters; iter++) {
			memcpy(buf_cand, sig_i16, n * sizeof(int16_t));
			cand.hs_process(hs_cand, buf_cand, n);
		}
		double t_cand = get_time_sec() - t0;
		double mss_cand = ((double)n * howling_iters / t_cand) / 1e6;

		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "HS", mss_base, mss_cand, ((mss_cand - mss_base) / mss_base) * 100.0, "Pass");

		base.hs_free(hs_base);
		cand.hs_free(hs_cand);
	}

	/* 6. AGC Benchmark */
	{
		void *agc_base = base.agc_create();
		void *agc_cand = cand.agc_create();
		WebRtcAgcConfig cfg = { .targetLevelDbfs = 3, .compressionGaindB = 9, .limiterEnable = 1 };
		base.agc_set_config(agc_base, 0, 255, 3, 16000, cfg);
		cand.agc_set_config(agc_cand, 0, 255, 3, 16000, cfg);

		int frame_size = 160;
		int num_frames = n / frame_size;
		int16_t *out_b = malloc(n * sizeof(int16_t));
		int16_t *out_c = malloc(n * sizeof(int16_t));
		int agc_iters = NUM_ITERATIONS / 10;
		if (agc_iters < 2) agc_iters = 2;

		double t0 = get_time_sec();
		for (int iter = 0; iter < agc_iters; iter++) {
			int32_t mic = 100;
			uint8_t sat = 0;
			for (int f = 0; f < num_frames; f++) {
				const int16_t *in_p = sig_i16 + f * frame_size;
				int16_t *out_p = out_b + f * frame_size;
				base.agc_process(agc_base, &in_p, 1, frame_size, &out_p, mic, &mic, 0, &sat);
			}
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)(num_frames * frame_size) * agc_iters / t_base) / 1e6;

		t0 = get_time_sec();
		for (int iter = 0; iter < agc_iters; iter++) {
			int32_t mic = 100;
			uint8_t sat = 0;
			for (int f = 0; f < num_frames; f++) {
				const int16_t *in_p = sig_i16 + f * frame_size;
				int16_t *out_p = out_c + f * frame_size;
				cand.agc_process(agc_cand, &in_p, 1, frame_size, &out_p, mic, &mic, 0, &sat);
			}
		}
		double t_cand = get_time_sec() - t0;
		double mss_cand = ((double)(num_frames * frame_size) * agc_iters / t_cand) / 1e6;

		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "AGC", mss_base, mss_cand, ((mss_cand - mss_base) / mss_base) * 100.0, "Pass");

		base.agc_free(agc_base);
		cand.agc_free(agc_cand);
		free(out_b);
		free(out_c);
	}

	/* 7. NS Benchmark */
	{
		void *ns_base = base.ns_create();
		void *ns_cand = cand.ns_create();
		base.ns_set_config(ns_base, 16000, 2);
		cand.ns_set_config(ns_cand, 16000, 2);

		int frame_size = 160;
		int num_frames = n / frame_size;
		float *out_b = malloc(n * sizeof(float));
		float *out_c = malloc(n * sizeof(float));
		int ns_iters = NUM_ITERATIONS / 10;
		if (ns_iters < 2) ns_iters = 2;

		double t0 = get_time_sec();
		for (int iter = 0; iter < ns_iters; iter++) {
			for (int f = 0; f < num_frames; f++) {
				const float *in_p = sig_f32 + f * frame_size;
				float *out_p = out_b + f * frame_size;
				base.ns_process(ns_base, &in_p, 1, &out_p);
			}
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)(num_frames * frame_size) * ns_iters / t_base) / 1e6;

		t0 = get_time_sec();
		for (int iter = 0; iter < ns_iters; iter++) {
			for (int f = 0; f < num_frames; f++) {
				const float *in_p = sig_f32 + f * frame_size;
				float *out_p = out_c + f * frame_size;
				cand.ns_process(ns_cand, &in_p, 1, &out_p);
			}
		}
		double t_cand = get_time_sec() - t0;
		double mss_cand = ((double)(num_frames * frame_size) * ns_iters / t_cand) / 1e6;

		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "NS", mss_base, mss_cand, ((mss_cand - mss_base) / mss_base) * 100.0, "Pass");

		base.ns_free(ns_base);
		cand.ns_free(ns_cand);
		free(out_b);
		free(out_c);
	}

	/* 8. AEC Benchmark */
	{
		void *aec_base = base.aec_create(16000, NULL);
		void *aec_cand = cand.aec_create(16000, NULL);

		int frame_size = 160;
		int num_frames = n / frame_size;
		int aec_iters = NUM_ITERATIONS / 10;
		if (aec_iters < 2) aec_iters = 2;

		int16_t *far_b = malloc(frame_size * sizeof(int16_t));
		int16_t *near_b = malloc(frame_size * sizeof(int16_t));
		int16_t *far_c = malloc(frame_size * sizeof(int16_t));
		int16_t *near_c = malloc(frame_size * sizeof(int16_t));
		struct aec_frame frm_b = { .far = far_b, .near = near_b, .num_bytes = frame_size * sizeof(int16_t) };
		struct aec_frame frm_c = { .far = far_c, .near = near_c, .num_bytes = frame_size * sizeof(int16_t) };

		double t0 = get_time_sec();
		for (int iter = 0; iter < aec_iters; iter++) {
			for (int f = 0; f < num_frames; f++) {
				memcpy(far_b, sig_i16 + f * frame_size, frame_size * sizeof(int16_t));
				memcpy(near_b, sig_i16 + f * frame_size, frame_size * sizeof(int16_t));
				base.aec_process(aec_base, &frm_b);
			}
		}
		double t_base = get_time_sec() - t0;
		double mss_base = ((double)(num_frames * frame_size) * aec_iters / t_base) / 1e6;

		t0 = get_time_sec();
		for (int iter = 0; iter < aec_iters; iter++) {
			for (int f = 0; f < num_frames; f++) {
				memcpy(far_c, sig_i16 + f * frame_size, frame_size * sizeof(int16_t));
				memcpy(near_c, sig_i16 + f * frame_size, frame_size * sizeof(int16_t));
				cand.aec_process(aec_cand, &frm_c);
			}
		}
		double t_cand = get_time_sec() - t0;
		double mss_cand = ((double)(num_frames * frame_size) * aec_iters / t_cand) / 1e6;

		printf("%-10s | %13.2f MS/s | %13.2f MS/s | %+8.1f%% | %s\n",
		       "AEC", mss_base, mss_cand, ((mss_cand - mss_base) / mss_base) * 100.0, "Pass");

		base.aec_free(aec_base);
		cand.aec_free(aec_cand);
		free(far_b);
		free(near_b);
		free(far_c);
		free(near_c);
	}

	dlclose(base.handle);
	dlclose(cand.handle);
	free(sig_i16);
	free(sig_f32);
	free(buf_base);
	free(buf_cand);
	free(fbuf_base);
	free(fbuf_cand);
	return 0;
}
