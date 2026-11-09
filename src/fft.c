/* SPDX-License-Identifier: MIT */
#include "fft.h"
#include <stdint.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Modern Clean-Room Radix-4/2 Real-Valued FFT / IFFT (RDFT)
 *
 * Implements high-throughput, cache-efficient, in-place real transforms
 * using pre-computed twiddle tables and branchless bit-reversal permutations.
 *
 * Supported sizes: 128, 256, 512, 1024.
 */

#define MAX_FFT_N 1024
#define MAX_FFT_M (MAX_FFT_N / 2)

static uint8_t pair_64_i[32], pair_64_j[32];
static int n_pairs_64 = 0;

static uint8_t pair_128_i[64], pair_128_j[64];
static int n_pairs_128 = 0;

static uint16_t pair_256_i[128], pair_256_j[128];
static int n_pairs_256 = 0;

static uint16_t pair_512_i[256], pair_512_j[256];
static int n_pairs_512 = 0;

static float tw_stage_fwd_re[512];
static float tw_stage_fwd_im[512];
static float tw_stage_inv_im[512];

static float tw_r_re[MAX_FFT_N / 4 + 1];
static float tw_r_im[MAX_FFT_N / 4 + 1];

static int tables_initialized = 0;

static void init_tables(void)
{
	if (tables_initialized)
		return;

	n_pairs_64 = 0;
	for (uint32_t i = 0; i < 64; i++) {
		uint32_t r = 0;
		for (int b = 0; b < 6; b++) {
			if (i & (1 << b))
				r |= (1 << (5 - b));
		}
		if (i < r) {
			pair_64_i[n_pairs_64] = (uint8_t)i;
			pair_64_j[n_pairs_64] = (uint8_t)r;
			n_pairs_64++;
		}
	}

	n_pairs_128 = 0;
	for (uint32_t i = 0; i < 128; i++) {
		uint32_t r = 0;
		for (int b = 0; b < 7; b++) {
			if (i & (1 << b))
				r |= (1 << (6 - b));
		}
		if (i < r) {
			pair_128_i[n_pairs_128] = (uint8_t)i;
			pair_128_j[n_pairs_128] = (uint8_t)r;
			n_pairs_128++;
		}
	}

	n_pairs_256 = 0;
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t r = 0;
		for (int b = 0; b < 8; b++) {
			if (i & (1 << b))
				r |= (1 << (7 - b));
		}
		if (i < r) {
			pair_256_i[n_pairs_256] = (uint16_t)i;
			pair_256_j[n_pairs_256] = (uint16_t)r;
			n_pairs_256++;
		}
	}

	n_pairs_512 = 0;
	for (uint32_t i = 0; i < 512; i++) {
		uint32_t r = 0;
		for (int b = 0; b < 9; b++) {
			if (i & (1 << b))
				r |= (1 << (8 - b));
		}
		if (i < r) {
			pair_512_i[n_pairs_512] = (uint16_t)i;
			pair_512_j[n_pairs_512] = (uint16_t)r;
			n_pairs_512++;
		}
	}

	int off = 0;
	for (int len = 4; len <= 512; len <<= 1) {
		int half = len >> 1;
		for (int j = 0; j < half; j++) {
			double angle = -2.0 * M_PI * (double)j / (double)len;
			tw_stage_fwd_re[off + j] = (float)cos(angle);
			tw_stage_fwd_im[off + j] = (float)sin(angle);
			tw_stage_inv_im[off + j] = (float)(-sin(angle));
		}
		off += half;
	}

	for (size_t k = 0; k <= MAX_FFT_N / 4; k++) {
		double angle = -2.0 * M_PI * (double)k / (double)MAX_FFT_N;
		tw_r_re[k] = (float)cos(angle);
		tw_r_im[k] = (float)sin(angle);
	}

	tables_initialized = 1;
}

