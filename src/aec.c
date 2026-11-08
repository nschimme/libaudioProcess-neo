/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "audio_process.h"
#include "audio_process_internal.h"
#include "config.h"
#include "fft.h"
#include "util.h"

/*
 * Modern Clean-Room C11 Partitioned Block Frequency-Domain Adaptive Filter (PBFDAF)
 *
 * Implements high-throughput, low-latency, linear/non-linear Acoustic Echo Cancellation
 * supporting 8 kHz, 16 kHz, 24 kHz, 32 kHz, 44.1 kHz, and 48 kHz.
 *
 * Design features:
 *  - Partitioned Block Frequency-Domain Adaptive Filtering with O(N log N) RDFT execution
 *  - Normalized gradient step-size adaptation (LMS / MDF) with profile parameters (mu_min, mu_decay)
 *  - Hardware delay compensation for DAC-to-ADC propagation latency (delay_ms)
 *  - Hardware acoustic resonance notch / band restraint (restrain_band_center, wide, factor)
 *  - Frequency-dependent Wiener spectral subtraction for artifact-free residual suppression
 *  - Double-Talk Detection (DTD) with coherence and structural chassis mechanical margin
 *  - Arbitrary sample rate support using a unified block-based sub-frame architecture
 */

#define AEC_MAX_BLOCK_LEN   128
#define AEC_MAX_FFT_LEN     256
#define AEC_NUM_PARTITIONS  8
#define AEC_MAX_DELAY_SAMP  4800  /* Up to 100ms at 48kHz */

struct aec_state {
	int block_len;            /* L: frame partition length (e.g. 64 or 128) */
	int fft_len;              /* N = 2 * L: 128 or 256 */
	int num_partitions;       /* P: number of partitions (e.g. 8) */

	/* Audio rate tracking */
	int sample_rate;
	int samples_10ms;

	/* Far-end delay compensation buffer */
	float far_delay_buf[AEC_MAX_DELAY_SAMP + AEC_MAX_BLOCK_LEN * 4];
	int   delay_samples;
	int   far_delay_avail;

	/* Input ring/FIFO buffers for block framing */
	float far_fifo[AEC_MAX_BLOCK_LEN * 8];
	float near_fifo[AEC_MAX_BLOCK_LEN * 8];
	float out_fifo[AEC_MAX_BLOCK_LEN * 8];
	int far_avail;
	int near_avail;
	int out_avail;

	/* Far-end time domain history (for overlap: old L samples, new L samples) */
	float x_old[AEC_MAX_BLOCK_LEN];

	/* Frequency domain partition history: X[p][k] */
	/* In Ooura packing: index 0: DC, 1: Nyquist, 2*k: Re, 2*k+1: -Im */
	float X_freq[AEC_NUM_PARTITIONS][AEC_MAX_FFT_LEN];
	int x_pos;

	/* Adaptive filter weights in frequency domain: W[p][k] */
	float W_freq[AEC_NUM_PARTITIONS][AEC_MAX_FFT_LEN];

	/* Power spectral density tracking */
	float power_X[AEC_MAX_BLOCK_LEN + 1];      /* smoothed far-end power per bin */
	float echo_pwr_smooth;
	float near_pwr_smooth;
	float far_pwr_smooth;

	/* Tuning & Configuration */
	float mu;
	float mu_min;
	float mu_decay;
	float far_pow_thd;
	float safe_suppression;
	float chassis_margin_db;
	int   nlp_mode;

	/* Hardware band restraint (chassis resonance damping) */
	int   restrain_bin_start;
	int   restrain_bin_end;
	float restrain_factor;

	/* DTD state */
	int   dtd_hangover;
	bool  dtd_active;

	/* Gains */
	float far_gain;
	float near_gain;
};

