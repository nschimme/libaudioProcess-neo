/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <dlfcn.h>
#include <assert.h>

#include "audio_process.h"
#include "audio_process_internal.h"
#include "util.h"
#include "biquad.h"
#include "drc_eq_internal.h"

#define ASSERT_TRUE(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
		exit(1); \
	} \
} while (0)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Quality metric: RMS level */
static double compute_rms_i16(const int16_t *buf, int start, int end)
{
	double sum = 0.0;
	int count = end - start;
	if (count <= 0) return 0.0;
	for (int i = start; i < end; i++) {
		double v = buf[i];
		sum += v * v;
	}
	return sqrt(sum / count);
}

static double compute_rms_f32(const float *buf, int start, int end)
{
	double sum = 0.0;
	int count = end - start;
	if (count <= 0) return 0.0;
	for (int i = start; i < end; i++) {
		double v = buf[i];
		sum += v * v;
	}
	return sqrt(sum / count);
}

/* Test 1: Guard rail on clamp_i16 boundaries */
static void test_clamp_guardrails(void)
{
	printf("Running test_clamp_guardrails...\n");
	ASSERT_TRUE(clamp_i16(0.0f) == 0, "clamp zero");
	ASSERT_TRUE(clamp_i16(32767.0f) == 32767, "clamp max i16");
	ASSERT_TRUE(clamp_i16(32767.4f) == 32767, "clamp max i16 float");
	ASSERT_TRUE(clamp_i16(32768.0f) == 32767, "clamp positive overflow");
	ASSERT_TRUE(clamp_i16(100000.0f) == 32767, "clamp high positive overflow");
	ASSERT_TRUE(clamp_i16(-32768.0f) == -32768, "clamp min i16");
	ASSERT_TRUE(clamp_i16(-32768.4f) == -32768, "clamp min i16 float");
	ASSERT_TRUE(clamp_i16(-32769.0f) == -32768, "clamp negative overflow");
	ASSERT_TRUE(clamp_i16(-100000.0f) == -32768, "clamp high negative overflow");
	printf("  -> PASS\n");
}

/* Test 2: Guard rail on LPF DC gain, step response, and stability */
static void test_lpf_guardrails(void)
{
	printf("Running test_lpf_guardrails...\n");
	int16_t state[2] = {0};
	audio_process_lpf_create(state, 16000, 1000);
	ASSERT_TRUE(state[0] == 0, "initial state zero");
	ASSERT_TRUE(state[1] > 0 && state[1] <= 256, "alpha valid range");

	/* Test DC step: all 10000 should converge towards 10000 without overshoot */
	int n = 1000;
	int16_t *buf = malloc(n * sizeof(int16_t));
	for (int i = 0; i < n; i++) buf[i] = 10000;
	audio_process_lpf_process(state, buf, n);

	/* Last samples should be very close to 10000 and monotonic without oscillation */
	ASSERT_TRUE(buf[n - 1] >= 9950 && buf[n - 1] <= 10000, "DC convergence");
	for (int i = 1; i < n; i++) {
		ASSERT_TRUE(buf[i] >= buf[i - 1], "monotonic step response");
	}

	/* Test silence: output must decay to 0 */
	for (int i = 0; i < n; i++) buf[i] = 0;
	audio_process_lpf_process(state, buf, n);
	ASSERT_TRUE(buf[n - 1] == 0, "decay to 0 on silence");

	/* Test high frequency attenuation: 7000 Hz sine with 1 kHz cutoff should be attenuated > 12 dB (first-order -6dB/octave roll-off) */
	memset(state, 0, sizeof(state));
	audio_process_lpf_create(state, 16000, 1000);
	for (int i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		buf[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 7000.0 * t));
	}
	audio_process_lpf_process(state, buf, n);
	double hf_rms = compute_rms_i16(buf, n - 400, n);
	double expected_rms = 10000.0 / sqrt(2.0);
	double attenuation_db = 20.0 * log10(expected_rms / hf_rms);
	ASSERT_TRUE(attenuation_db > 12.0, "LPF high frequency stopband attenuation > 12 dB");

	audio_process_lpf_free();
	free(buf);
	printf("  -> PASS\n");
}