static inline void bit_reverse_permute(float *__restrict__ a, size_t m)
{
	if (m == 64) {
		for (int p = 0; p < n_pairs_64; p++) {
			size_t i = pair_64_i[p];
			size_t j = pair_64_j[p];
			float tr0 = a[2 * i], ti0 = a[2 * i + 1];
			a[2 * i] = a[2 * j];
			a[2 * i + 1] = a[2 * j + 1];
			a[2 * j] = tr0;
			a[2 * j + 1] = ti0;
		}
	} else if (m == 128) {
		for (int p = 0; p < n_pairs_128; p++) {
			size_t i = pair_128_i[p];
			size_t j = pair_128_j[p];
			float tr0 = a[2 * i], ti0 = a[2 * i + 1];
			a[2 * i] = a[2 * j];
			a[2 * i + 1] = a[2 * j + 1];
			a[2 * j] = tr0;
			a[2 * j + 1] = ti0;
		}
	} else if (m == 256) {
		for (int p = 0; p < n_pairs_256; p++) {
			size_t i = pair_256_i[p];
			size_t j = pair_256_j[p];
			float tr0 = a[2 * i], ti0 = a[2 * i + 1];
			a[2 * i] = a[2 * j];
			a[2 * i + 1] = a[2 * j + 1];
			a[2 * j] = tr0;
			a[2 * j + 1] = ti0;
		}
	} else if (m == 512) {
		for (int p = 0; p < n_pairs_512; p++) {
			size_t i = pair_512_i[p];
			size_t j = pair_512_j[p];
			float tr0 = a[2 * i], ti0 = a[2 * i + 1];
			a[2 * i] = a[2 * j];
			a[2 * i + 1] = a[2 * j + 1];
			a[2 * j] = tr0;
			a[2 * j + 1] = ti0;
		}
	}
}

static inline void radix4_stage1_2(float *__restrict__ a, size_t m, int isgn)
{
	float s = (isgn > 0 ? 1.0f : -1.0f);
	float *p = a;
	float *p_end = a + 2 * m;

	while (p < p_end) {
		float r0 = p[0] + p[2], i0 = p[1] + p[3];
		float r1 = p[0] - p[2], i1 = p[1] - p[3];
		float r2 = p[4] + p[6], i2 = p[5] + p[7];
		float r3 = p[4] - p[6], i3 = p[5] - p[7];

		p[0] = r0 + r2;
		p[1] = i0 + i2;
		p[4] = r0 - r2;
		p[5] = i0 - i2;

		float w3_re = s * i3;
		float w3_im = -s * r3;

		p[2] = r1 + w3_re;
		p[3] = i1 + w3_im;
		p[6] = r1 - w3_re;
		p[7] = i1 - w3_im;

		p += 8;
	}
}