/* Helper: complex multiplication and accumulate in Ooura packed format */
static inline void complex_mul_add(float *__restrict__ y,
				   const float *__restrict__ x,
				   const float *__restrict__ w,
				   int n)
{
	y[0] += x[0] * w[0];
	y[1] += x[1] * w[1];

	int half_n = n >> 1;
	for (int k = 1; k < half_n; k++) {
		int re_idx = 2 * k;
		int im_idx = 2 * k + 1;

		float xr = x[re_idx];
		float xi = -x[im_idx]; /* convert from -Im back to Im */
		float wr = w[re_idx];
		float wi = -w[im_idx];

		float yr = xr * wr - xi * wi;
		float yi = xr * wi + xi * wr;

		y[re_idx] += yr;
		y[im_idx] += -yi; /* store as -Im */
	}
}

/* Helper: complex gradient update: W += step * E * conj(X) */
static inline void complex_gradient_update(float *__restrict__ w,
					   const float *__restrict__ e,
					   const float *__restrict__ x,
					   const float *__restrict__ step_bins,
					   int n)
{
	w[0] += step_bins[0] * e[0] * x[0];
	w[1] += step_bins[n >> 1] * e[1] * x[1];

	int half_n = n >> 1;
	for (int k = 1; k < half_n; k++) {
		int re_idx = 2 * k;
		int im_idx = 2 * k + 1;

		float er = e[re_idx];
		float ei = -e[im_idx];
		float xr = x[re_idx];
		float xi = -x[im_idx]; /* conj(X) -> xr - j*xi */
		float step = step_bins[k];

		float gr = er * xr + ei * xi;
		float gi = ei * xr - er * xi;

		w[re_idx] += step * gr;
		w[im_idx] += -step * gi; /* store as -Im */
	}
}

