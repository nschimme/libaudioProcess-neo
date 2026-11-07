/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "audio_process.h"
#include "biquad.h"
#include "webrtc/common_audio/fft4g.h"

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

	/* detection thresholds (dB) */
	float ptpr_thr;
	float papr_thr;
	float pnpr_thr;

	/* notch filters */
	struct biquad notches[HD_MAX_NOTCHES];
	int notch_bins[HD_MAX_NOTCHES];
	int num_notches;

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
	float *fft_buf = s->fft_buf;
	const float *window = s->window;
	for (int i = 0; i < HD_FFT_SIZE; i++)
		fft_buf[i] = (float)frame[i] * window[i];

	WebRtc_rdft(HD_FFT_SIZE, 1, fft_buf, s->fft_ip, s->fft_w);

	float *spectrum = s->spectrum;
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

	const float ptpr_thr = s->ptpr_thr;
	const float papr_thr = s->papr_thr;
	const float pnpr_thr = s->pnpr_thr;
	const float inv_avg = 1.0f / avg;

	int num = 0;
	for (int k = 5; k < HD_HALF_FFT - 6; k++) {
		float mag = spectrum[k];
		if (mag < 1e-10f)
			continue;

		if (20.0f * log10f(mag) < ptpr_thr)
			continue;
		if (20.0f * log10f(mag * inv_avg) < papr_thr)
			continue;

		int pass = 1;
		for (int n = 0; n < PNPR_NUM_CHECKS && pass; n++) {
			float neighbor = spectrum[k + pnpr_offsets[n]];
			if (neighbor < 1e-10f)
				neighbor = 1e-10f;
			if (20.0f * log10f(mag / neighbor) < pnpr_thr)
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

	hd_init_window(s->window, HD_FFT_SIZE);

	return s;
}

void audio_process_hs_process(void *handle, int16_t *data, int num_samples)
{
	struct hd_state *s = handle;
	if (!s || !data || num_samples <= 0)
		return;

	int num_frames = num_samples / HD_FFT_SIZE;

	for (int f = 0; f < num_frames; f++) {
		int16_t *frame = data + f * HD_FFT_SIZE;

		hd_compute_spectrum(s, frame);

		int candidates[HD_HALF_FFT];
		int num_cand = hd_detect_candidates(s, candidates);

		hd_update_persistence(s, candidates, num_cand);
		hd_update_notches(s);

		const float inv_crossfade = 1.0f / (float)HD_CROSSFADE;
		for (int i = 0; i < s->num_notches; i++) {
			struct biquad *notch = &s->notches[i];
			/* Crossfade portion: first HD_CROSSFADE samples */
			for (int j = 0; j < HD_CROSSFADE; j++) {
				int16_t filtered = biquad_process_i16(notch, frame[j]);
				float t = (float)j * inv_crossfade;
				frame[j] = (int16_t)((1.0f - t) * (float)frame[j] + t * (float)filtered);
			}
			/* Remaining steady-state samples: use block processing directly */
			biquad_process_block_i16(notch, frame + HD_CROSSFADE, frame + HD_CROSSFADE,
						 HD_FFT_SIZE - HD_CROSSFADE);
		}
	}
}

void audio_process_hs_free(void *handle)
{
	free(handle);
}