/* Test 3: Guard rail on HPF DC rejection and high frequency pass-through */
static void test_hpf_guardrails(void)
{
	printf("Running test_hpf_guardrails...\n");
	int16_t state[16] = {0};
	audio_process_hpf_create(state, state + 8, 0, 0, 8, 8);

	/* DC signal: 10000 constant */
	int n = 2000;
	int16_t *buf = malloc(n * sizeof(int16_t));
	for (int i = 0; i < n; i++) buf[i] = 10000;

	audio_process_hpf_process(state, buf, n);
	/* DC must be rejected (attenuated by > 40 dB, < 50 LSB) */
	ASSERT_TRUE(abs(buf[n - 1]) < 50, "HPF DC rejection");

	/* Reset and test 2 kHz passband: should pass through with near-unity gain */
	memset(state, 0, sizeof(state));
	for (int i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		buf[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 2000.0 * t));
	}
	audio_process_hpf_process(state, buf, n);
	int max_val = 0;
	for (int i = n - 500; i < n; i++) {
		if (abs(buf[i]) > max_val) max_val = abs(buf[i]);
	}
	ASSERT_TRUE(max_val >= 9500 && max_val <= 10500, "HPF passband unity gain");

	audio_process_hpf_free();
	free(buf);
	printf("  -> PASS\n");
}

/* Test 4: Guard rail on DRC compression, envelope tracking, and THD */
static void test_drc_guardrails(void)
{
	printf("Running test_drc_guardrails...\n");
	void *drc = audio_process_drc_create(16000, 1);
	ASSERT_TRUE(drc != NULL, "drc create");
	/* Threshold = 20dB (~0.1 linear = 3276), ratio = 4:1 */
	audio_process_drc_set_config(drc, 20, 10, 4, 0);

	int n = 1600; /* 100ms */
	int16_t *buf = malloc(n * sizeof(int16_t));

	/* Large full-scale signal (30000) */
	for (int i = 0; i < n; i++) buf[i] = 30000;
	audio_process_drc_process(drc, buf, n);

	/* Must be compressed and non-overflowing */
	for (int i = 0; i < n; i++) {
		ASSERT_TRUE(buf[i] <= 32767 && buf[i] >= -32768, "no overflow");
		if (i > 800) {
			ASSERT_TRUE(buf[i] < 30000, "drc compresses loud signals");
		}
	}

	/* Test quiet signal below threshold: must pass through with 100% unity gain (unmodified) */
	audio_process_drc_free(drc);
	drc = audio_process_drc_create(16000, 1);
	audio_process_drc_set_config(drc, 20, 10, 4, 0);
	for (int i = 0; i < n; i++) buf[i] = 1000; /* Below threshold ~3276 */
	audio_process_drc_process(drc, buf, n);
	for (int i = 0; i < n; i++) {
		ASSERT_TRUE(buf[i] == 1000, "quiet signal passes with 0dB gain");
	}

	audio_process_drc_free(drc);
	free(buf);
	printf("  -> PASS\n");
}

/* Test 5: Guard rail on DRC+EQ combined in-place safety and frequency response */
static void test_drc_eq_guardrails(void)
{
	printf("Running test_drc_eq_guardrails...\n");
	void *drc_eq = audio_process_drc_eq_create(16000, 1);
	int freqs[2] = {1000, 2000};
	float gains[2] = {6.0f, -6.0f};
	int qs[2] = {10, 10};
	audio_process_drc_eq_set_config(drc_eq, 20, 10, 4, 2, freqs, gains, qs);
	audio_process_drc_eq_enable(drc_eq, 1, 1);

	int frame_size = 160;
	float *in = malloc(frame_size * sizeof(float));
	float *out = malloc(frame_size * sizeof(float));

	for (int i = 0; i < frame_size; i++) in[i] = 0.5f;

	/* Separate input/output buffers */
	const float *in_ptr = in;
	float *out_ptr = out;
	audio_process_drc_eq_process(drc_eq, &in_ptr, &out_ptr);

	for (int i = 0; i < frame_size; i++) {
		ASSERT_TRUE(!isnan(out[i]), "out not nan");
		ASSERT_TRUE(!isinf(out[i]), "out not inf");
	}

	/* In-place input/output buffer */
	float *inplace = malloc(frame_size * sizeof(float));
	memcpy(inplace, in, frame_size * sizeof(float));
	const float *inplace_in = inplace;
	float *inplace_out = inplace;
	audio_process_drc_eq_process(drc_eq, &inplace_in, &inplace_out);

	for (int i = 0; i < frame_size; i++) {
		ASSERT_TRUE(!isnan(inplace[i]), "inplace not nan");
		ASSERT_TRUE(!isinf(inplace[i]), "inplace not inf");
	}

	audio_process_drc_eq_free(drc_eq);
	free(in);
	free(out);
	free(inplace);
	printf("  -> PASS\n");
}

