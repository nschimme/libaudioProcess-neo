/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <stdint.h>

#include "audio_process.h"
#include "audio_process_internal.h"
#include "config.h"
#include "util.h"
#include "webrtc/modules/audio_processing/aec/include/echo_cancellation.h"
#include "webrtc/modules/audio_processing/aec/aec_core.h"
#include "webrtc/modules/audio_processing/aec/aec_core_internal.h"

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

	h->aec = WebRtcAec_Create();
	if (!h->aec)
		goto fail;

	if (WebRtcAec_Init(h->aec, sample_rate, sample_rate) != 0)
		goto fail;

	AecConfig aec_cfg = {
		.nlpMode = cfg.nlp_mode,
		.skewMode = cfg.drift_compensation ? kAecTrue : kAecFalse,
		.metricsMode = cfg.metrics ? kAecTrue : kAecFalse,
		.delay_logging = cfg.delay_logging ? kAecTrue : kAecFalse
	};
	WebRtcAec_set_config(h->aec, aec_cfg);

	struct AecCore *core = WebRtcAec_aec_core(h->aec);
	WebRtcAec_enable_delay_agnostic(core, cfg.delay_agnostic ? 1 : 0);
	WebRtcAec_enable_extended_filter(core, cfg.extended_filter ? 1 : 0);

	if (cfg.user_mode) {
		core->ingenic_ext_enabled = 1;
		core->mu_min = cfg.mu_min;
		core->mu_decay = cfg.mu_decay;
		core->mu_current = core->normal_mu;
		core->cor_thd1 = cfg.cor_thd1;
		core->cor_thd2 = cfg.cor_thd2;
		core->cor_thd3 = cfg.cor_thd3;
		core->cor_thd4 = cfg.cor_thd4;
		core->far_pow_thd = cfg.far_pow_thd;
		core->safe_suppression = cfg.safe_suppression;
		core->restrain_band_center = cfg.restrain_band_center;
		core->restrain_band_wide = cfg.restrain_band_wide;
		core->restrain_factor = cfg.restrain_factor;
	}

	return h;
fail:
	if (h->aec)
		WebRtcAec_Free(h->aec);
	free(h);
	return NULL;
}

int audio_process_aec_process(struct aec_handle *h, struct aec_frame *f)
{
	if (!h || h->magic != AEC_MAGIC || !f)
		return -1;

	int bytes_per_10ms = (h->sample_rate * 2) / 100;
	if (bytes_per_10ms == 0)
		return -1;

	int num_frames = f->num_bytes / bytes_per_10ms;
	int samples_10ms = h->sample_rate / 100;
	float far_f[MAX_SAMPLES_10MS];
	float near_f[MAX_SAMPLES_10MS];
	float out_f[MAX_SAMPLES_10MS];

	const float far_gain = h->far_gain;
	const float near_gain = h->near_gain;
	const int samples_per_frame = bytes_per_10ms / 2;
	const int16_t *far_ptr = f->far;
	int16_t *near_ptr = f->near;

	for (int i = 0; i < num_frames; i++) {
		for (int j = 0; j < samples_10ms; j++)
			far_f[j] = (float)far_ptr[j] * far_gain;

		WebRtcAec_BufferFarend(h->aec, far_f, (size_t)samples_10ms);

		for (int j = 0; j < samples_10ms; j++)
			near_f[j] = (float)near_ptr[j] * near_gain;

		const float *near_pp = near_f;
		float *out_pp = out_f;
		WebRtcAec_Process(h->aec, &near_pp, 1, &out_pp,
				  (size_t)samples_10ms, 0, 0);

		for (int j = 0; j < samples_10ms; j++)
			near_ptr[j] = clamp_i16(out_f[j]);

		far_ptr += samples_per_frame;
		near_ptr += samples_per_frame;
	}

	return 0;
}

int audio_process_aec_free(struct aec_handle *h)
{
	if (!h || h->magic != AEC_MAGIC)
		return -1;

	WebRtcAec_Free(h->aec);
	free(h);
	return 0;
}
