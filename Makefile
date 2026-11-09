CROSS_COMPILE ?= mipsel-linux-

CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip

CFLAGS  = -std=c11 -D_GNU_SOURCE -Os -Wall -Wextra -Werror=implicit-function-declaration
CFLAGS += -fPIC -DMIPS_FPU_LE -Isrc
CFLAGS += -ffunction-sections -fdata-sections -flto
CFLAGS += -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident

LDFLAGS  = -shared -fPIC
LDFLAGS += -Wl,-z,max-page-size=0x1000 -Wl,--gc-sections -Wl,--no-as-needed -lm -lpthread -flto
LDFLAGS += -Wl,--version-script=$(SRCDIR)/libaudioProcess.map -Wl,-soname,libaudioProcess.so

AR      = $(CROSS_COMPILE)ar

TARGET  = libaudioProcess.so
TARGET_A = libaudioProcess.a
SRCDIR  = src

# Clean-Room C11 Modules
MOD_SRC = $(addprefix $(SRCDIR)/, \
	aec.c agc.c ns.c hpf.c lpf.c \
	howling.c drc.c eq.c drc_eq.c biquad.c config.c fft.c)

ALL_SRC = $(MOD_SRC)

OBJS = $(ALL_SRC:.c=.o)

all: $(TARGET) $(TARGET_A)

$(TARGET): $(OBJS) $(SRCDIR)/libaudioProcess.map
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	$(STRIP) $@

$(TARGET_A): $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET_A)

size: $(TARGET)
	@ls -lh $(TARGET)
	@readelf -d $(TARGET) | grep NEEDED
	@echo "Exported symbols:"
	@nm -D --defined-only $(TARGET) | grep -c ' T '

test:
	gcc -std=c11 -D_GNU_SOURCE -O2 -rdynamic -Isrc src/aec.c src/agc.c src/ns.c src/biquad.c src/config.c src/hpf.c src/lpf.c src/drc.c src/eq.c src/drc_eq.c src/howling.c src/fft.c tests/unit_tests.c -ldl -lm -o tests/unit_tests_bin
	./tests/unit_tests_bin
	rm -f tests/unit_tests_bin

bench:
	gcc -std=c11 -D_GNU_SOURCE -rdynamic -Isrc -O2 tests/bench_and_test.c -ldl -lm -o tests/bench_bin

process_wav:
	gcc -std=c11 -D_GNU_SOURCE -Isrc -O2 tests/process_wav.c -ldl -lm -o tests/process_wav_bin

eval-quality: process_wav
	python3 scripts/eval_quality_tcd.py $(BASELINE) $(CANDIDATE)

.PHONY: all clean size test bench process_wav eval-quality