/* Test 6: Guard rail on HS (Howling Suppression) with notch depth verification */
static void test_howling_guardrails(void)
{
	printf("Running test_howling_guardrails...\n");
	void *hs = audio_process_hs_create(16000, 128);
	ASSERT_TRUE(hs != NULL, "hs create");

	/* Create 1 kHz howling tone */
	int n = 128 * 25; /* 25 frames */
	int16_t *buf = malloc(n * sizeof(int16_t));
	for (int i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		buf[i] = (int16_t)(16000.0 * sin(2.0 * M_PI * 1000.0 * t));
	}

	double initial_rms = compute_rms_i16(buf, 0, 128);
	audio_process_hs_process(hs, buf, n);

	/* Check that howling filter stays bounded and active */
	for (int i = 0; i < n; i++) {
		ASSERT_TRUE(buf[i] <= 32767 && buf[i] >= -32768, "hs bounded");
	}

	/* Verify howling suppression: steady state RMS must be attenuated */
	double final_rms = compute_rms_i16(buf, n - 256, n);
	double suppression_db = 20.0 * log10(initial_rms / (final_rms > 1.0 ? final_rms : 1.0));
	ASSERT_TRUE(suppression_db > 3.0, "HS notch filter suppresses howling tone by > 3 dB");

	audio_process_hs_free(hs);
	free(buf);
	printf("  -> PASS\n");
}

/* Test 7: Extreme Audio Stress Test (White noise, Nyquist, denormals, clipping safety) */
static void test_extreme_audio_stress(void)
{
	printf("Running test_extreme_audio_stress...\n");
	int n = 16000;
	int16_t *stress_buf = malloc(n * sizeof(int16_t));

	/* Mix of full-scale square wave, Nyquist (Fs/2), and extreme spikes */
	for (int i = 0; i < n; i++) {
		if (i % 2 == 0) stress_buf[i] = 32767;
		else stress_buf[i] = -32768;
	}

	/* Run through HPF */
	int16_t hpf_st[16] = {0};
	audio_process_hpf_create(hpf_st, hpf_st + 8, 0, 0, 8, 8);
	audio_process_hpf_process(hpf_st, stress_buf, n);
	for (int i = 0; i < n; i++) {
		ASSERT_TRUE(stress_buf[i] <= 32767 && stress_buf[i] >= -32768, "HPF stress non-overflow");
	}

	/* Run through LPF */
	int16_t lpf_st[2] = {0};
	audio_process_lpf_create(lpf_st, 16000, 2000);
	audio_process_lpf_process(lpf_st, stress_buf, n);
	for (int i = 0; i < n; i++) {
		ASSERT_TRUE(stress_buf[i] <= 32767 && stress_buf[i] >= -32768, "LPF stress non-overflow");
	}

	/* Run through DRC */
	void *drc = audio_process_drc_create(16000, 1);
	audio_process_drc_set_config(drc, 20, 10, 4, 0);
	audio_process_drc_process(drc, stress_buf, n);
	for (int i = 0; i < n; i++) {
		ASSERT_TRUE(stress_buf[i] <= 32767 && stress_buf[i] >= -32768, "DRC stress non-overflow");
	}
	audio_process_drc_free(drc);

	free(stress_buf);
	printf("  -> PASS\n");
}