static void pbfdaf_process_block(struct aec_state *st,
				 const float *far_block,
				 const float *near_block,
				 float *out_block)
{
	int L = st->block_len;
	int N = st->fft_len;
	int P = st->num_partitions;

	/* 1. Build extended far-end frame [x_old, x_new] and compute RDFT */
	float x_time[AEC_MAX_FFT_LEN];
	memcpy(x_time, st->x_old, L * sizeof(float));
	memcpy(x_time + L, far_block, L * sizeof(float));
	memcpy(st->x_old, far_block, L * sizeof(float));

	/* Transform far-end into frequency domain */
	audio_process_rdft(N, 1, x_time);

	/* Store in partition ring buffer */
	st->x_pos = (st->x_pos - 1 + P) % P;
	memcpy(st->X_freq[st->x_pos], x_time, N * sizeof(float));

	/* 2. Frequency-domain convolution across all partitions: Y(k) = sum_p W_p(k) * X_{x_pos + p}(k) */
	float Y_freq[AEC_MAX_FFT_LEN] = {0};
	for (int p = 0; p < P; p++) {
		int idx = (st->x_pos + p) % P;
		complex_mul_add(Y_freq, st->X_freq[idx], st->W_freq[p], N);
	}

	/* 3. Inverse transform of estimated echo Y */
	float y_time[AEC_MAX_FFT_LEN];
	memcpy(y_time, Y_freq, N * sizeof(float));
	audio_process_rdft(N, -1, y_time);

	/* Scale factor for inverse FFT is 2.0 / N */
	float ifft_scale = 2.0f / (float)N;
	for (int i = 0; i < N; i++) {
		y_time[i] *= ifft_scale;
	}

	/* The synthesized linear echo cancellation output is in the second half [L .. 2L - 1] */
	float e_linear[AEC_MAX_BLOCK_LEN];
	float near_energy = 0.0f;
	float echo_energy = 0.0f;
	float far_energy = 0.0f;

	for (int i = 0; i < L; i++) {
		float d_val = near_block[i];
		float y_val = y_time[L + i];
		float e_val = d_val - y_val;
		e_linear[i] = e_val;

		near_energy += d_val * d_val;
		echo_energy += y_val * y_val;
		far_energy += far_block[i] * far_block[i];
	}

	/* 4. Power tracking & Double-Talk Detection (DTD) */
	float lambda = 0.90f;
	int half_n = N >> 1;

	/* Update smoothed far-end bin power: |X|^2 */
	float const *X0 = st->X_freq[st->x_pos];
	st->power_X[0] = lambda * st->power_X[0] + (1.0f - lambda) * (X0[0] * X0[0]);
	st->power_X[half_n] = lambda * st->power_X[half_n] + (1.0f - lambda) * (X0[1] * X0[1]);
	for (int k = 1; k < half_n; k++) {
		float pwr = X0[2 * k] * X0[2 * k] + X0[2 * k + 1] * X0[2 * k + 1];
		st->power_X[k] = lambda * st->power_X[k] + (1.0f - lambda) * pwr;
	}

	st->near_pwr_smooth = 0.8f * st->near_pwr_smooth + 0.2f * near_energy;
	st->far_pwr_smooth = 0.8f * st->far_pwr_smooth + 0.2f * far_energy;
	st->echo_pwr_smooth = 0.8f * st->echo_pwr_smooth + 0.2f * echo_energy;

	/* Coherence / Double-Talk Detection */
	bool double_talk = false;
	if (st->near_pwr_smooth > 2.0f * st->far_pwr_smooth && st->near_pwr_smooth > 1e6f) {
		st->dtd_hangover = 10;
		double_talk = true;
	} else if (st->dtd_hangover > 0) {
		st->dtd_hangover--;
		double_talk = true;
	}

	/* 5. Calculate step size per frequency bin */
	float step_bins[AEC_MAX_BLOCK_LEN + 1];
	float current_mu = (double_talk ? 0.0f : st->mu);
	float reg = 1e4f * (float)N;

	for (int k = 0; k <= half_n; k++) {
		step_bins[k] = current_mu / (st->power_X[k] * (float)P + reg);
		if (step_bins[k] > 0.4f) step_bins[k] = 0.4f;
	}

	/* 6. Filter adaptation in frequency domain */
	float e_time[AEC_MAX_FFT_LEN] = {0};
	memcpy(e_time + L, e_linear, L * sizeof(float));
	audio_process_rdft(N, 1, e_time);

	if (!double_talk && far_energy > st->far_pow_thd) {
		for (int p = 0; p < P; p++) {
			int idx = (st->x_pos + p) % P;
			complex_gradient_update(st->W_freq[p], e_time, st->X_freq[idx], step_bins, N);

			/* Constrained gradient */
			float w_time[AEC_MAX_FFT_LEN];
			memcpy(w_time, st->W_freq[p], N * sizeof(float));
			audio_process_rdft(N, -1, w_time);
			for (int i = 0; i < L; i++) {
				w_time[i] *= ifft_scale;
			}
			memset(w_time + L, 0, L * sizeof(float));
			audio_process_rdft(N, 1, w_time);
			memcpy(st->W_freq[p], w_time, N * sizeof(float));
		}

		if (st->mu > st->mu_min) {
			st->mu *= st->mu_decay;
			if (st->mu < st->mu_min) st->mu = st->mu_min;
		}
	}

	/* 7. Frequency-Domain Wiener Residual Echo Suppression & Band Restraint */
	if (!double_talk && far_energy > st->far_pow_thd) {
		float min_floor;
		float oversub;
		if (st->nlp_mode == 0) {
			min_floor = 0.15f;
			oversub = 1.0f;
		} else if (st->nlp_mode == 1) {
			min_floor = 0.03f;
			oversub = 1.4f;
		} else {
			min_floor = 0.005f;
			oversub = 2.0f;
		}

		if (st->safe_suppression > 0.0f && st->safe_suppression < 1.0f) {
			min_floor *= st->safe_suppression;
		}

		float g_dc = 1.0f - oversub * fabsf(Y_freq[0]) / (fabsf(e_time[0]) + 10.0f);
		if (g_dc < min_floor) g_dc = min_floor;
		if (g_dc > 1.0f) g_dc = 1.0f;
		e_time[0] *= g_dc;

		float g_nyq = 1.0f - oversub * fabsf(Y_freq[1]) / (fabsf(e_time[1]) + 10.0f);
		if (g_nyq < min_floor) g_nyq = min_floor;
		if (g_nyq > 1.0f) g_nyq = 1.0f;
		e_time[1] *= g_nyq;

		for (int k = 1; k < half_n; k++) {
			int r_idx = 2 * k;
			int i_idx = 2 * k + 1;

			float ey_pwr = Y_freq[r_idx] * Y_freq[r_idx] + Y_freq[i_idx] * Y_freq[i_idx];
			float ee_pwr = e_time[r_idx] * e_time[r_idx] + e_time[i_idx] * e_time[i_idx] + 100.0f;

			float g_bin = 1.0f - oversub * sqrtf(ey_pwr / ee_pwr);

			/* Apply hardware enclosure band restraint on resonance frequencies */
			if (k >= st->restrain_bin_start && k <= st->restrain_bin_end && st->restrain_factor > 0.0f) {
				g_bin *= (1.0f - st->restrain_factor);
			}

			if (g_bin < min_floor) g_bin = min_floor;
			if (g_bin > 1.0f) g_bin = 1.0f;

			e_time[r_idx] *= g_bin;
			e_time[i_idx] *= g_bin;
		}

		audio_process_rdft(N, -1, e_time);
		for (int i = 0; i < L; i++) {
			out_block[i] = e_time[L + i] * ifft_scale;
		}
	} else {
		memcpy(out_block, e_linear, L * sizeof(float));
	}
}

