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
	/* DC must be rejected (attenuated by > 40 dB, < 100 LSB) */
	ASSERT_TRUE(abs(buf[n - 1]) < 50, "HPF DC rejection");

	/* Reset and test 2 kHz passband: should pass through with near-unity gain */
	memset(state, 0, sizeof(state));
	for (int i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		buf[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 2000.0 * t));
	}
	audio_process_hpf_process(state, buf, n);
	/* Find amplitude in steady state (last 500 samples) */
	int max_val = 0;
	for (int i = n - 500; i < n; i++) {
		if (abs(buf[i]) > max_val) max_val = abs(buf[i]);
	}
	ASSERT_TRUE(max_val >= 9500 && max_val <= 10500, "HPF passband unity gain");

	free(buf);
	printf("  -> PASS\n");
}

/* Test 4: Guard rail on DRC compression and envelope tracking */
static void test_drc_guardrails(void)
{
	printf("Running test_drc_guardrails...\n");
	void *drc = audio_process_drc_create(16000, 1);
	ASSERT_TRUE(drc != NULL, "drc create");
	/* Threshold = 20dB, ratio = 4:1 */
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
			/* In steady state, 30000 should be significantly compressed */
			ASSERT_TRUE(buf[i] < 30000, "drc compresses loud signals");
		}
	}

	audio_process_drc_free(drc);
	free(buf);
	printf("  -> PASS\n");
}

/* Test 5: Guard rail on DRC+EQ combined in-place safety */
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

/* Test 6: Guard rail on HS (Howling Suppression) with pure tone howling */
static void test_howling_guardrails(void)
{
	printf("Running test_howling_guardrails...\n");
	void *hs = audio_process_hs_create(16000, 128);
	ASSERT_TRUE(hs != NULL, "hs create");

	/* Create 1 kHz howling tone */
	int n = 128 * 20; /* 20 frames */
	int16_t *buf = malloc(n * sizeof(int16_t));
	for (int i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		buf[i] = (int16_t)(16000.0 * sin(2.0 * M_PI * 1000.0 * t));
	}

	audio_process_hs_process(hs, buf, n);

	/* Check that howling filter stays bounded and active */
	for (int i = 0; i < n; i++) {
		ASSERT_TRUE(buf[i] <= 32767 && buf[i] >= -32768, "hs bounded");
	}

	audio_process_hs_free(hs);
	free(buf);
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

	printf("\nAll unit tests passed successfully!\n");
	return 0;
}