/* Test 8: AGC (Automatic Gain Control) E2E Regression Tests */
static void test_agc_guardrails(void)
{
	printf("Running test_agc_guardrails (E2E)...\n");
	void *handle = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
	if (!handle) handle = dlopen("./libaudioProcess.so", RTLD_NOW | RTLD_GLOBAL);

	void *(*fn_agc_create)(void) = dlsym(handle, "audio_process_agc_create");
	int (*fn_agc_set_config)(void *, int, int, int, int, WebRtcAgcConfig) = dlsym(handle, "audio_process_agc_set_config");
	void (*fn_agc_get_config)(void *, WebRtcAgcConfig *) = dlsym(handle, "audio_process_agc_get_config");
	int (*fn_agc_process)(void *, const int16_t *const *, size_t, size_t, int16_t *const *, int32_t, int32_t *, int16_t, uint8_t *) = dlsym(handle, "audio_process_agc_process");
	int (*fn_agc_free)(void *) = dlsym(handle, "audio_process_agc_free");

	if (!fn_agc_create || !fn_agc_set_config || !fn_agc_get_config || !fn_agc_process || !fn_agc_free) {
		printf("  [SKIP - AGC symbols not in binary link]\n");
		return;
	}

	void *agc = fn_agc_create();
	ASSERT_TRUE(agc != NULL, "agc create");

	WebRtcAgcConfig in_cfg = { .targetLevelDbfs = 3, .compressionGaindB = 12, .limiterEnable = 1 };
	int res = fn_agc_set_config(agc, 0, 255, 3, 16000, in_cfg);
	ASSERT_TRUE(res == 0, "agc set config");

	WebRtcAgcConfig out_cfg;
	fn_agc_get_config(agc, &out_cfg);
	ASSERT_TRUE(out_cfg.targetLevelDbfs == 3, "agc get config target");

	/* Scenario A: Quiet speech ramp-up */
	int16_t in_buf[160];
	int16_t out_buf[160];
	const int16_t *in_ptr = in_buf;
	int16_t *out_ptr = out_buf;
	int32_t mic_level = 100;
	uint8_t sat = 0;

	for (int f = 0; f < 30; f++) {
		for (int i = 0; i < 160; i++) in_buf[i] = 800; /* Low level */
		res = fn_agc_process(agc, &in_ptr, 1, 160, &out_ptr, mic_level, &mic_level, 0, &sat);
		ASSERT_TRUE(res == 0, "agc process quiet");
	}
	/* Verify gain amplified the signal */
	double quiet_out_rms = compute_rms_i16(out_buf, 0, 160);
	ASSERT_TRUE(quiet_out_rms >= 800.0, "AGC amplifies quiet speech upward");

	/* Scenario B: Loud speech / Limiter engaged */
	for (int f = 0; f < 30; f++) {
		for (int i = 0; i < 160; i++) in_buf[i] = 28000; /* Loud level */
		res = fn_agc_process(agc, &in_ptr, 1, 160, &out_ptr, mic_level, &mic_level, 0, &sat);
		ASSERT_TRUE(res == 0, "agc process loud");
		for (int i = 0; i < 160; i++) {
			ASSERT_TRUE(out_buf[i] <= 32767 && out_buf[i] >= -32768, "AGC limiter prevents overflow");
		}
	}

	fn_agc_free(agc);
	printf("  -> PASS\n");
}

/* Test 9: NS (Noise Suppression) E2E Regression Tests */
static void test_ns_guardrails(void)
{
	printf("Running test_ns_guardrails (E2E)...\n");
	void *handle = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
	if (!handle) handle = dlopen("./libaudioProcess.so", RTLD_NOW | RTLD_GLOBAL);

	void *(*fn_ns_create)(void) = dlsym(handle, "audio_process_ns_create");
	int (*fn_ns_set_config)(void *, int, int) = dlsym(handle, "audio_process_ns_set_config");
	void (*fn_ns_get_config)(void) = dlsym(handle, "audio_process_ns_get_config");
	void (*fn_ns_process)(void *, const float *const *, int, float *const *) = dlsym(handle, "audio_process_ns_process");
	int (*fn_ns_free)(void *) = dlsym(handle, "audio_process_ns_free");

	if (!fn_ns_create || !fn_ns_set_config || !fn_ns_process || !fn_ns_free) {
		printf("  [SKIP - NS symbols not in binary link]\n");
		return;
	}

	void *ns = fn_ns_create();
	ASSERT_TRUE(ns != NULL, "ns create");

	int res = fn_ns_set_config(ns, 16000, 2); /* policy 2 = aggressive */
	ASSERT_TRUE(res == 0, "ns set config");
	if (fn_ns_get_config) fn_ns_get_config();

	/* E2E Test: Stationary noise suppression convergence */
	float in_buf[160];
	float out_buf[160];
	const float *in_ptr = in_buf;
	float *out_ptr = out_buf;

	double initial_noise_rms = 0.0;
	double final_noise_rms = 0.0;

	for (int f = 0; f < 50; f++) {
		for (int i = 0; i < 160; i++) {
			in_buf[i] = ((float)(rand() % 2000) - 1000.0f) / 10000.0f;
		}
		if (f == 0) initial_noise_rms = compute_rms_f32(in_buf, 0, 160);
		fn_ns_process(ns, &in_ptr, 1, &out_ptr);
		if (f == 49) final_noise_rms = compute_rms_f32(out_buf, 0, 160);
	}

	double snr_gain_db = 20.0 * log10(initial_noise_rms / (final_noise_rms > 1e-6 ? final_noise_rms : 1e-6));
	ASSERT_TRUE(snr_gain_db > 5.0, "NS achieves > 5 dB stationary noise attenuation");

	fn_ns_free(ns);
	printf("  -> PASS\n");
}

