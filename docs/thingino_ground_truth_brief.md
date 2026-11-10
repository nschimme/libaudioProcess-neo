# Brief: finding ground truth for how Thingino calls libaudioProcess

Purpose: settle the open ABI questions from primary sources instead of second-hand reports. Written so the work can be
done in a fresh session without the history of the noise-suppression work.

## Sources (all public)
- `gtxaspec/raptor` and `themactep/prudynt-t`: the camera daemons. They call the **public IMP API** (`IMP_AI_EnableNs`, ...),
  not `audio_process_*`, so they show configuration, rates and policy values but not the library call convention.
- `themactep/thingino-firmware`: packaging. `package/libaudioprocess-neo` pins neo at `475278e` (the public upstream head,
  the old WebRTC-wrapping version); `package/ingenic-lib` fetches the vendor `libimp.so`; `package/openimp` packages
  `opensensor/openimp`.
- `opensensor/openimp`: an open reimplementation of libimp that `dlopen`s `libaudioProcess.so`. `src/t31/openimp_t31_audio.c`
  has the NS/AGC/HPF call sites (verified: NS = `ns_process(ns, in, 1, out)`, 4 arguments, int16-range floats, effects only at
  rates up to 16 kHz). It does **not** call the library's AEC (bundled AECM).
- The vendor `libimp.so` (closed binary, from `ingenic-lib`) is the only source for the vendor's AEC and multi-band behaviour.

## Still open (nothing in the public source answers these)
1. `audio_process_aec_process`: is the frame passed by pointer (this library) or by value as a ~160-byte struct? Field offsets
   consumed: far buffer, near buffer, byte count. Discriminator: how `a1` is loaded at the call (`addiu a1, sp, N` = pointer).
2. `audio_process_aec_create`: is a sample rate passed? (This library declares `(int sample_rate, const char *profile_path)`.)
3. `audio_process_ns_process` at 32/48 kHz in the vendor libimp: `num_bands`, floats per band, who splits the signal, band order.
4. Four arguments or five (with a length) at the vendor `ns_process` call site.
5. NS mode 4 ("non-pumping music/video profile" per an openimp comment): which neo build defines it, if any.
6. Per-tick order of NS, AEC and AGC, and whether NS ever runs ahead of AEC on the near-end signal.

## How
- `git clone --depth 1` the repositories above; `grep -rn "audio_process_"` to find call sites.
- For the vendor binary: obtain `libimp.so` for the SoC (T23 first) from the `ingenic-lib` package source, disassemble the
  MIPS (needs a MIPS-capable objdump), and look at the `jalr` sites that follow the `dlsym` of each name.
- Compare against `src/audio_process.h` and `src/audio_process_internal.h` in this repository; do not change the AEC code or the
  headers until the by-pointer / by-value question has a primary-source answer.