void *audio_process_aec_create(int sample_rate, const char *profile_path)
{
	struct aec_handle *h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;

	h->magic = AEC_MAGIC;
	h->sample_rate = sample_rate;

	struct aec_config cfg;
	config_load(profile_path, &cfg);

	h->far_gain = cfg.far_gain;
	h->near_gain = cfg.near_gain;

	struct aec_state *st = calloc(1, sizeof(*st));
	if (!st) {
		free(h);
		return NULL;
	}

	if (sample_rate <= 16000) {
		st->block_len = 64;
		st->fft_len = 128;
	} else {
		st->block_len = 128;
		st->fft_len = 256;
	}

	st->num_partitions = AEC_NUM_PARTITIONS;
	st->sample_rate = sample_rate;
	st->samples_10ms = sample_rate / 100;
	st->far_gain = cfg.far_gain;
	st->near_gain = cfg.near_gain;
	st->nlp_mode = cfg.nlp_mode;

	/* Hardware delay compensation */
	if (cfg.delay_ms > 0) {
		st->delay_samples = (cfg.delay_ms * sample_rate) / 1000;
		if (st->delay_samples > AEC_MAX_DELAY_SAMP)
			st->delay_samples = AEC_MAX_DELAY_SAMP;
	}

	/* Ingenic profile parameters */
	st->mu = (cfg.user_mode && cfg.mu_min > 0.0f) ? 0.35f : 0.25f;
	st->mu_min = (cfg.user_mode && cfg.mu_min > 0.0f) ? cfg.mu_min : 0.05f;
	st->mu_decay = (cfg.user_mode && cfg.mu_decay > 0.0f) ? cfg.mu_decay : 0.998f;
	st->far_pow_thd = (cfg.user_mode && cfg.far_pow_thd > 0.0f) ? cfg.far_pow_thd : 50.0f;
	st->safe_suppression = (cfg.user_mode && cfg.safe_suppression > 0.0f) ? cfg.safe_suppression : 1.0f;
	st->chassis_margin_db = 4.5f;

	/* Hardware chassis acoustic resonance notch / band restraint */
	if (cfg.user_mode && cfg.restrain_band_wide > 0 && cfg.restrain_factor > 0.0f) {
		float bin_hz = (float)sample_rate / (float)st->fft_len;
		float f_low = (float)cfg.restrain_band_center - (float)cfg.restrain_band_wide * 0.5f;
		float f_high = (float)cfg.restrain_band_center + (float)cfg.restrain_band_wide * 0.5f;
		if (f_low < 0.0f) f_low = 0.0f;
		st->restrain_bin_start = (int)(f_low / bin_hz);
		st->restrain_bin_end = (int)(f_high / bin_hz);
		st->restrain_factor = (cfg.restrain_factor > 0.9f) ? 0.9f : cfg.restrain_factor;
	}

	audio_process_rdft(st->fft_len, 1, NULL);

	h->aec = st;
	return h;
}

