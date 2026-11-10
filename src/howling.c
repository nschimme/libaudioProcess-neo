/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "audio_process.h"
#include "biquad.h"
#include "fft.h"

#define HD_FFT_SIZE    128
#define HD_HALF_FFT    64
#define HD_MAX_NOTCHES 8
#define HD_PERSIST_THR 3
#define HD_TTL_MAX     1000
#define HD_CROSSFADE   32

/* PNPR neighbor offsets: +-1 and +-4 bins */
static const int pnpr_offsets[] = {-1, -4, 3, 6};
#define PNPR_NUM_CHECKS 4

struct hd_state {
	int sample_rate;

	/* analysis */
	float window[HD_FFT_SIZE];
	float spectrum[HD_HALF_FFT];
	int persist[HD_HALF_FFT];

	/* detection thresholds (dB and linear factors) */
	float ptpr_thr;
	float papr_thr;
	float pnpr_thr;
	float ptpr_lin;
	float papr_lin;
	float pnpr_lin;

	/* notch filters */
	struct biquad notches[HD_MAX_NOTCHES];
	int notch_bins[HD_MAX_NOTCHES];
	int num_notches;

	/* streaming: input of the block being collected, and its fill level */
	int16_t raw[HD_FFT_SIZE];
	int pos;

	/* FFT workspace (auto-initialized when ip[0]==0) */
	float fft_buf[HD_FFT_SIZE];
	size_t fft_ip[10]; /* 2 + sqrt(HD_FFT_SIZE/2) */
	float fft_w[HD_HALF_FFT];
};

static void hd_init_window(float *w, int n)
{
	for (int i = 0; i < n; i++)
		w[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1)));
}

static void hd_compute_spectrum(struct hd_state *s, const int16_t *frame)
{
	float * __restrict__ fft_buf = s->fft_buf;
	const float * __restrict__ window = s->window;
	const int16_t * __restrict__ frm = frame;

	/* Unroll 4x for vector-pipeline parallelism */
	for (int i = 0; i < HD_FFT_SIZE; i += 4) {
		fft_buf[i + 0] = (float)frm[i + 0] * window[i + 0];
		fft_buf[i + 1] = (float)frm[i + 1] * window[i + 1];
		fft_buf[i + 2] = (float)frm[i + 2] * window[i + 2];
		fft_buf[i + 3] = (float)frm[i + 3] * window[i + 3];
	}

	WebRtc_rdft(HD_FFT_SIZE, 1, fft_buf, s->fft_ip, s->fft_w);

	float * __restrict__ spectrum = s->spectrum;
	spectrum[0] = fabsf(fft_buf[0]);
	for (int k = 1; k < HD_HALF_FFT; k++) {
		float re = fft_buf[2 * k];
		float im = fft_buf[2 * k + 1];
		spectrum[k] = sqrtf(re * re + im * im);
	}
}

static int hd_detect_candidates(struct hd_state *s, int *candidates)
{
	const float *spectrum = s->spectrum;
	float avg = 0.0f;
	for (int k = 0; k < HD_HALF_FFT; k++)
		avg += spectrum[k];
	avg *= (1.0f / (float)HD_HALF_FFT);
	if (avg < 1e-10f)
		avg = 1e-10f;

	const float ptpr_lin = s->ptpr_lin;
	const float papr_threshold = avg * s->papr_lin;
	const float pnpr_lin = s->pnpr_lin;

	int num = 0;
	for (int k = 5; k < HD_HALF_FFT - 6; k++) {
		float mag = spectrum[k];
		if (mag < 1e-10f)
			continue;

		/* 20*log10(mag) < ptpr_thr <=> mag < 10^(ptpr_thr/20) */
		if (mag < ptpr_lin)
			continue;
		/* 20*log10(mag/avg) < papr_thr <=> mag < avg * 10^(papr_thr/20) */
		if (mag < papr_threshold)
			continue;

		int pass = 1;
		for (int n = 0; n < PNPR_NUM_CHECKS && pass; n++) {
			float neighbor = spectrum[k + pnpr_offsets[n]];
			if (neighbor < 1e-10f)
				neighbor = 1e-10f;
			/* 20*log10(mag/neighbor) < pnpr_thr <=> mag < neighbor * 10^(pnpr_thr/20) */
			if (mag < neighbor * pnpr_lin)
				pass = 0;
		}

		if (pass && num < HD_HALF_FFT)
			candidates[num++] = k;
	}

	return num;
}

static void hd_update_persistence(struct hd_state *s, const int *candidates,
				  int num_cand)
{
	for (int k = 0; k < HD_HALF_FFT; k++) {
		int found = 0;
		for (int c = 0; c < num_cand; c++) {
			if (candidates[c] == k) {
				found = 1;
				break;
			}
		}

		if (found) {
			if (s->persist[k] < HD_TTL_MAX)
				s->persist[k]++;
		} else if (s->persist[k] > 0) {
			s->persist[k]--;
		}
	}
}

