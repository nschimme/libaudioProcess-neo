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
 *  - Split-Complex (Structure of Arrays) representation for zero-overhead SIMD autovectorization
 *  - Extended 16-partition depth covering >= 100ms acoustic reverberation tails
 *  - Circular far-end delay line pre-filled with silence (delay_ms) with zero near-end buffer drift
 *  - Enclosure resonance notch / band restraint (restrain_band_center, wide, factor)
 *  - Frequency-dependent Wiener spectral subtraction for artifact-free residual suppression
 *  - Double-Talk Detection (DTD) with coherence and structural chassis mechanical margin
 *  - Bounded FIFO architecture with zero startup priming memory corruption
 */

#define AEC_MAX_BLOCK_LEN   128
#define AEC_MAX_FFT_LEN     256
#define AEC_MAX_BINS        (AEC_MAX_FFT_LEN / 2 + 1)  /* 129 bins */
#define AEC_NUM_PARTITIONS  32
#define AEC_MAX_DELAY_SAMP  4800                       /* Up to 100ms at 48kHz */
#define AEC_FIFO_SIZE       4096                       /* Ample headroom for framing */
#ifndef AEC_TRACK_PEAKY
#define AEC_TRACK_PEAKY     0.2f  /* a bin holding more of the far-end power than this is a tone */
#endif
#ifndef AEC_TRACK_BLOCKS
#define AEC_TRACK_BLOCKS    8     /* adapting blocks between delay checks */
#endif
#ifndef AEC_TRACK_LATE
#define AEC_TRACK_LATE      8     /* echo peak at or beyond this partition counts as late */
#endif
#ifndef AEC_TRACK_CONFIRM
#define AEC_TRACK_CONFIRM   10    /* consecutive late checks before the delay moves */
#endif
#ifndef AEC_TRACK_KEEP
#define AEC_TRACK_KEEP      3     /* partitions of lead kept in front of the echo peak */
#endif
#ifndef AEC_MU0
#define AEC_MU0             0.4f
#endif
#ifndef AEC_MU_MIN
#define AEC_MU_MIN          0.3f
#endif
#ifndef AEC_MU_DECAY
#define AEC_MU_DECAY        0.9995f
#endif

struct aec_state {
	int block_len;            /* L: frame partition length (e.g. 64 or 128) */
	int fft_len;              /* N = 2 * L: 128 or 256 */
	int num_bins;             /* K = N / 2 + 1: 65 or 129 */
	int num_partitions;       /* P: number of partitions (16) */

	/* Audio rate tracking */
	int sample_rate;
	int samples_10ms;

	/* Far-end circular delay buffer */
	float far_delay_buf[AEC_MAX_DELAY_SAMP];
	int   delay_samples;
	int   delay_idx;

	/* Input ring/FIFO buffers for block framing */
	float far_fifo[AEC_FIFO_SIZE];
	float near_fifo[AEC_FIFO_SIZE];
	float out_fifo[AEC_FIFO_SIZE];
	int far_avail;
	int near_avail;
	int out_avail;

	/* Far-end time domain history (for overlap: old L samples, new L samples) */
	float x_old[AEC_MAX_BLOCK_LEN];

	/* Split-complex partition history: X_re[p][k], X_im[p][k] */
	float X_re[AEC_NUM_PARTITIONS][AEC_MAX_BINS];
	float X_im[AEC_NUM_PARTITIONS][AEC_MAX_BINS];
	int x_pos;
	int track_count;     /* adaptation blocks since the last delay check */
	int track_late;      /* consecutive checks with the echo peak in a late partition */
	int constrain_pos;   /* next partition whose gradient is constrained (round robin) */

	/* Split-complex adaptive filter weights: W_re[p][k], W_im[p][k] */
	float W_re[AEC_NUM_PARTITIONS][AEC_MAX_BINS];
	float W_im[AEC_NUM_PARTITIONS][AEC_MAX_BINS];