static void complex_fft(float *__restrict__ a, size_t m, int isgn)
{
	bit_reverse_permute(a, m);

	/* Fused Stages 1 & 2 (len = 4) */
	radix4_stage1_2(a, m, isgn);

	/* Stages len = 8, 16, ... m */
	const float *__restrict__ tw_im_table = (isgn > 0 ? tw_stage_fwd_im : tw_stage_inv_im);
	int off = 2;

	for (size_t len = 8; len <= m; len <<= 1) {
		size_t half_len = len >> 1;
		const float *__restrict__ tw_r = tw_stage_fwd_re + off;
		const float *__restrict__ tw_i = tw_im_table + off;

		for (size_t i = 0; i < m; i += len) {
			float *__restrict__ p_u = a + 2 * i;
			float *__restrict__ p_v = a + 2 * (i + half_len);

			/* j = 0: W = (1, 0), zero multiplies */
			float u0_re = p_u[0], u0_im = p_u[1];
			float v0_re = p_v[0], v0_im = p_v[1];
			p_u[0] = u0_re + v0_re;
			p_u[1] = u0_im + v0_im;
			p_v[0] = u0_re - v0_re;
			p_v[1] = u0_im - v0_im;

			size_t j = 1;
			for (; j + 1 < half_len; j += 2) {
				float w0_re = tw_r[j],     w0_im = tw_i[j];
				float w1_re = tw_r[j + 1], w1_im = tw_i[j + 1];

				float u0_r = p_u[2 * j],     u0_i = p_u[2 * j + 1];
				float v0_r = p_v[2 * j],     v0_i = p_v[2 * j + 1];
				float u1_r = p_u[2 * j + 2], u1_i = p_u[2 * j + 3];
				float v1_r = p_v[2 * j + 2], v1_i = p_v[2 * j + 3];

				float vw0_re = v0_r * w0_re - v0_i * w0_im;
				float vw0_im = v0_r * w0_im + v0_i * w0_re;
				float vw1_re = v1_r * w1_re - v1_i * w1_im;
				float vw1_im = v1_r * w1_im + v1_i * w1_re;

				p_u[2 * j]     = u0_r + vw0_re;
				p_u[2 * j + 1] = u0_i + vw0_im;
				p_v[2 * j]     = u0_r - vw0_re;
				p_v[2 * j + 1] = u0_i - vw0_im;

				p_u[2 * j + 2] = u1_r + vw1_re;
				p_u[2 * j + 3] = u1_i + vw1_im;
				p_v[2 * j + 2] = u1_r - vw1_re;
				p_v[2 * j + 3] = u1_i - vw1_im;
			}
			if (j < half_len) {
				float w_re = tw_r[j], w_im = tw_i[j];
				float u_re = p_u[2 * j], u_im = p_u[2 * j + 1];
				float v_re = p_v[2 * j], v_im = p_v[2 * j + 1];
				float vw_re = v_re * w_re - v_im * w_im;
				float vw_im = v_re * w_im + v_im * w_re;
				p_u[2 * j]     = u_re + vw_re;
				p_u[2 * j + 1] = u_im + vw_im;
				p_v[2 * j]     = u_re - vw_re;
				p_v[2 * j + 1] = u_im - vw_im;
			}
		}
		off += half_len;
	}
}

void audio_process_rdft(size_t n, int isgn, float *a)
{
	if (!a || (n != 128 && n != 256 && n != 512 && n != 1024))
		return;

	if (!tables_initialized)
		init_tables();

	size_t m = n >> 1;
	size_t step = MAX_FFT_N / n;

	if (isgn >= 0) {
		complex_fft(a, m, 1);
		float z0_re = a[0], z0_im = a[1];
		a[0] = z0_re + z0_im;
		a[1] = z0_re - z0_im;

		size_t half_m = m >> 1;
		float *__restrict__ p_k = a + 2;
		float *__restrict__ p_sym = a + 2 * (m - 1);
		const float *__restrict__ tw_r = tw_r_re + step;
		const float *__restrict__ tw_i = tw_r_im + step;

		for (size_t k = 1; k <= half_m; k++) {
			float re_k = p_k[0], im_k = p_k[1];
			float re_sym = p_sym[0], im_sym = p_sym[1];

			float e_re = 0.5f * (re_k + re_sym);
			float e_im = 0.5f * (im_k - im_sym);
			float o_re = 0.5f * (im_k + im_sym);
			float o_im = -0.5f * (re_k - re_sym);

			float w_re = *tw_r;
			float w_im = *tw_i;

			float wo_re = w_re * o_re - w_im * o_im;
			float wo_im = w_re * o_im + w_im * o_re;

			p_k[0] = e_re + wo_re;
			p_k[1] = -(e_im + wo_im);
			if (k < m - k) {
				p_sym[0] = e_re - wo_re;
				p_sym[1] = -(-e_im + wo_im);
			}

			p_k += 2;
			p_sym -= 2;
			tw_r += step;
			tw_i += step;
		}
	} else {
		float dc = a[0], nyq = a[1];
		float z0_re = 0.5f * (dc + nyq);
		float z0_im = 0.5f * (dc - nyq);

		size_t half_m = m >> 1;
		float *__restrict__ p_k = a + 2;
		float *__restrict__ p_sym = a + 2 * (m - 1);
		const float *__restrict__ tw_r = tw_r_re + step;
		const float *__restrict__ tw_i = tw_r_im + step;

		for (size_t k = 1; k <= half_m; k++) {
			float xk_re = p_k[0], xk_im = -p_k[1];
			float xm_re = p_sym[0], xm_im = -p_sym[1];

			float e_re = 0.5f * (xk_re + xm_re);
			float e_im = 0.5f * (xk_im - xm_im);
			float wo_re = 0.5f * (xk_re - xm_re);
			float wo_im = 0.5f * (xk_im + xm_im);

			float w_re = *tw_r;
			float w_im = *tw_i;

			float o_re = w_re * wo_re + w_im * wo_im;
			float o_im = w_re * wo_im - w_im * wo_re;

			p_k[0] = e_re - o_im;
			p_k[1] = e_im + o_re;
			if (k < m - k) {
				p_sym[0] = e_re + o_im;
				p_sym[1] = -(e_im - o_re);
			}

			p_k += 2;
			p_sym -= 2;
			tw_r += step;
			tw_i += step;
		}
		a[0] = z0_re;
		a[1] = z0_im;

		complex_fft(a, m, -1);
	}
}

