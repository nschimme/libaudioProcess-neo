/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#include "audio_process.h"
#include "audio_process_internal.h"
#include "util.h"

#pragma pack(push, 1)
struct wav_header {
	char     riff_tag[4];   /* "RIFF" */
	uint32_t riff_size;
	char     wave_tag[4];   /* "WAVE" */
	char     fmt_tag[4];    /* "fmt " */
	uint32_t fmt_size;      /* 16 for PCM */
	uint16_t audio_format;  /* 1 for PCM */
	uint16_t num_channels;  /* 1 for Mono, 2 for Stereo */
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
	char     data_tag[4];   /* "data" */
	uint32_t data_size;
};
#pragma pack(pop)

typedef void  (*fn_hpf_create)(int16_t *, int16_t *, int16_t, int16_t, int, int);
typedef int   (*fn_hpf_process)(int16_t *, int16_t *, int);
typedef void  (*fn_lpf_create)(int16_t *, int, int);
typedef void  (*fn_lpf_process)(int16_t *, int16_t *, int);
typedef void *(*fn_drc_create)(int, int);
typedef int   (*fn_drc_set_config)(void *, int, int, int, int);
typedef void  (*fn_drc_process)(void *, int16_t *, int);
typedef void  (*fn_drc_free)(void *);
typedef void *(*fn_agc_create)(void);
typedef int   (*fn_agc_set_config)(void *, int, int, int, int, WebRtcAgcConfig);
typedef int   (*fn_agc_process)(void *, const int16_t *const *, size_t, size_t, int16_t *const *, int32_t, int32_t *, int16_t, uint8_t *);
typedef int   (*fn_agc_free)(void *);
typedef void *(*fn_ns_create)(void);
typedef int   (*fn_ns_set_config)(void *, int, int);
typedef void  (*fn_ns_process)(void *, const float *const *, int, float *const *);
typedef int   (*fn_ns_free)(void *);
typedef void *(*fn_aec_create)(int, const char *);
typedef int   (*fn_aec_process)(struct aec_handle *, struct aec_frame *);
typedef int   (*fn_aec_free)(struct aec_handle *);

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "Usage: %s <libaudioProcess.so> <input.wav> <output.wav> [module: all|hpf|lpf|drc|agc|ns|aec] [far_ref.wav]\n", argv[0]);
		return 1;
	}

	const char *lib_path = argv[1];
	const char *in_path = argv[2];
	const char *out_path = argv[3];
	const char *mod = (argc > 4) ? argv[4] : "all";
	const char *far_path = (argc > 5) ? argv[5] : NULL;

	/* Pre-load libm with RTLD_GLOBAL to resolve math symbols on libraries
	   built with --as-needed */
	dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL);

	void *handle = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		fprintf(stderr, "Failed to load %s: %s\n", lib_path, dlerror());
		return 1;
	}

	/* Optional far-end reference audio for dual-channel AEC evaluation */
	int16_t *far_samples = NULL;
	int far_num_samples = 0;
	if (far_path && strlen(far_path) > 0) {
		FILE *ffar = fopen(far_path, "rb");
		if (ffar) {
			struct wav_header far_hdr;
			if (fread(&far_hdr, 1, sizeof(far_hdr), ffar) == sizeof(far_hdr) &&
			    far_hdr.bits_per_sample == 16 && far_hdr.num_channels == 1) {
				far_num_samples = far_hdr.data_size / sizeof(int16_t);
				far_samples = malloc(far_num_samples * sizeof(int16_t));
				if (fread(far_samples, sizeof(int16_t), far_num_samples, ffar) != (size_t)far_num_samples) {
					free(far_samples);
					far_samples = NULL;
					far_num_samples = 0;
				}
			}
			fclose(ffar);
		}
	}

	FILE *fin = fopen(in_path, "rb");
	if (!fin) {
		fprintf(stderr, "Failed to open input WAV: %s\n", in_path);
		dlclose(handle);
		return 1;
	}

	struct wav_header hdr;
	if (fread(&hdr, 1, sizeof(hdr), fin) != sizeof(hdr)) {
		fprintf(stderr, "Invalid WAV header in %s\n", in_path);
		fclose(fin);
		dlclose(handle);
		return 1;
	}

	if (hdr.bits_per_sample != 16 || hdr.num_channels != 1) {
		fprintf(stderr, "Only 16-bit Mono PCM supported (got %d-bit, %d-ch)\n",
			hdr.bits_per_sample, hdr.num_channels);
		fclose(fin);
		dlclose(handle);
		return 1;
	}

	int sample_rate = hdr.sample_rate;
	int num_samples = hdr.data_size / sizeof(int16_t);
	int16_t *samples = malloc(num_samples * sizeof(int16_t));
	if (fread(samples, sizeof(int16_t), num_samples, fin) != (size_t)num_samples) {
		fprintf(stderr, "Incomplete read of %s\n", in_path);
		free(samples);
		fclose(fin);
		dlclose(handle);
		return 1;
	}
	fclose(fin);

	/* Initialize modules based on choice */
	int run_all = (strcmp(mod, "all") == 0);

	/* 1. HPF */
	if (run_all || strcmp(mod, "hpf") == 0) {
		fn_hpf_create hpf_create = dlsym(handle, "audio_process_hpf_create");
		fn_hpf_process hpf_proc = dlsym(handle, "audio_process_hpf_process");
		if (hpf_create && hpf_proc) {
			int16_t state[16] = {0};
			hpf_create(state, state + 8, 0, 0, 8, 8);
			hpf_proc(state, samples, num_samples);
		}
	}

	/* 2. NS */
	if (run_all || strcmp(mod, "ns") == 0) {
		fn_ns_create ns_create = dlsym(handle, "audio_process_ns_create");
		fn_ns_set_config ns_set_cfg = dlsym(handle, "audio_process_ns_set_config");
		fn_ns_process ns_proc = dlsym(handle, "audio_process_ns_process");
		fn_ns_free ns_free = dlsym(handle, "audio_process_ns_free");
		if (ns_create && ns_set_cfg && ns_proc && ns_free) {
			void *ns = ns_create();
			if (ns_set_cfg(ns, sample_rate, 2) == 0) {
				int frame_size = sample_rate / 100; /* 10ms */
				int num_frames = num_samples / frame_size;
				float in_f[480] = {0}, out_f[480] = {0};
				const float *inp = in_f;
				float *outp = out_f;

				for (int f = 0; f < num_frames; f++) {
					int16_t *chunk = samples + f * frame_size;
					for (int i = 0; i < frame_size; i++) in_f[i] = (float)chunk[i];
					ns_proc(ns, &inp, 1, &outp);
					for (int i = 0; i < frame_size; i++) chunk[i] = clamp_i16(out_f[i]);
				}
			}
			ns_free(ns);
		}
	}

	/* 3. AGC */
	if (run_all || strcmp(mod, "agc") == 0) {
		fn_agc_create agc_create = dlsym(handle, "audio_process_agc_create");
		fn_agc_set_config agc_set_cfg = dlsym(handle, "audio_process_agc_set_config");
		fn_agc_process agc_proc = dlsym(handle, "audio_process_agc_process");
		fn_agc_free agc_free = dlsym(handle, "audio_process_agc_free");
		if (agc_create && agc_set_cfg && agc_proc && agc_free) {
			void *agc = agc_create();
			WebRtcAgcConfig cfg = { .targetLevelDbfs = 3, .compressionGaindB = 9, .limiterEnable = 1 };
			agc_set_cfg(agc, 0, 255, 3, sample_rate, cfg);
			int frame_size = sample_rate / 100; /* 10ms */
			int num_frames = num_samples / frame_size;
			for (int f = 0; f < num_frames; f++) {
				int16_t *chunk = samples + f * frame_size;
				const int16_t *inp = chunk;
				int16_t *outp = chunk;
				int32_t mic = 100;
				uint8_t sat = 0;
				agc_proc(agc, &inp, 1, frame_size, &outp, mic, &mic, 0, &sat);
			}
			agc_free(agc);
		}
	}

	/* 4. DRC */
	if (run_all || strcmp(mod, "drc") == 0) {
		fn_drc_create drc_create = dlsym(handle, "audio_process_drc_create");
		fn_drc_set_config drc_set_cfg = dlsym(handle, "audio_process_drc_set_config");
		fn_drc_process drc_proc = dlsym(handle, "audio_process_drc_process");
		fn_drc_free drc_free = dlsym(handle, "audio_process_drc_free");
		if (drc_create && drc_set_cfg && drc_proc && drc_free) {
			void *drc = drc_create(sample_rate, 1);
			drc_set_cfg(drc, 20, 10, 4, 0);
			drc_proc(drc, samples, num_samples);
			drc_free(drc);
		}
	}

	/* 5. AEC: executed when mod is "aec", or when running pipeline with a valid far-end reference */
	if (strcmp(mod, "aec") == 0 || (run_all && far_samples != NULL)) {
		fn_aec_create aec_create = dlsym(handle, "audio_process_aec_create");
		fn_aec_process aec_proc = dlsym(handle, "audio_process_aec_process");
		fn_aec_free aec_free = dlsym(handle, "audio_process_aec_free");
		if (aec_create && aec_proc && aec_free) {
			struct aec_handle *aec = aec_create(sample_rate, NULL);
			if (aec) {
				int frame_size = sample_rate / 100; /* 10ms */
				int num_frames = num_samples / frame_size;
				int16_t *far_frame = malloc(frame_size * sizeof(int16_t));

				for (int f = 0; f < num_frames; f++) {
					int16_t *near_chunk = samples + f * frame_size;
					if (far_samples && (f + 1) * frame_size <= far_num_samples) {
						memcpy(far_frame, far_samples + f * frame_size, frame_size * sizeof(int16_t));
					} else {
						memset(far_frame, 0, frame_size * sizeof(int16_t));
					}

					struct aec_frame frm = {
						.far = far_frame,
						.near = near_chunk,
						.num_bytes = frame_size * sizeof(int16_t)
					};
					aec_proc(aec, &frm);
				}

				free(far_frame);
				aec_free(aec);
			}
		}
	}

	if (far_samples) {
		free(far_samples);
	}

	/* Write output WAV */
	FILE *fout = fopen(out_path, "wb");
	if (!fout) {
		fprintf(stderr, "Failed to create output WAV: %s\n", out_path);
		free(samples);
		dlclose(handle);
		return 1;
	}
	fwrite(&hdr, 1, sizeof(hdr), fout);
	fwrite(samples, sizeof(int16_t), num_samples, fout);
	fclose(fout);

	free(samples);
	dlclose(handle);
	return 0;
}