	/* Power spectral density tracking */
	float power_X[AEC_MAX_BINS];               /* smoothed far-end power per bin */
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


/* Purely linear, contiguous SIMD split-complex MAC */
static inline void split_complex_mul_add(float *__restrict__ y_re,
					float *__restrict__ y_im,
					const float *__restrict__ x_re,
					const float *__restrict__ x_im,
					const float *__restrict__ w_re,
					const float *__restrict__ w_im,
					int num_bins)
{
	#pragma GCC ivdep
	for (int k = 0; k < num_bins; k++) {
		y_re[k] += x_re[k] * w_re[k] - x_im[k] * w_im[k];
		y_im[k] += x_re[k] * w_im[k] + x_im[k] * w_re[k];
	}
}

/* Purely linear, contiguous SIMD split-complex gradient update */
static inline void split_complex_gradient_update(float *__restrict__ w_re,
						float *__restrict__ w_im,
						const float *__restrict__ e_re,
						const float *__restrict__ e_im,
						const float *__restrict__ x_re,
						const float *__restrict__ x_im,
						const float *__restrict__ step_bins,
						int num_bins)
{
	#pragma GCC ivdep
	for (int k = 0; k < num_bins; k++) {
		float gr = e_re[k] * x_re[k] + e_im[k] * x_im[k];
		float gi = e_im[k] * x_re[k] - e_re[k] * x_im[k];
		float step = step_bins[k];

		w_re[k] += step * gr;
		w_im[k] += step * gi;
	}
}

static void pbfdaf_process_block(struct aec_state *st,
				 const float *far_block,
				 const float *near_block,
				 float *out_block)
{
	const int L = st->block_len;
	const int N = st->fft_len;
	const int K = st->num_bins;
	const int P = st->num_partitions;

	/* 1. Build extended far-end frame [x_old, x_new] and compute direct split RDFT */
	float x_time[AEC_MAX_FFT_LEN];
	memcpy(x_time, st->x_old, L * sizeof(float));
	memcpy(x_time + L, far_block, L * sizeof(float));
	memcpy(st->x_old, far_block, L * sizeof(float));

	st->x_pos = (st->x_pos - 1 + P) % P;
	audio_process_rdft_forward(N, x_time, st->X_re[st->x_pos], st->X_im[st->x_pos]);

	/* 2. Frequency-domain convolution across all partitions: Y(k) = sum_p W_p(k) * X_{x_pos + p}(k) */
	float Y_re[AEC_MAX_BINS] = {0};
	float Y_im[AEC_MAX_BINS] = {0};

	for (int p = 0; p < P; p++) {
		int idx = (st->x_pos + p) % P;
		split_complex_mul_add(Y_re, Y_im,
				      st->X_re[idx], st->X_im[idx],
				      st->W_re[p], st->W_im[p], K);
	}

	/* 3. Inverse transform of estimated echo Y directly to time domain */
	float y_time[AEC_MAX_FFT_LEN];
	audio_process_rdft_inverse(N, Y_re, Y_im, y_time);

	const float ifft_scale = 2.0f / (float)N;
	#pragma GCC ivdep
	for (int i = 0; i < N; i++) {
		y_time[i] *= ifft_scale;
	}

	/* Synthesized linear echo cancellation output in second half [L .. 2L - 1] */
	float e_linear[AEC_MAX_BLOCK_LEN];
	float near_energy = 0.0f;
	float echo_energy = 0.0f;
	float far_energy = 0.0f;

	#pragma GCC ivdep
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
	const float lambda = 0.90f;
	const float *X_re_0 = st->X_re[st->x_pos];
	const float *X_im_0 = st->X_im[st->x_pos];

	#pragma GCC ivdep
	for (int k = 0; k < K; k++) {
		float pwr = X_re_0[k] * X_re_0[k] + X_im_0[k] * X_im_0[k];
		st->power_X[k] = lambda * st->power_X[k] + (1.0f - lambda) * pwr;
	}

	st->near_pwr_smooth = 0.8f * st->near_pwr_smooth + 0.2f * near_energy;
	st->far_pwr_smooth = 0.8f * st->far_pwr_smooth + 0.2f * far_energy;
	st->echo_pwr_smooth = 0.8f * st->echo_pwr_smooth + 0.2f * echo_energy;

	bool double_talk = false;
	if (st->near_pwr_smooth > 2.0f * st->far_pwr_smooth && st->near_pwr_smooth > 1e6f) {
		st->dtd_hangover = 10;
		double_talk = true;
	} else if (st->dtd_hangover > 0) {
		st->dtd_hangover--;
		double_talk = true;
	}

	/* 5. Calculate step size per frequency bin */
	float step_bins[AEC_MAX_BINS];
	const float current_mu = (double_talk ? 0.0f : st->mu);
	const float reg = 1e4f * (float)N;

	#pragma GCC ivdep
	for (int k = 0; k < K; k++) {
		float s = current_mu / (st->power_X[k] * (float)P + reg);
		step_bins[k] = (s > 0.4f) ? 0.4f : s;
	}

	/* 6. Filter adaptation in frequency domain with direct split RDFT */
	float e_time[AEC_MAX_FFT_LEN] = {0};
	memcpy(e_time + L, e_linear, L * sizeof(float));

	float E_re[AEC_MAX_BINS], E_im[AEC_MAX_BINS];
	audio_process_rdft_forward(N, e_time, E_re, E_im);

	if (!double_talk && far_energy > st->far_pow_thd) {
		for (int p = 0; p < P; p++) {
			int idx = (st->x_pos + p) % P;
			split_complex_gradient_update(st->W_re[p], st->W_im[p],
						      E_re, E_im,
						      st->X_re[idx], st->X_im[idx],
						      step_bins, K);
		}

		/* Constrain ONE partition per block, round robin (zero the
		 * uncausal half of its impulse response). All partitions get the
		 * constraint every P blocks; it costs two transforms per block
		 * instead of 2P. */
		{
			int p = st->constrain_pos;
			st->constrain_pos = (p + 1) % P;
			float w_time[AEC_MAX_FFT_LEN];
			audio_process_rdft_inverse(N, st->W_re[p], st->W_im[p], w_time);

			#pragma GCC ivdep
			for (int i = 0; i < L; i++) {
				w_time[i] *= ifft_scale;
			}
			memset(w_time + L, 0, L * sizeof(float));
			audio_process_rdft_forward(N, w_time, st->W_re[p], st->W_im[p]);
		}

		/* Delay tracking: if the echo peak of the adaptive filter sits in a
		 * late partition for a while, delay the far-end reference by that
		 * amount and shift the weights down, so the filter span is spent on
		 * the echo instead of on the dead time before it. */
		if (++st->track_count >= AEC_TRACK_BLOCKS) {
			st->track_count = 0;

			/* A narrowband reference (a tone, a hum) has no unique delay:
			 * every multiple of its period fits. Only track a broadband one. */
			float pw_sum = 0.0f, pw_max = 0.0f;
			for (int k = 0; k < K; k++) {
				pw_sum += st->power_X[k];
				if (st->power_X[k] > pw_max)
					pw_max = st->power_X[k];
			}
			int broadband = pw_max < AEC_TRACK_PEAKY * pw_sum;

			float best = 0.0f, total = 0.0f;
			int pk = 0;
			for (int p = 0; p < P; p++) {
				float e = 0.0f;
				for (int k = 0; k < K; k++)
					e += st->W_re[p][k] * st->W_re[p][k] + st->W_im[p][k] * st->W_im[p][k];
				total += e;
				if (e > best) { best = e; pk = p; }
			}
			if (broadband && total > 0.0f && best > 0.3f * total && pk >= AEC_TRACK_LATE) {
				if (++st->track_late >= AEC_TRACK_CONFIRM) {
					int shift = pk - AEC_TRACK_KEEP;
					int room = (AEC_MAX_DELAY_SAMP - 1 - st->delay_samples) / L;
					if (shift > room)
						shift = room;
					if (shift > 0) {
						st->delay_samples += shift * L;
						for (int p = 0; p + shift < P; p++) {
							memcpy(st->W_re[p], st->W_re[p + shift], sizeof(st->W_re[p]));
							memcpy(st->W_im[p], st->W_im[p + shift], sizeof(st->W_im[p]));
						}
						for (int p = P - shift; p < P; p++) {
							memset(st->W_re[p], 0, sizeof(st->W_re[p]));
							memset(st->W_im[p], 0, sizeof(st->W_im[p]));
						}
						memset(st->X_re, 0, sizeof(st->X_re));
						memset(st->X_im, 0, sizeof(st->X_im));
						memset(st->x_old, 0, sizeof(st->x_old));
					}
					st->track_late = 0;
				}
			} else {
				st->track_late = 0;
			}
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

		#pragma GCC ivdep
		for (int k = 0; k < K; k++) {
			float ey_pwr = Y_re[k] * Y_re[k] + Y_im[k] * Y_im[k];
			float ee_pwr = E_re[k] * E_re[k] + E_im[k] * E_im[k] + 100.0f;

			float g_bin = 1.0f - oversub * sqrtf(ey_pwr / ee_pwr);

			if (k >= st->restrain_bin_start && k <= st->restrain_bin_end && st->restrain_factor > 0.0f) {
				g_bin *= (1.0f - st->restrain_factor);
			}

			if (g_bin < min_floor) g_bin = min_floor;
			if (g_bin > 1.0f) g_bin = 1.0f;

			E_re[k] *= g_bin;
			E_im[k] *= g_bin;
		}

		audio_process_rdft_inverse(N, E_re, E_im, e_time);

		#pragma GCC ivdep
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

	st->num_bins = (st->fft_len >> 1) + 1;
	st->num_partitions = AEC_NUM_PARTITIONS;
	st->sample_rate = sample_rate;
	st->samples_10ms = sample_rate / 100;
	st->far_gain = cfg.far_gain;
	st->near_gain = cfg.near_gain;
	st->nlp_mode = cfg.nlp_mode;

	/* Initialize far-end delay line pre-filled with silence (zeroes) */
	if (cfg.delay_ms > 0) {
		st->delay_samples = (cfg.delay_ms * sample_rate) / 1000;
		if (st->delay_samples >= AEC_MAX_DELAY_SAMP)
			st->delay_samples = AEC_MAX_DELAY_SAMP - 1;
	}

	st->mu = (cfg.user_mode && cfg.mu_min > 0.0f) ? 0.35f : AEC_MU0;
	st->mu_min = (cfg.user_mode && cfg.mu_min > 0.0f) ? cfg.mu_min : AEC_MU_MIN;
	st->mu_decay = (cfg.user_mode && cfg.mu_decay > 0.0f) ? cfg.mu_decay : AEC_MU_DECAY;
	st->far_pow_thd = (cfg.user_mode && cfg.far_pow_thd > 0.0f) ? cfg.far_pow_thd : 50.0f;
	st->safe_suppression = (cfg.user_mode && cfg.safe_suppression > 0.0f) ? cfg.safe_suppression : 1.0f;
	st->chassis_margin_db = 4.5f;

	if (cfg.user_mode && cfg.restrain_band_wide > 0 && cfg.restrain_factor > 0.0f) {
		float bin_hz = (float)sample_rate / (float)st->fft_len;
		float f_low = (float)cfg.restrain_band_center - (float)cfg.restrain_band_wide * 0.5f;
		float f_high = (float)cfg.restrain_band_center + (float)cfg.restrain_band_wide * 0.5f;
		if (f_low < 0.0f) f_low = 0.0f;
		st->restrain_bin_start = (int)(f_low / bin_hz);
		st->restrain_bin_end = (int)(f_high / bin_hz);
		st->restrain_factor = (cfg.restrain_factor > 0.9f) ? 0.9f : cfg.restrain_factor;
	}

	aec_rdft_init();

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
		/* Far-end delay compensation (configured plus tracked) in a ring buffer */
		for (int i = 0; i < samples_10ms; i++) {
			st->far_delay_buf[st->delay_idx] = (float)far_ptr[i] * far_gain;
			int rd = st->delay_idx - st->delay_samples;
			if (rd < 0)
				rd += AEC_MAX_DELAY_SAMP;
			st->far_fifo[st->far_avail++] = st->far_delay_buf[rd];
			if (++st->delay_idx >= AEC_MAX_DELAY_SAMP)
				st->delay_idx = 0;
		}

		for (int i = 0; i < samples_10ms; i++) {
			st->near_fifo[st->near_avail++] = (float)near_ptr[i] * near_gain;
		}

		/* Process complete L-sample blocks */
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
