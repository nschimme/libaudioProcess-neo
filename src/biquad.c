/* SPDX-License-Identifier: MIT */
#include "biquad.h"
#include "util.h"
#include <math.h>

void biquad_design_peaking(struct biquad *bq, float fs, float freq,
			   float gain_db, float q)
{
	if (q < 0.01f)
		q = 0.01f;
	if (freq < 1.0f || freq >= fs * 0.5f)
		freq = fs * 0.25f;

	float A = powf(10.0f, gain_db / 40.0f);
	float w0 = 2.0f * (float)M_PI * freq / fs;
	float sw = sinf(w0);
	float cw = cosf(w0);
	float alpha = sw / (2.0f * q);

	float a0_inv = 1.0f / (1.0f + alpha / A);
	bq->b0 = (1.0f + alpha * A) * a0_inv;
	bq->b1 = (-2.0f * cw) * a0_inv;
	bq->b2 = (1.0f - alpha * A) * a0_inv;
	bq->a1 = bq->b1;
	bq->a2 = (1.0f - alpha / A) * a0_inv;
	bq->z1 = 0.0f;
	bq->z2 = 0.0f;
}

void biquad_design_notch(struct biquad *bq, float fs, float freq, float bw)
{
	if (freq < 1.0f || freq >= fs * 0.5f)
		freq = fs * 0.25f;
	if (bw < 0.01f)
		bw = 0.01f;

	float w0 = 2.0f * (float)M_PI * freq / fs;
	float sw = sinf(w0);
	if (sw < 1e-10f)
		sw = 1e-10f;
	float cw = cosf(w0);
	float alpha = sw * sinhf(logf(2.0f) / 2.0f * bw * w0 / sw);

	float a0_inv = 1.0f / (1.0f + alpha);
	bq->b0 = a0_inv;
	bq->b1 = (-2.0f * cw) * a0_inv;
	bq->b2 = a0_inv;
	bq->a1 = bq->b1;
	bq->a2 = (1.0f - alpha) * a0_inv;
	bq->z1 = 0.0f;
	bq->z2 = 0.0f;
}

void biquad_reset(struct biquad *bq)
{
	bq->z1 = 0.0f;
	bq->z2 = 0.0f;
}

float biquad_process(struct biquad *bq, float in)
{
	float out = bq->b0 * in + bq->z1;
	bq->z1 = bq->b1 * in - bq->a1 * out + bq->z2;
	bq->z2 = bq->b2 * in - bq->a2 * out;
	return out;
}

int16_t biquad_process_i16(struct biquad *bq, int16_t in)
{
	return clamp_i16(biquad_process(bq, (float)in));
}

void biquad_process_block_f32(struct biquad *bq, const float *in, float *out, int n)
{
	const float b0 = bq->b0;
	const float b1 = bq->b1;
	const float b2 = bq->b2;
	const float a1 = bq->a1;
	const float a2 = bq->a2;
	float z1 = bq->z1;
	float z2 = bq->z2;

	for (int i = 0; i < n; i++) {
		float x = in[i];
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		out[i] = y;
	}

	bq->z1 = z1;
	bq->z2 = z2;
}

void biquad_process_block_i16(struct biquad *bq, const int16_t *in, int16_t *out, int n)
{
	const float b0 = bq->b0;
	const float b1 = bq->b1;
	const float b2 = bq->b2;
	const float a1 = bq->a1;
	const float a2 = bq->a2;
	float z1 = bq->z1;
	float z2 = bq->z2;

	for (int i = 0; i < n; i++) {
		float x = (float)in[i];
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		out[i] = clamp_i16(y);
	}

	bq->z1 = z1;
	bq->z2 = z2;
}