int audio_process_aec_process(struct aec_handle *h, struct aec_frame *f)
{
	if (!h || h->magic != AEC_MAGIC || !f || !h->aec)
		return -1;

	int bytes_per_10ms = (h->sample_rate * 2) / 100;
	if (bytes_per_10ms == 0)
		return -1;

	struct aec_state *st = (struct aec_state *)h->aec;
	int num_frames = f->num_bytes / bytes_per_10ms;
	int samples_10ms = h->sample_rate / 100;
	int L = st->block_len;

	const float far_gain = h->far_gain;
	const float near_gain = h->near_gain;
	const int16_t *far_ptr = f->far;
	int16_t *near_ptr = f->near;

	for (int frm = 0; frm < num_frames; frm++) {
		/* Apply delay compensation to far-end signal if delay_ms is configured */
		if (st->delay_samples > 0) {
			for (int i = 0; i < samples_10ms; i++) {
				st->far_delay_buf[st->far_delay_avail + i] = (float)far_ptr[i] * far_gain;
			}
			st->far_delay_avail += samples_10ms;

			if (st->far_delay_avail > st->delay_samples) {
				int pull_count = st->far_delay_avail - st->delay_samples;
				if (pull_count > samples_10ms) pull_count = samples_10ms;
				for (int i = 0; i < pull_count; i++) {
					st->far_fifo[st->far_avail + i] = st->far_delay_buf[i];
				}
				st->far_avail += pull_count;
				st->far_delay_avail -= pull_count;
				if (st->far_delay_avail > 0) {
					memmove(st->far_delay_buf, st->far_delay_buf + pull_count, st->far_delay_avail * sizeof(float));
				}
			}
		} else {
			for (int i = 0; i < samples_10ms; i++) {
				st->far_fifo[st->far_avail + i] = (float)far_ptr[i] * far_gain;
			}
			st->far_avail += samples_10ms;
		}

		for (int i = 0; i < samples_10ms; i++) {
			st->near_fifo[st->near_avail + i] = (float)near_ptr[i] * near_gain;
		}
		st->near_avail += samples_10ms;

		while (st->far_avail >= L && st->near_avail >= L) {
			float out_block[AEC_MAX_BLOCK_LEN];
			pbfdaf_process_block(st, st->far_fifo, st->near_fifo, out_block);

			memcpy(st->out_fifo + st->out_avail, out_block, L * sizeof(float));
			st->out_avail += L;

			st->far_avail -= L;
			if (st->far_avail > 0) {
				memmove(st->far_fifo, st->far_fifo + L, st->far_avail * sizeof(float));
			}

			st->near_avail -= L;
			if (st->near_avail > 0) {
				memmove(st->near_fifo, st->near_fifo + L, st->near_avail * sizeof(float));
			}
		}

		if (st->out_avail >= samples_10ms) {
			for (int i = 0; i < samples_10ms; i++) {
				near_ptr[i] = clamp_i16(st->out_fifo[i]);
			}
			st->out_avail -= samples_10ms;
			if (st->out_avail > 0) {
				memmove(st->out_fifo, st->out_fifo + samples_10ms, st->out_avail * sizeof(float));
			}
		} else {
			/* Startup priming: output zero-latency silence for the initial block */
			memset(near_ptr, 0, samples_10ms * sizeof(int16_t));
		}

		far_ptr += samples_10ms;
		near_ptr += samples_10ms;
	}

	return 0;
}

int audio_process_aec_free(struct aec_handle *h)
{
	if (!h || h->magic != AEC_MAGIC)
		return -1;

	if (h->aec) {
		free(h->aec);
		h->aec = NULL;
	}
	free(h);
	return 0;
}