void audio_process_rdft_forward(size_t n, const float *__restrict__ in_time,
				float *__restrict__ out_re, float *__restrict__ out_im)
{
	if (!in_time || !out_re || !out_im)
		return;

	if (!tables_initialized)
		init_tables();

	float a[MAX_FFT_N];
	memcpy(a, in_time, n * sizeof(float));

	size_t m = n >> 1;
	size_t step = MAX_FFT_N / n;

	complex_fft(a, m, 1);
	float z0_re = a[0], z0_im = a[1];
	out_re[0] = z0_re + z0_im;
	out_im[0] = 0.0f;
	out_re[m] = z0_re - z0_im;
	out_im[m] = 0.0f;

	size_t half_m = m >> 1;
	float *__restrict__ p_k = a + 2;
	float *__restrict__ p_sym = a + 2 * (m - 1);
	const float *__restrict__ tw_r = tw_r_re + step;
	const float *__restrict__ tw_i = tw_r_im + step;

	for (size_t k = 1; k <= half_m; k++) {
		float re_k = p_k[0], im_k = p_k[1];
		float re_sym = p_sym[0], im_sym = p_sym[1];

		float e_re = 0.5f * (re_k + re_sym);
		float e_im = 0.5f * (im_k - im_sym);
		float o_re = 0.5f * (im_k + im_sym);
		float o_im = -0.5f * (re_k - re_sym);

		float w_re = *tw_r;
		float w_im = *tw_i;

		float wo_re = w_re * o_re - w_im * o_im;
		float wo_im = w_re * o_im + w_im * o_re;

		out_re[k] = e_re + wo_re;
		out_im[k] = e_im + wo_im;
		if (k < m - k) {
			out_re[m - k] = e_re - wo_re;
			out_im[m - k] = -(e_im - wo_im);
		}

		p_k += 2;
		p_sym -= 2;
		tw_r += step;
		tw_i += step;
	}
}

void audio_process_rdft_inverse(size_t n, const float *__restrict__ in_re,
				const float *__restrict__ in_im, float *__restrict__ out_time)
{
	if (!in_re || !in_im || !out_time)
		return;

	float a[MAX_FFT_N];
	size_t half_n = n >> 1;
	a[0] = in_re[0];
	a[1] = in_re[half_n];

	for (size_t k = 1; k < half_n; k++) {
		a[2 * k] = in_re[k];
		a[2 * k + 1] = -in_im[k];
	}

	audio_process_rdft(n, -1, a);
	memcpy(out_time, a, n * sizeof(float));
}

/* Drop-in replacements for WebRTC legacy symbols */

void WebRtc_rdft(size_t n, int isgn, float *a, size_t *ip, float *w)
{
	(void)ip;
	(void)w;
	audio_process_rdft(n, isgn, a);
}

void aec_rdft_init(void)
{
	init_tables();
}

void aec_rdft_forward_128(float *a)
{
	audio_process_rdft(128, 1, a);
}

void aec_rdft_inverse_128(float *a)
{
	audio_process_rdft(128, -1, a);
}