static void hd_update_notches(struct hd_state *s)
{
	int new_bins[HD_MAX_NOTCHES];
	int new_num = 0;

	for (int k = 5; k < HD_HALF_FFT - 1 && new_num < HD_MAX_NOTCHES; k++) {
		if (s->persist[k] >= HD_PERSIST_THR)
			new_bins[new_num++] = k;
	}

	if (new_num == s->num_notches &&
	    memcmp(new_bins, s->notch_bins, (size_t)new_num * sizeof(int)) == 0)
		return;

	s->num_notches = new_num;
	memcpy(s->notch_bins, new_bins, (size_t)new_num * sizeof(int));

	float bin_hz = (float)s->sample_rate / HD_FFT_SIZE;
	for (int i = 0; i < new_num; i++)
		biquad_design_notch(&s->notches[i],
				    (float)s->sample_rate,
				    (float)new_bins[i] * bin_hz, 0.5f);
}

void *audio_process_hs_create(int sample_rate, int frame_size)
{
	(void)frame_size;

	struct hd_state *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;

	s->sample_rate = sample_rate > 0 ? sample_rate : 8000;
	s->ptpr_thr = 30.0f;
	s->papr_thr = 15.0f;
	s->pnpr_thr = 10.0f;
	s->ptpr_lin = powf(10.0f, s->ptpr_thr / 20.0f);
	s->papr_lin = powf(10.0f, s->papr_thr / 20.0f);
	s->pnpr_lin = powf(10.0f, s->pnpr_thr / 20.0f);

	hd_init_window(s->window, HD_FFT_SIZE);

	return s;
}

/*
 * Apply the active notches to n samples that sit at positions pos..pos+n-1 of
 * the current analysis block. The first HD_CROSSFADE positions of a block fade
 * the filtered signal in, as before; the rest use the block filter.
 */
static void hd_filter(struct hd_state *s, int16_t *buf, int pos, int n)
{
	const float inv_crossfade = 1.0f / (float)HD_CROSSFADE;

	for (int i = 0; i < s->num_notches; i++) {
		struct biquad *notch = &s->notches[i];
		int j = 0;

		for (; j < n && pos + j < HD_CROSSFADE; j++) {
			int16_t filtered = biquad_process_i16(notch, buf[j]);
			float t = (float)(pos + j) * inv_crossfade;
			buf[j] = (int16_t)((1.0f - t) * (float)buf[j] + t * (float)filtered);
		}
		if (j < n)
			biquad_process_block_i16(notch, buf + j, buf + j, n - j);
	}
}

/*
 * Two call shapes are accepted (see docs, section 4.6):
 *  - vendor libimp: hs_process(handle, buf, buf, num_bytes): the same pointer
 *    twice (in place) and a byte count;
 *  - this library's and openimp's original shape: hs_process(handle, data,
 *    num_samples), where the third argument is a small count.
 * The vendor call is recognised by pointer equality, which holds wherever the
 * buffer lives (on MIPS32 a heap buffer can sit below 16 MB). Otherwise a
 * third argument below HS_MAX_COUNT is a sample count (real counts are a few
 * thousand), and anything larger is read as a distinct output pointer with the
 * fourth argument in bytes.
 *
 * Any length is processed: samples are filtered as they arrive, the
 * 128-sample analysis block fills across calls, and the notches found from a
 * completed block apply from the next sample on.
 */
#define HS_MAX_COUNT 65536
void audio_process_hs_process(void *handle, int16_t *data, uintptr_t arg2,
			      uintptr_t arg3)
{
	struct hd_state *s = handle;
	if (!s || !data)
		return;

	int n;
	if (arg2 == (uintptr_t)data) {
		n = (int)(arg3 / sizeof(int16_t));
	} else if (arg2 < HS_MAX_COUNT) {
		n = (int)arg2;
	} else {
		int16_t *out = (int16_t *)arg2;
		n = (int)(arg3 / sizeof(int16_t));
		if (n > 0)
			memmove(out, data, (size_t)n * sizeof(int16_t));
		data = out;
	}
	if (n <= 0)
		return;

	while (n > 0) {
		int take = HD_FFT_SIZE - s->pos;
		if (take > n)
			take = n;

		memcpy(s->raw + s->pos, data, (size_t)take * sizeof(int16_t));
		hd_filter(s, data, s->pos, take);
		s->pos += take;
		data += take;
		n -= take;

		if (s->pos == HD_FFT_SIZE) {
			int candidates[HD_HALF_FFT];

			hd_compute_spectrum(s, s->raw);
			int num_cand = hd_detect_candidates(s, candidates);
			hd_update_persistence(s, candidates, num_cand);
			hd_update_notches(s);
			s->pos = 0;
		}
	}
}

void audio_process_hs_free(void *handle)
{
	free(handle);
}
