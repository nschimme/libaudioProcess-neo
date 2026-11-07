CROSS_COMPILE ?= mipsel-linux-

CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip

CFLAGS  = -std=c11 -D_GNU_SOURCE -Os -Wall -Wextra -Werror=implicit-function-declaration
CFLAGS += -fPIC -DMIPS_FPU_LE -DWEBRTC_POSIX -Isrc
CFLAGS += -ffunction-sections -fdata-sections -flto
CFLAGS += -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident

LDFLAGS  = -shared -lm -lpthread -fPIC
LDFLAGS += -Wl,-z,max-page-size=0x1000 -Wl,--gc-sections -Wl,--as-needed -flto
LDFLAGS += -Wl,--version-script=$(SRCDIR)/libaudioProcess.map

AR      = $(CROSS_COMPILE)ar

TARGET  = libaudioProcess.so
TARGET_A = libaudioProcess.a
SRCDIR  = src

# Our modules
MOD_SRC = $(addprefix $(SRCDIR)/, \
	aec.c agc.c ns.c hpf.c lpf.c \
	howling.c drc.c eq.c drc_eq.c biquad.c config.c)

# WebRTC AEC
AEC_SRC = $(addprefix $(SRCDIR)/webrtc/modules/audio_processing/aec/, \
	aec_core.c aec_core_mips.c echo_cancellation.c \
	aec_rdft.c aec_rdft_mips.c aec_resampler.c)

# WebRTC AGC
AGC_SRC = $(addprefix $(SRCDIR)/webrtc/modules/audio_processing/agc/legacy/, \
	analog_agc.c digital_agc.c)

# WebRTC NS
NS_SRC = $(addprefix $(SRCDIR)/webrtc/modules/audio_processing/ns/, \
	ns_core.c noise_suppression.c)

# WebRTC utility
UTIL_SRC = $(addprefix $(SRCDIR)/webrtc/modules/audio_processing/utility/, \
	delay_estimator.c delay_estimator_wrapper.c)

# WebRTC common audio
COMMON_SRC = $(addprefix $(SRCDIR)/webrtc/common_audio/, \
	ring_buffer.c fft4g.c)

# WebRTC SPL
SPL_SRC = $(addprefix $(SRCDIR)/webrtc/common_audio/signal_processing/, \
	auto_correlation.c auto_corr_to_refl_coef.c \
	complex_bit_reverse.c complex_fft.c \
	copy_set_operations.c cross_correlation.c cross_correlation_mips.c \
	division_operations.c dot_product_with_scale.c \
	downsample_fast.c \
	energy.c filter_ar.c filter_ar_fast_q12.c \
	filter_ma_fast_q12.c get_hanning_window.c get_scaling_square.c \
	levinson_durbin.c \
	lpc_to_refl_coef.c min_max_operations.c min_max_operations_mips.c \
	randomization_functions.c real_fft.c refl_coef_to_lpc.c \
	resample.c resample_48khz.c resample_by_2.c resample_by_2_internal.c \
	resample_fractional.c \
	spl_init.c spl_sqrt.c spl_sqrt_floor_mips.c \
	splitting_filter.c sqrt_of_one_minus_x_squared.c \
	vector_scaling_operations.c)

# WebRTC VAD
VAD_SRC = $(addprefix $(SRCDIR)/webrtc/common_audio/vad/, \
	webrtc_vad.c vad_core.c vad_filterbank.c vad_gmm.c vad_sp.c)

ALL_SRC = $(MOD_SRC) $(AEC_SRC) $(AGC_SRC) $(NS_SRC) $(UTIL_SRC) \
	  $(COMMON_SRC) $(SPL_SRC) $(VAD_SRC)

OBJS = $(ALL_SRC:.c=.o)

all: $(TARGET) $(TARGET_A)

$(TARGET): $(OBJS) $(SRCDIR)/libaudioProcess.map
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	$(STRIP) $@

$(TARGET_A): $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/webrtc/%.o: $(SRCDIR)/webrtc/%.c
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET_A)

size: $(TARGET)
	@ls -lh $(TARGET)
	@readelf -d $(TARGET) | grep NEEDED
	@echo "Exported symbols:"
	@nm -D --defined-only $(TARGET) | grep -c ' T '

test:
	gcc -std=c11 -D_GNU_SOURCE -O2 -Isrc src/biquad.c src/hpf.c src/lpf.c src/drc.c src/eq.c src/drc_eq.c src/howling.c src/webrtc/common_audio/fft4g.c src/webrtc/common_audio/signal_processing/copy_set_operations.c tests/unit_tests.c -lm -o tests/unit_tests_bin
	./tests/unit_tests_bin
	rm -f tests/unit_tests_bin

bench:
	gcc -std=c11 -D_GNU_SOURCE -O2 tests/bench.c -ldl -lm -o tests/bench_bin

.PHONY: all clean size test bench
