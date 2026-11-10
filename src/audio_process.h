/* SPDX-License-Identifier: MIT */
#ifndef AUDIO_PROCESS_H
#define AUDIO_PROCESS_H

#include <stdint.h>
#include <stddef.h>

/* WebRtcAgcConfig definition matching legacy Ingenic ABI */
typedef struct {
	int16_t targetLevelDbfs;
	int16_t compressionGaindB;
	uint8_t limiterEnable;
} WebRtcAgcConfig;

#ifdef __cplusplus
extern "C" {
#endif

struct aec_handle;
struct aec_frame;

/* AEC (Acoustic Echo Cancellation) */
void *audio_process_aec_create(int sample_rate, const char *profile_path);
int   audio_process_aec_process(struct aec_handle *h, struct aec_frame *f);
int   audio_process_aec_free(struct aec_handle *h);

/* AGC (Automatic Gain Control) */
void *audio_process_agc_create(void);
int   audio_process_agc_set_config(void *agc, int min_level, int max_level,
				   int mode, int sample_rate,
				   WebRtcAgcConfig config);
void  audio_process_agc_get_config(void *agc, WebRtcAgcConfig *out);
int   audio_process_agc_process(void *agc, const int16_t *const *in_near,
				size_t num_bands, size_t samples,
				int16_t *const *out, int32_t in_mic_level,
				int32_t *out_mic_level, int16_t echo,
				uint8_t *saturation_warning);
int   audio_process_agc_free(void *agc);

/* NS (Noise Suppression) */
void *audio_process_ns_create(void);
int   audio_process_ns_set_config(void *ns, int sample_rate, int policy);
void  audio_process_ns_get_config(void);
void  audio_process_ns_process(void *ns, const float *const *in_bands,
			       int num_bands, float *const *out_bands);
int   audio_process_ns_free(void *ns);

/* HPF (High-Pass Filter, Butterworth biquad, 300 Hz cutoff) */
void  audio_process_hpf_create(int16_t *state_x, int16_t *state_y,
			       int16_t init_x, int16_t init_y,
			       int count_x, int count_y);
int   audio_process_hpf_process(int16_t *state, int16_t *data, int num_samples);
void  audio_process_hpf_free(void);

/* LPF (Low-Pass Filter, first-order IIR, Q8 alpha) */
void  audio_process_lpf_create(int16_t *state, int sample_rate, int cutoff_freq);
void  audio_process_lpf_process(int16_t *state, int16_t *data, int num_samples);
void  audio_process_lpf_free(void);

/* HS (Howling Suppression) */
void *audio_process_hs_create(int sample_rate, int frame_size);
/* Two call shapes: vendor libimp passes (handle, buf, buf, num_bytes); the
 * original shape is (handle, data, num_samples) with the fourth argument unused. */
void  audio_process_hs_process(void *handle, int16_t *data, uintptr_t arg2,
			       uintptr_t arg3);
void  audio_process_hs_free(void *handle);

/* DRC+EQ combined (T32) */
void *audio_process_drc_eq_create(int sample_rate, int channels);
int   audio_process_drc_eq_set_config(void *handle, int drc_threshold,
				      int drc_knee, int drc_ratio,
				      int eq_num_bands, int *eq_freqs,
				      float *eq_gains, int *eq_qs);
int   audio_process_drc_eq_enable(void *handle, int drc_enable, int eq_enable);
void  audio_process_drc_eq_process(void *handle, const float *const *in,
				   float *const *out);
void  audio_process_drc_eq_free(void *handle);

/* DRC only (T41 ZRT) */
void *audio_process_drc_create(int sample_rate, int channels);
int   audio_process_drc_set_config(void *handle, int threshold, int knee,
				   int ratio, int makeup);
void  audio_process_drc_process(void *handle, int16_t *data, int num_samples);
void  audio_process_drc_free(void *handle);

#ifdef __cplusplus
}
#endif

#endif