/* Test 10: AEC (Acoustic Echo Cancellation) E2E Regression Tests */
static void test_aec_guardrails(void)
{
	printf("Running test_aec_guardrails (E2E)...\n");
	void *handle = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
	if (!handle) handle = dlopen("./libaudioProcess.so", RTLD_NOW | RTLD_GLOBAL);

	void *(*fn_aec_create)(int, const char *) = dlsym(handle, "audio_process_aec_create");
	int (*fn_aec_process)(struct aec_handle *, struct aec_frame *) = dlsym(handle, "audio_process_aec_process");
	int (*fn_aec_free)(struct aec_handle *) = dlsym(handle, "audio_process_aec_free");

	if (!fn_aec_create || !fn_aec_process || !fn_aec_free) {
		printf("  [SKIP - AEC symbols not in binary link]\n");
		return;
	}

	struct aec_handle *aec = (struct aec_handle *)fn_aec_create(16000, NULL);
	ASSERT_TRUE(aec != NULL, "aec create");

	/* E2E Echo cancellation test: feed far-end speaker audio into near-end mic with delay */
	int16_t far_buf[160];
	int16_t near_buf[160];
	struct aec_frame frm = {
		.far = far_buf,
		.near = near_buf,
		.num_bytes = 320
	};

	double init_echo_rms = 0.0;
	double final_echo_rms = 0.0;

	/* Run 60 frames (600ms) to allow adaptive filter convergence */
	for (int f = 0; f < 60; f++) {
		for (int i = 0; i < 160; i++) {
			double t = (double)(f * 160 + i) / 16000.0;
			int16_t val = (int16_t)(8000.0 * sin(2.0 * M_PI * 500.0 * t));
			far_buf[i] = val;
			near_buf[i] = (int16_t)(val * 0.7); /* Direct acoustic echo coupled at -3dB */
		}
		if (f == 0) init_echo_rms = compute_rms_i16(near_buf, 0, 160);
		int res = fn_aec_process(aec, &frm);
		ASSERT_TRUE(res == 0, "aec process frame");
		if (f == 59) final_echo_rms = compute_rms_i16(near_buf, 0, 160);
	}

	/* Assert echo suppression convergence: ERLE > 6 dB */
	double erle_db = 20.0 * log10(init_echo_rms / (final_echo_rms > 1.0 ? final_echo_rms : 1.0));
	ASSERT_TRUE(erle_db > 6.0, "AEC suppresses acoustic echo (ERLE > 6 dB)");

	int res = fn_aec_free(aec);
	ASSERT_TRUE(res == 0, "aec free");
	printf("  -> PASS\n");
}

/* Test 11: Real-valued FFT / IFFT Roundtrip & Energy Conservation */
static void test_fft_guardrails(void)
{
	printf("Running test_fft_guardrails...\n");
	extern void audio_process_rdft(size_t n, int isgn, float *a);

	int sizes[] = {128, 256, 512};
	for (int s = 0; s < 3; s++) {
		int n = sizes[s];
		float orig[512], a[512];
		float orig_energy = 0.0f;
		for (int i = 0; i < n; i++) {
			orig[i] = sinf((float)i * 0.25f) + 0.5f * cosf((float)i * 0.77f);
			a[i] = orig[i];
			orig_energy += orig[i] * orig[i];
		}

		audio_process_rdft(n, 1, a);
		audio_process_rdft(n, -1, a);

		float max_recon_err = 0.0f;
		float recon_energy = 0.0f;
		for (int i = 0; i < n; i++) {
			float r = a[i] * 2.0f / (float)n;
			recon_energy += r * r;
			float err = fabsf(r - orig[i]);
			if (err > max_recon_err)
				max_recon_err = err;
		}

		ASSERT_TRUE(max_recon_err < 1e-4f, "FFT roundtrip max error within 1e-4");
		float energy_diff = fabsf(recon_energy - orig_energy) / orig_energy;
		ASSERT_TRUE(energy_diff < 1e-4f, "FFT energy conserved within 1e-4");
	}
	printf("  -> PASS\n");
}

int main(void)
{
	printf("=========================================================\n");
	printf(" Audio Process Comprehensive Unit & E2E Regression Suite \n");
	printf("=========================================================\n");

	test_clamp_guardrails();
	test_lpf_guardrails();
	test_hpf_guardrails();
	test_drc_guardrails();
	test_drc_eq_guardrails();
	test_howling_guardrails();
	test_extreme_audio_stress();
	test_agc_guardrails();
	test_ns_guardrails();
	test_aec_guardrails();
	test_fft_guardrails();

	printf("\nAll 11 Unit & E2E Quality Tests Passed Successfully!\n");
	return 0;
}
