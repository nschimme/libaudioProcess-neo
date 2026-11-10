# Brief: ground truth for how Thingino calls libaudioProcess

Purpose: settle ABI questions from primary sources instead of second-hand reports. This records what has been settled and what
is left, so the work can continue in a fresh session without the history of the noise-suppression work.

## Sources (all public)
- `gtxaspec/raptor`, `themactep/prudynt-t`: the camera daemons. They call the **public IMP API** (`IMP_AI_EnableNs`, ...),
  not `audio_process_*`, so they show configuration, rates and policy values but not the library call convention.
- `themactep/thingino-firmware`: packaging. `package/libaudioprocess-neo` pins neo at `475278e` (the public upstream head, the old
  WebRTC-wrapping version) and `prudynt-t` selects it; `package/ingenic-lib` fetches the vendor binaries; `package/openimp`
  packages `opensensor/openimp`.
- `opensensor/openimp`: an open reimplementation of libimp that `dlopen`s `libaudioProcess.so` (NS/AGC/HPF only; it bundles its own
  AEC). `src/t31/openimp_t31_audio.c` has the call sites.
- `gtxaspec/ingenic-lib` (commit `ea62f3d`): the vendor `libimp.so` and `libaudioProcess.so` per SoC family and SDK version. The
  `libimp.so` files are not stripped and `llvm-objdump --triple=mipsel-linux-gnu --mcpu=mips32r2` disassembles them.
  Scope used: the **caller side only** (call sites, argument registers, struct layouts, return handling). The vendor
  `libaudioProcess.so` algorithms are not to be read, to keep this library's algorithms clean-room. Do not add vendor binaries
  to this repository.

## Settled (vendor `libimp.so`, T23 SDK 1.3.0 and T31 SDK 1.1.1)
- `ns_process(ns, in_bands, 1, out_bands)`: four arguments, `num_bands` a literal 1, one pointer per array, int16 to float with
  `cvt.s.w`, back with `trunc.w.s` and a 16-bit store, **no clamp**.
- `IMP_AI_EnableNs`: mode must be below 4 (`sltiu`) and the rate one of 8000/16000/32000/48000; it stores 160 samples per call
  (80 at 8 kHz), allocates two float buffers of that size and calls `ns_set_config(ns, rate, mode)`. No band split at 32/48 kHz:
  consecutive 160-sample slices of the full-rate signal, real rate passed to `set_config`.
- `aec_create(sample_rate, profile_path)`; `aec_process(handle, &frame)` with a 16-byte stack frame
  `{far ptr (null = skip), mic ptr, out ptr (= mic), count}`: by pointer, in place. This matches `struct aec_frame`.
- Per-tick order: device read, AEC, NS, AGC, high-pass filter, howling suppression (NS reads the buffer the AEC wrote in place).
- Each `IMP_AI_Enable*` resolves its own group of names with `dlsym`; the AEC group has create, process, free only.
- The earlier reports of a by-value AEC frame, a fifth `ns_process` argument and an `aec_create` without a rate are contradicted.
- OpenIMP agrees on NS (`ns_process(ns, in, 1, out)`) and AGC; it applies NS/AGC only up to 16 kHz.
- "Mode 4" ("music" policy) exists only on the `feature/music-video-noise-suppression` branch of `matteius/libaudioProcess-neo`
  (one commit on the old WebRTC code) and in OpenIMP, which accepts 0-4; the vendor `libimp` rejects 4 before calling the library.

## Still open
1. The same invariants on the other SoC families and SDK versions (T10/T20/T21/T30/T32/T40/T41/A1/C100): a scripted check over
   every `libimp.so` in `ingenic-lib` (literal 1 in the `ns_process` call, stack address as the `aec_process` frame, the rate
   constants and `sltiu 4` in `EnableNs`, no clamp after `trunc.w.s`).
2. What `frame[3]` of the AEC frame counts exactly (bytes for mono; divided by the channel count) and whether stereo changes the
   call; the AEC has still not been run on a camera.
3. Whether any caller ever passes more than one band (none found in the vendor binary or OpenIMP).
4. What mode 4 should do in this library, if anything (a decision, not a fact).
