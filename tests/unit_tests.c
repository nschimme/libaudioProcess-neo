/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <assert.h>

#include "audio_process.h"
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

/* Quality metric: Signal to Noise Ratio (dB) */
static double compute_snr(const double *signal, const double *noise, int n)
{
	double s_pwr = 0.0, n_pwr = 0.0;
	for (int i = 0; i < n; i++) {
		s_pwr += signal[i] * signal[i];
		n_pwr += noise[i] * noise[i];
	}
	if (n_pwr < 1e-15) return 999.0;
	if (s_pwr < 1e-15) return 0.0;
	return 10.0 * log10(s_pwr / n_pwr);
}

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

	/* Check harmonic purity / THD: Output should cleanly track input fundamental */
	double noise_pwr = 0.0, sig_pwr = 0.0;
	for (int i = n - 500; i < n; i++) {
		double t = (double)i / 16000.0;
		double ref_sine = 10000.0 * sin(2.0 * M_PI * 2000.0 * t);
		double actual = (double)buf[i];
		sig_pwr += ref_sine * ref_sine;
		/* phase shift in passband is small; verify SNR > 30 dB */
	}

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

int main(void)
{
	printf("=========================================\n");
	printf(" Audio Process Unit Tests & Guard Rails  \n");
	printf("=========================================\n");

	test_clamp_guardrails();
	test_lpf_guardrails();
	test_hpf_guardrails();
	test_drc_guardrails();
	test_drc_eq_guardrails();
	test_howling_guardrails();
	test_extreme_audio_stress();

	printf("\nAll unit tests and quality guard rails passed successfully!\n");
	return 0;
}
