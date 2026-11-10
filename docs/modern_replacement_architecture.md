# Modern Replacement Architecture for WebRTC Subsystems in libaudioProcess-neo

## 1. Executive Summary & Motivation

`libaudioProcess-neo` currently serves as a 74 KB drop-in C11 replacement for Ingenic's proprietary 510–733 KB `libaudioProcess.so`. While the custom modules (`biquad.c`, `hpf.c`, `lpf.c`, `drc.c`, `eq.c`, `howling.c`) are lightweight and highly optimized, the vendored WebRTC M47 subsystem (`src/webrtc/`):
- Accounts for **> 80% of total binary footprint** (~60 KB of 78 KB).
- Relies on 2008-era fixed-point subband splitters (`splitting_filter.c`) and legacy code originally designed for desktop PC voice chat.
- Carries legacy code complexity (over 20,000 lines across WebRTC AEC, AGC, and NS).

By designing modern, clean-room C11 replacement algorithms using a **black-box oracle strategy**, we can:
1. Maintain **ABI compatibility** with Ingenic's `libimp.so`: the 19 symbols and frame formats that `libimp` resolves with `dlsym` (see §2.1). The library currently exports 31 symbols; the extra exports are ignored by `libimp` and are not part of the contract.
2. Keep the AEC configuration compatible with `webrtc_profile.ini`: `libimp` hands the INI directory to `audio_process_aec_create`, and neo parses only `[Set_Far_Frame]`, `[Set_Near_Frame]` and `[AEC]`. The INI schema is owned by neo; the legacy `[AGC]`, `[HP]`, `[LE]`, `[NS]` and `[VAD]` blocks are ignored (AGC/NS/HPF settings reach the library through `libimp` calls driven by `raptor.conf`).
3. Drop binary size by another ~50% (from 78 KB down to **~35 KB**).
4. Substantially reduce CPU utilization on Thingino MIPS32r2 and RISC-V camera SoCs while achieving superior audio clarity and faster convergence.

---

## 2. ABI & Configuration Mapping

The replacement algorithms strictly adhere to the existing public function interfaces and configuration parser:

| Module | Exported Functions | Configuration Source |
|---|---|---|
| **AGC** | `audio_process_agc_create`<br>`audio_process_agc_set_config`<br>`audio_process_agc_get_config`<br>`audio_process_agc_process`<br>`audio_process_agc_free` | `WebRtcAgcConfig` (`targetLevelDbfs`, `compressionGaindB`, `limiterEnable`), `mode`, `min/max_level`, `sample_rate` |
| **NS** | `audio_process_ns_create`<br>`audio_process_ns_set_config`<br>`audio_process_ns_get_config`<br>`audio_process_ns_process`<br>`audio_process_ns_free` | `policy` (0=mild, 1=medium, 2=aggressive, 3=very aggressive), `sample_rate` |
| **AEC** | `audio_process_aec_create`<br>`audio_process_aec_process`<br>`audio_process_aec_free` | `webrtc_profile.ini` (`[Set_Far_Frame] Frame_V`, `[Set_Near_Frame] Frame_V`, `delay_ms`, `[AEC] set_suppression_mode`, `user_mode`, `mu_min`, `mu_decay`, `cor_thd1..4`, etc.) |

### 2.1 ABI Contract with `libimp`

Provenance is tagged on each item: **[vendor binary]** means read from the caller side of the vendor `libimp.so` (T23 SDK 1.3.0 and T31 SDK 1.1.1, from `gtxaspec/ingenic-lib` at commit `ea62f3d`; disassembled with `llvm-objdump`), that is, call sites, argument registers, struct layouts and return-code handling only; no vendor algorithm code was read, the vendor `libaudioProcess.so` was not disassembled, and no vendor binary is part of this repository (a later scripted sweep covered 38 `libimp.so` builds, one per SoC and SDK version, with the same limits; its result is in §4.5); **[reported]** means reported by the Thingino project from its reading of the `libimp` disassembly and `raptor` sources (not independently re-derived here); **[verified]** means checked against this repository's code or tests.

`libimp` embeds its own `audio_process.c`, loads `libaudioProcess.so` with `dlopen()` and resolves everything with `dlsym()` at runtime. A missing symbol is fatal for the whole audio pipeline. **[reported]**

- **Required symbols (19, exact spelling):** `audio_process_{aec,agc,ns,hpf,hs}_{create,process,free}` (15), plus `audio_process_agc_set_config`, `audio_process_agc_get_config`, `audio_process_ns_set_config` and `audio_process_ns_get_config`. Extra exports are ignored. **[reported]** All 19 are exported by the library, which exports 31 symbols in total. **[verified]** The `local: *` version script keeps internals hidden so new globals cannot collide with `libimp`'s. `scripts/check_abi.sh` (run by `make abi-check` and by CI on the MIPS candidate library) fails if any of the 19 names is missing. **[verified]**
- **Lifecycle:** `create` once per `IMP_AI_EnableX`, `set_config` right after, `process` every 10 ms frame from the capture thread, `free` on disable. **[reported]** `ns_free` releases the only allocation and there are no mutable statics in `ns.c` or `hpf.c`. **[verified]**
- **Frozen layouts:** `struct aec_frame { const int16_t *far; int16_t *near; int reserved; int num_bytes; }` (built on `libimp`'s stack) and `WebRtcAgcConfig { int16_t targetLevelDbfs; int16_t compressionGaindB; uint8_t limiterEnable; }` (passed by value to `agc_set_config`). **[reported]** Both match the definitions in `src/audio_process_internal.h` and `src/audio_process.h`. **[verified]** In the vendor binary the AEC frame is a 16-byte structure built on `libimp`'s stack and passed **by pointer** (see the AEC bullet below). **[vendor binary]**
- **NS call contract:**
  - `process(ns, const float *const *in, num_bands, float *const *out)` takes float samples **in int16 range** (the upstream WebRTC float API contract, which the vendor library wraps). **[reported]** The NS result is independent of scale (75-80 dB agreement between int16-range and +-1.0 input on a speech-in-noise clip, about 57-60 dB on a digitally silent clip), and in-place processing is bit-identical to separate buffers, including at 32/48 kHz. **[verified]**
  - The NS has **no length parameter**: each call consumes exactly one block of band 0, **80 samples at 8 kHz and 160 samples at 16 kHz and above**, the same `blockLen` as WebRTC's NS (`InitCore`). Cutting the capture stream into blocks is `libimp`'s job (with `raptor`'s 20 ms frames, `samples_per_frame = rate / 50`, that is two calls per frame at 8 or 16 kHz). `libimp` also owns two 640-byte float band buffers (160 floats). **[reported]** A shim that accepted larger blocks would read one block and drop the rest, producing corrupt audio that looks fine at initialisation. The NS uses exactly these block lengths. **[verified]** In the vendor binary, `IMP_AI_EnableNs` stores the samples per call (160 for 16, 32 and 48 kHz, 80 for 8 kHz), allocates two float buffers of that many samples (640 bytes for 160), and the record thread slices each captured frame into consecutive blocks of that size. **[vendor binary]**
  - **Accepted rates:** `WebRtcNs_Init` and `WebRtcAec_Init` reject every rate outside {8000, 16000, 32000, 48000}, so `ns_set_config` returns -1 for anything else. **[reported]** In practice `raptor` only produces 8000 and 16000 (`rad` clamps, `rad_main.c`). **[reported]** The library accepts exactly the four rates and returns exactly 0 or -1, and rejects a policy outside 0-3 with -1. **[verified]**
  - **Sample rates:** the capture rate is configurable (`[audio] sample_rate` in `raptor.conf`, passed straight to the HAL with no clamp; the IMP enumeration covers 8-96 kHz; G.711 forces 8 kHz). The default is 16 kHz. **[reported]** The AEC supports only 8 and 16 kHz, so a camera that needs echo cancellation stays at 16 kHz. The microphone's frequency response, not the rate, is the practical ceiling (low-cost capsules often roll off by 12-16 kHz). **[reported]** The NS accepts only 8, 16, 32 and 48 kHz; 44.1 kHz is accepted by IMP but rejected by the vendor NS and by this library (`set_config` returns -1), so enabling NS there fails for the vendor too. **[reported / verified]**
  - **Bands (resolved by the vendor binary):** `ns_process` is called as `ns_process(ns, in_bands, 1, out_bands)`: **four arguments, with `num_bands` a literal 1**, and each of `in_bands` and `out_bands` is an array on the stack holding one pointer to a float buffer. Samples are converted from `int16` to float with `cvt.s.w` and back with `trunc.w.s` and a 16-bit store. **[vendor binary, T23 1.3.0 and T31 1.1.1]** OpenIMP makes the identical call. **[verified in the openimp source]** There is no band splitting: at 32 and 48 kHz the vendor passes consecutive 160-sample slices of the full-rate signal as a single band, and gives the real rate (32000 or 48000) to `set_config`. **[vendor binary]** This library therefore analyses each slice as 16 kHz-rate audio, exactly as the WebRTC NS it replaces does; its time constants are counted in calls, so at 48 kHz they run three times faster in real time and its frequency axis is stretched threefold (a limit shared with the original, not yet measured here). The multi-band path (`num_bands > 1`, with upper bands following the low band's gain) has no known caller and is kept as a labelled compatibility extension; the NS reads and writes exactly 160 samples per band (80 at 8 kHz) whatever the layout, so no layout can overrun a buffer (AddressSanitizer tests with exact-length buffers). **[verified]**
  - **Evaluation limit for the upper bands:** the TCD-VoIP material is nearly band-limited: its 8-16 kHz band is 13-48 dB below the 0-8 kHz band and its 16-24 kHz band holds about 0.002% of the energy, so it cannot say anything about upper-band quality at 32/48 kHz. Real wideband captures from a camera are needed, and the multi-band path has never run on real audio. **[verified]**
  - **Call site, resolved:** the argument list is `(ns, in_bands, num_bands, out_bands)`; the report of a fifth argument carrying a length is contradicted by the binary, where only registers `$4` to `$7` are loaded before the call. **[vendor binary]** The declaration in `src/audio_process.h` is correct.
  - `set_config(ns, sample_rate, policy)` receives the raw `audio.ns_level` value 0-3, which maps one to one onto WebRTC's `kLowSuppression` .. `kVeryHighSuppression`. **[reported]**
  - The vendor wrapper is `WebRtcNs_Init(ns, rate)` followed by `WebRtcNs_set_policy(ns, policy)`, with the result normalised to exactly 0 or -1 (any non-zero core result becomes -1); `libimp` may test only zero versus non-zero. **[reported]** `ns_set_config` has the same observable behaviour: an unsupported rate fails before anything changes, a valid rate re-initialises the state, and an out-of-range policy then fails without being applied. **[verified]**
  - `ns_get_config` is a `void(void)` no-op that `libimp` calls and ignores; keep it callable. **[reported]** It is exported with that signature. **[verified]**
- **Default NS policy:** NS is **off by default** (`ns_enabled` is false). When enabled, `ns_level` defaults to 1 (moderate): the shipped `raptor.conf` line is the commented `# ns_level = 1  # 0=low, 1=moderate, 2=high, 3=veryhigh`, and `rad`'s code fallback is `RSS_NS_MODERATE`. The value passes straight through as the policy, and `IMP_AI_EnableNs` hard-validates the mode as 0-3 (anything else fails the enable). **[reported]** In the vendor binary the check (`sltiu $2, $5, 4` on the mode argument) happens in `libimp` itself, before the library is called, and the rate is compared against 8000, 16000, 32000 and 48000 with any other rate an error. **[vendor binary]** A mode of 4 therefore never reaches this library through the vendor `libimp`. Policy 1 is therefore what users get by default; the CI quality gate currently scores policy 2. **[verified]**
- **Call chain and runtime:** `raptor.conf [audio]` -> `rad_main.c` -> `hal_audio_enable_ns()` -> `IMP_AI_EnableNs` -> `dlopen` / `dlsym` of the NS entry points. NS and AGC are applied inside `libimp`'s own record thread, so frames returned by `IMP_AI_GetFrame` are already processed. The software AEC path (`audio_process_aec_process`, with `webrtc_profile.ini`) already includes NS, so the two should not be stacked. **[reported]**
- **Latency:** the reported contract is that every effect is frame-synchronous with zero added delay (the AEC profile ships `delay_ms = 0`); extra NS delay would shift the near-end signal relative to the AEC and degrade double-talk handling. **[reported]** Per-tick order, from the control flow of the vendor record thread: read the device, then **AEC**, then **NS**, then **AGC**, then the high-pass filter, then howling suppression. The NS input loop reads through the same buffer register that the AEC received as its in-place input and output, so the NS runs downstream of the AEC and its delay cannot misalign the AEC. **[vendor binary, T23; on T31 the AEC block is placed after the others in the function but branches back to the top of the loop]** Neo's AEC does not call the NS internally. **[verified]** Measured algorithmic delay at 16 kHz: the vendor-twin WebRTC NS 96 samples (6.0 ms) and the neo NS 96 samples (6.0 ms); an earlier neo filterbank used 160 samples (10.0 ms) and was replaced to meet this contract. **[verified]**
- **Reconfiguration:** there is no mid-stream churn. `set_config` is called once at enable, the frame length comes from the IO attributes set at channel setup, and `raptor`/`rad` restarts the whole audio device on a configuration change. **[reported]**
- **Two `dlsym` tables:** `libimp` loads the library in two code paths, a full AGC+NS set (with get/set_config) and an AGC-only five-entry table, and both must resolve every symbol they ask for. **[reported]** In the vendor binary each `IMP_AI_Enable*` function resolves its own group of names with `dlsym` (AGC and NS five names each, HPF and HS three, AEC three: create, process, free; no AEC `set_config`) into a table of function pointers that the matching `Disable*` clears. **[vendor binary]** `scripts/check_abi.sh` checks all 19. **[verified]**
- **AEC call convention (resolved by the vendor binary):** `audio_process_aec_create(int sample_rate, const char *profile_path)` takes two arguments (the first is `attr.samplerate` of the audio device, the second a pointer to the profile-path string). `audio_process_aec_process(handle, &frame)` is called with a pointer to a 16-byte structure built on the stack: `frame[0]` the far-end reference pointer (null means the AEC is skipped), `frame[1]` the microphone data pointer, `frame[2]` an output pointer equal to `frame[1]` (in place), `frame[3]` a count. **[vendor binary, T23 1.3.0 and T31 1.1.1]** This matches `struct aec_frame { const int16_t *far; int16_t *near; int reserved; int num_bytes; }` and the declarations in `src/audio_process.h`, whose layout is unchanged since the repository's first commits; the library reads offsets 0, 4 and 12 and writes in place into `near`. **[verified]** The two earlier reports (a roughly 160-byte structure passed by value, and `aec_create` taking no rate) are contradicted by the binary; the report that neo initialises the WebRTC AEC inside `aec_create` describes the version Thingino pins (`475278e`), not the current clean-room AEC. The meaning of `frame[3]` is a byte count for the mono case (it is the device frame size divided by the channel count); the AEC itself has not been run on a camera.
- **Output range (requirement):** the vendor record thread converts the NS's float output back with `trunc.w.s` and a 16-bit store and **no clamp**, so a value beyond the int16 range would wrap into a full-scale click. The WebRTC NS saturates internally and `libimp` never needed to. This library saturates its low-band and upper-band outputs to [-32768, 32767] (it did not before: on clipped full-scale input 7.2% of output samples were out of range, by up to 2469 counts), and a unit test asserts it. **[vendor binary / verified]**
- **Independent reference (source-verified):** `opensensor/openimp` is an open reimplementation of libimp that Thingino packages (tag v2026.11.07-rc3 at the time of writing) and that loads `libaudioProcess.so` with `dlopen`. Its T31/T23 audio path calls the library as follows. **[verified in the openimp source]** For each 10 ms frame (`sample_rate / 100` samples) it runs HPF, then NS, then AGC in turn. NS: `ns_set_config(ns, sample_rate, mode)` at enable, then `ns_process(ns, inputs, 1, outputs)` with one band, four arguments, int16-range floats converted from the samples, separate input and output arrays, and the result clamped to the int16 range; a non-zero `set_config` result makes the enable fail. AGC: `agc_set_config(agc, 0, 255, mode, sample_rate, config)` with the config struct by value, and `agc_process(agc, inputs, 1, samples, outputs, 127, &level, 0, &saturated)` in place. These agree with the declarations in `src/audio_process.h`. In that implementation NS and AGC run only when the sample rate is 16 kHz or below. It does not call the library's AEC at all (it bundles its own AECM), so it cannot settle the AEC calling convention. It accepts NS modes 0-4 and describes mode 4 as a "non-pumping music/video profile" of the neo library; no public neo commit contains such a mode and this library rejects policies above 3, so the origin of mode 4 is unknown.
- **What ships today:** the Thingino firmware pins `libaudioProcess-neo` at commit `475278e` (the public upstream head), the version that wraps the vendored WebRTC code and initialises the WebRTC AEC inside `aec_create`. Reports about what neo does at `aec_create` describe that version, not this one. `raptor` and `prudynt-t` do not call the library directly; they use the public IMP API (`IMP_AI_EnableNs(&ioattr, level)`, `IMP_AI_EnableAgc`, `IMP_AI_EnableHpf`). **[verified in the raptor, prudynt-t and thingino-firmware sources]**
- **Call pattern:** `libimp` calls the AEC and NS serially from a single record thread, one 10 ms frame per call, with `samples == rate / 100` per call. `set_config` may follow `create` immediately, before any `process`, and calling it twice must be harmless; the NS satisfies both (tested). **[reported / verified]** AGC's nine-argument `agc_process` signature matches `src/audio_process.h`. **[reported / verified]** Any new exported name must also be listed in `src/libaudioProcess.map` (which hides everything else with `local: *`). **[reported]**
- **AGC:** takes `int16` samples (a deliberate domain difference from NS). `agc_target_dbfs` maps to `targetLevelDbfs` and `agc_compression_db` to `compressionGaindB`. The mic level (`in_mic_level` in, `*out_mic_level` out) and `*saturation_warning` round-trip on every frame. **[reported]**
- **HPF/HS:** `int16` in place with caller-owned state buffers, no hidden globals. **[reported]**
- **Library:** MIPS o32, C linkage, `SONAME=libaudioProcess.so`, built against the target's libc flavour. **[reported]** The Makefile sets the SONAME. **[verified]**
- **Free to change:** everything inside the wrappers, algorithm internals, memory sizes, the INI schema (parsed by neo, not by `libimp`, which only relays the path), the dead `[AGC]`/`[NS]`/`[HP]` sections and extra exports. A hot-reload or re-read hook is acceptable as long as `create` and `set_config` still perform their initialisation exactly when called. **[reported]**

---

## 3. Replacement Algorithm Specifications

### 3.1 Modern Automatic Gain Control (AGC)

#### Legacy WebRTC Limitations
- WebRTC legacy AGC divides audio into multiple sub-bands using quadrature mirror filters, tracks digital and analog stage levels independently with heavy lookup tables, and introduces latency and CPU overhead.

#### Proposed Modern Design: Hybrid Peak/RMS Level Detector + Lookahead Soft Limiter
- **Dual Envelope Detection**:
  - Fast-attack (5 ms) / slow-release (300 ms) RMS tracking for perceived loudness.
  - Sub-millisecond peak detector to catch voice transients and plosives.
- **Continuous Gain Mapping**:
  $$\text{Gain}_{\text{target}} = \min\left(10^{\frac{\text{compressionGaindB}}{20}},\; \frac{10^{-\frac{\text{targetLevelDbfs}}{20}}}{\max(\text{RMS}, \epsilon)}\right)$$
  Smooth gain transitions using first-order low-pass filtering to eliminate audio "breathing" artifacts.
- **Smooth Knee Limiter**:
  - When `limiterEnable == 1`, signals exceeding -1.0 dBFS are softly compressed using a polynomial knee curve ($y = x - \frac{1}{3}x^3$ above threshold), preventing hard digital clipping without introducing harmonic distortion.
- **Complexity**: ~150 lines of C11 vs. 2,300 lines of WebRTC legacy code.

---

### 3.2 Modern Noise Suppression (NS)

#### Legacy WebRTC Limitations
- Requires subband splitting (`splitting_filter.c`), multi-band quantile noise tracking, and complex decision-directed a priori SNR estimation, taking > 3,500 lines of code and high memory bandwidth.

#### Implemented Design: MCRA Noise Tracking + Decision-Directed Wiener Gain with OM-LSA Speech Presence
The first design proposed here (minimum-statistics tracking with a fixed over-subtraction mask `G = max(beta, 1 - alpha * Pn/Py)`) was built and measured on the TCD-VoIP clips. It matched the stationary-noise attenuation gates but cut speech by 9-10 dB in every band (ViSQOL about 2.1 on clean speech against about 4.2 for the oracle), so it was replaced. The implemented design (`src/ns.c`, about 420 lines):
- **STFT (low-delay filterbank):** a window of W = L + ov samples with a hop of L, where L is the band length (80 samples at 8 kHz, 160 at 16, 32 and 48 kHz) and ov = 3L/5 (48 or 96 samples). The window is a power-complementary rising ramp of ov samples (a `sin(pi/2 * sin^2)` curve, whose low sidelobes keep loud low-frequency speech from leaking into high bins), a flat top of L - ov samples, and the mirrored falling ramp. With a hop of L the falling ramp of one frame overlaps the rising ramp of the next in exactly ov samples, so the overlap-add reconstructs perfectly. The FFT length equals the window: a 128-point real FFT at 8 kHz and a 256-point real FFT at 16, 32 and 48 kHz. At 32/48 kHz the band is analysed as 16 kHz audio.
- **Noise tracking (MCRA-style):** speech presence per bin from the ratio of smoothed power to a minimum tracked over about 1.2 s. The noise estimate follows the power with these guards, each added to fix a measured failure: (1) the fall target depends on the frame type, the raw power in speech frames and the smoothed power in pauses, because chasing instantaneous troughs in pauses biased the estimate 5-10 dB low and let fluctuating noise (babble) pass as speech; (2) a bin whose smoothed power is more than 10x the current estimate is treated as signal and the estimate rises only slowly there (weight 0.995), because the windowed-minimum ratio cannot flag rapidly modulated sparse sounds such as a harmonic voice or a tone; (3) the upward update is capped and slowed in frames that follow speech; (4) the estimate has an absolute floor so digital silence cannot drive it to zero; (5) the first frame seeds the smoothed power with its own power, so start-up does not depend on the absolute signal level.
- **Gain:** decision-directed a priori SNR feeds a Wiener gain, blended with the policy floor by an OM-LSA speech-presence probability (local frequency smoothing plus a frame-level term; the absence-prior cap is set per policy).
- **Policy floors** equal the documented oracle attenuation, `10^(-dB/20)`: 0.457, 0.229 and 0.115 for policies 0-2 (6.8, 12.8 and 18.8 dB) and 0.070 for policy 3 (23 dB; the §4.2 gate is only a lower bound of 21 dB). The estimate converges to the floor on stationary noise, so measured attenuation matches these figures.
- **Latency:** ov samples, the overlap of the window ramps: 96 samples (6.0 ms) at 16 kHz and 48 samples (6.0 ms) at 8 kHz, measured by cross-correlation. This equals the measured delay of the vendor-twin WebRTC NS at 16 kHz (96 samples). The upper subbands of the compatibility path are delayed by the same ov samples.
- **Synthesis:** inverse real FFT and overlap-add.
- **Known limits:** car noise is level with the oracle on average and is the weakest type (dev -0.03, test +0.01). At policy 2 babble trails on the test half (-0.07, 4 of 7 files) while leading at policy 1 (+0.03). A single office file scores -0.39 at both rates on the test half. Adaptation was measured against the oracle on stationary noise: after a step up in the noise level, full suppression returns within 0.2-0.3 s for +10 dB, 2.8-2.9 s for +20 dB and 3.9-4.0 s for +30 dB (oracle: 1.3-1.4 s, 2.0-3.3 s and 3.9-5.7 s). A steady loud tone that suddenly appears (60 dB above the noise) passes unsuppressed for between 5 and 10 s before it is treated as noise; the oracle suppresses it in under 5 s. That delay is the cost of protecting modulated sparse sounds such as voiced speech. At 32 and 48 kHz the vendor passes 160-sample slices of full-rate audio as a single band, so the NS runs its per-call constants three times faster in real time and its frequency axis is stretched (shared with the original; scaling the constants by the rate would be a compatible improvement, not yet measured). The multi-band path has no known caller and has never run on real audio.

---

### 3.3 Modern Acoustic Echo Cancellation (AEC)

#### Legacy WebRTC Limitations
- Employs 12 partitioned time/frequency NLMS blocks, multi-subband cross-correlation delay estimators, and coarse non-linear suppression (NLP).

#### Proposed Modern Design: Partitioned Block Frequency-Domain Adaptive Filter (PBFDAF)
- **Frequency-Domain Block Filtering**:
  - Partitions the echo path into $P$ blocks ($P = 4 \dots 8$, covering 60–120 ms of acoustic impulse response).
  - Performs filter multiplication in the frequency domain ($O(N \log N)$ operations instead of $O(N^2)$ time-domain NLMS).
- **Normalized Gradient Adaptation**:
  $$W_{m+1}(k) = W_m(k) + \mu(k) \cdot \frac{E(k) X^*(k)}{|X(k)|^2 + \lambda}$$
  where $\mu$ is initialized and decayed according to `mu_min` and `mu_decay` from `webrtc_profile.ini`.
- **Coherence-Based Double-Talk Detector (DTD)**:
  - Continuously calculates normalized cross-power spectral density between far-end speaker signal $x[n]$ and near-end microphone signal $d[n]$.
  - When cross-correlation drops below thresholds `cor_thd1..4`, near-end speech (double-talk) is detected, freezing filter updates to prevent filter divergence.
  - **Chassis Acoustic Feedback & Mechanical Bleed Provision**:
    In compact Thingino IP camera enclosures and video doorbells, the built-in speaker is located mere centimeters from the microphone capsule. High-amplitude speaker output vibrates the camera's plastic chassis and PCB, creating high-magnitude, non-linear structural acoustic feedback. Without compensation, this intense structural coupling can easily fool standard cross-correlation detectors into misclassifying echo as near-end speech, permanently freezing adaptive filter convergence during loud speaker playback. The DTD includes an adjustable structural coupling bias margin ($\Delta_{\text{chassis}} \approx 3\text{--}6\text{ dB}$) so that enclosure vibration does not trigger false double-talk states while still instantaneously freezing updates during genuine human near-end speech.
- **Residual Echo Suppression**:
  - When `nlp_mode` is set (0=conservative, 1=moderate, 2=aggressive), attenuation is scaled during single-talk periods, matching Ingenic's `safe_suppression` parameter.
- **Complexity**: ~500 lines of C11 vs. 7,000 lines of WebRTC AEC.

---

### 3.4 Dedicated Radix-4 / Split-Radix Real FFT

#### Legacy WebRTC Limitations
- Ooura's `fft4g.c` (1,332 lines) is a general-purpose arbitrary-size FFT. It dynamically allocates bit-reversal tables and twiddle factor tables in RAM.

#### Proposed Dedicated Real FFT
- **Specialized for $N = 128$ and $N = 256$** *(not re-verified: the implemented NS uses 256-, 512- and 1024-point transforms and the shared `src/fft.c`, so this section describes a proposal that has not been implemented for NS)*:
  - Hardcodes bit-reversal permutation and sine/cosine twiddle constants in `.rodata`.
  - Implements an unrolled 3-stage Radix-4 butterfly that computes real-to-half-complex transforms directly in registers without intermediate buffer loads.
- **Impact**:
  - Eliminates dynamic initialization overhead (`makewt`, `makect`, `bitrv2`).
  - Reduces FFT code size from ~12 KB down to **< 2.5 KB** (> 12% total library size savings).

---

## 4. Specific Black-Box Oracle Requirements & Acceptance Criteria

### 4.0 Core Principle: The Oracle is a Floor, Not a Rigid Ceiling
The empirical oracle measurements detailed below define the **minimum acceptable baseline (floor)**. WebRTC M47 carries historical design compromises from 2008-era PC VoIP drivers. Clean-room C11 implementations are permitted—and explicitly expected—to surpass the oracle where modern signal processing excels:
- **Eradicating "musical noise"**: Modern spectral estimators must produce smooth, artifact-free noise floors rather than the swirling tone bursts typical of early spectral subtraction.
- **Dynamic AGC Transparency**: Faster transient response with zero pumping artifacts.
- **Linearity & Stability**: Elimination of unnecessary subband splitting phase distortion.

### 4.1 Empirical Oracle Baseline Measurements

Direct empirical profiling of the current WebRTC M47 Oracle running on MIPS32r2 yields the following ground-truth characteristics:

#### A. AGC Empirical Oracle Profile
Tested with `targetLevelDbfs = 3` (-3.0 dBFS target = 16,384 RMS) and `compressionGaindB = 12`:
- **-30 dBFS Input**: Measured Gain = **+11.9 dB** (Output = -21.1 dBFS, perfectly respecting the +12 dB max gain limit without noise blowing up).
- **-20 dBFS Input**: Measured Gain = **+11.6 dB** (Output = -11.4 dBFS).
- **-10 dBFS Input**: Measured Gain = **+9.6 dB** (Output = **-3.4 dBFS**, locking exactly onto target).
- **-3 dBFS Input**: Measured Gain = **+2.6 dB** (Output = **-3.4 dBFS**, locking onto target).
- **0 dBFS Input**: Measured Gain = **-2.9 dB** (Output = **-3.1 dBFS**, limiter active, zero digital overflow).

#### B. NS Empirical Oracle Profile
Tested on stationary Gaussian noise at 16 kHz:
- **Policy 0 (Mild)**: **6.8 dB** attenuation.
- **Policy 1 (Medium)**: **12.8 dB** attenuation (+6.0 dB deeper).
- **Policy 2 (Aggressive)**: **18.8 dB** attenuation (+6.0 dB deeper).
- **Policy 3 (Very Aggressive)**: **21.7 dB** attenuation (+2.9 dB deeper).
*(Notice the clean ~6 dB/step progression across policies 0, 1, and 2).*

*How the NS gates are measured in `tests/unit_tests.c`:* 10 ms frames of uniform white noise (amplitude about 0.1), attenuation in dB of the RMS ratio. The oracle figures above were recorded on Gaussian noise at 16 kHz; the unit test uses uniform noise and also exercises 8, 24, 32 and 48 kHz. Steady state is taken over frames 200-259; a separate loose sanity bound covers the start-up window (frames 30-59), which varies by several dB with the noise realisation.

#### C. AEC Empirical Oracle Profile
Tested with 20ms acoustic echo delay coupled at -4.4 dB ($0.6\times$ amplitude):
- **Echo Return Loss Enhancement (ERLE)**:
  - **Synthetic Simulation (Zero-Noise)**: **70.0 dB** steady-state suppression on perfect linear simulation.
  - **Physical Camera Hardware Expectations**: In real Thingino camera hardware, analog microphone preamp noise, ADC/DAC quantization, speaker harmonic distortion, and room reverberation limit raw linear ERLE to **30–40 dB**. The evaluation test suite explicitly distinguishes between synthetic verification checks and physical hardware target gates.
- **Convergence Time**: Reaches > 15 dB ERLE within 40 frames (400 ms).

---

### 4.2 Candidate Acceptance Criteria Derived from Oracle

Any clean-room replacement must match the empirical transfer functions of the oracle:

1. **AGC Acceptance Gates**:
   - Maximum digital boost cap must strictly match `compressionGaindB` (±0.5 dB error tolerance).
   - Compression target must lock to `-targetLevelDbfs` within **±0.5 dB** for all inputs between -15 dBFS and -3 dBFS.
   - For 0 dBFS full-scale inputs, output must remain at `-targetLevelDbfs` with **zero clipping samples** (`max_abs <= 32767`).
   - MIPS throughput must exceed **> 40 MSamples/s** (WebRTC baseline runs at ~24 MS/s).

2. **NS Acceptance Gates**:
   - Policy 0 attenuation: **6.8 dB ± 1.0 dB**.
   - Policy 1 attenuation: **12.8 dB ± 1.5 dB**.
   - Policy 2 attenuation: **18.8 dB ± 2.0 dB**.
   - Policy 3 attenuation: **> 21.0 dB**.
   - MIPS throughput must exceed **> 0.8 MSamples/s** (WebRTC baseline is bottlenecked at ~0.10 MS/s).

3. **AEC Acceptance Gates**:
   - **Steady-State ERLE on Single-Talk Far-End Echo**:
     - Synthetic test vector gate: **> 40 dB** (theoretical floor check).
     - Acoustic chamber / degraded VoIP gate: **> 25 dB** (physical hardware expectation is 30–40 dB max).
   - Convergence time to 15 dB ERLE must be **< 500 ms**.
   - Double-talk detection must freeze adaptation within **30 ms**, preserving near-end voice with **< 1.0 dB** distortion, while tolerating chassis acoustic feedback without false freezes.
   - MIPS throughput must exceed **> 0.15 MSamples/s** (WebRTC baseline is bottlenecked at ~0.01 MS/s).

### 4.4 Black-Box Oracle Verification Protocol
```
                   +------------------------+
                   |  Test Audio Battery    |
                   | (Speech, Echo, Noise)  |
                   +-----------+------------+
                               |
               +---------------+---------------+
               |                               |
               v                               v
    +----------------------+       +----------------------+
    |  WebRtc Oracle (.so) |       |  Candidate (.so)     |
    +----------+-----------+       +-----------+----------+
               |                               |
               \-------> Comparator <----------/
                            |
                 - ERLE >= Oracle ERLE
                 - SNR improvement >= Oracle SNR
                 - PESQ / STOI delta >= 0.00
                 - MIPS CPU Throughput >= 2.0x
                 - Binary Footprint <= 50%
```
Each candidate replacement will be validated through this automated comparator harness prior to merging.

### 4.5 Evaluation Findings (descriptive; not acceptance criteria)

These are measurements taken while building the NS, recorded so the quality gate can be improved on evidence. They do not change §4.2-§4.4. Scores are ViSQOL (speech lattice model, 16 kHz) deltas of the candidate against the WebRTC baseline unless noted; the CI gate passes a row at a delta of -0.05 or better.

**What the current CI gate covers**
- The NS rows use the first two clips of each noise type, all at noise gains 45-55, while the dataset spans gains 5-55. They are among the mildest conditions.
- Unprocessed audio passes 6 of the 8 NS rows (all car, office and road rows) and fails only the two babble rows, so the gate does not require any noise reduction.
- The competing-speaker ("Clean Voice Transparency") category produces no rows. The battery asks for SNR values 0, 6 and 12, but the dataset stores the strings 10, 20, 30 and 40 for its 56 competing-speaker clips, so nothing matches.
- The echo rows run the `hpf`, `ns`, `agc`, `drc` chain at 16 kHz.

**Metric versus human opinion** (76 unprocessed noisy clips, human MOS from the dataset's spreadsheet)

| | ViSQOL | Zimtohrli (full length) |
|---|---|---|
| Spearman rank correlation with humans | 0.94 | 0.96 |
| Bias (metric minus human) | -0.34 (road -0.54) | +0.36 |
| Mean absolute error | 0.39 | 0.41 |
| Pairs ordered wrongly (human gap >= 0.5 MOS, of 2078) | 67 | 21 |

- Both metrics rank noise conditions like listeners but differ in absolute level, so deltas are more trustworthy than absolute scores.
- ViSQOL floors near 1.0 on the harshest noise where humans give 2.4-3.1. Zimtohrli compresses mild noise into 4.58-4.99 and cannot resolve small differences there.
- The ViSQOL model appears to have been trained on TCD-VoIP data, so its agreement here is probably inflated.
- Human scores exist only for unprocessed degradations. None of this validates how either metric judges noise-suppression artifacts.

**Which metric is trusted**
- Lag-compensated, full-length Zimtohrli is the trusted metric. Zimtohrli does no internal alignment and is very sensitive to delay: a clean clip delayed by 96 samples drops from 4.96 to 4.34, and 160 samples scores higher than 96. Outputs must therefore be advanced by their known delay (96 samples at 16 kHz, 48 at 8 kHz) before scoring; scoring uncompensated outputs against a zero-delay reference biases every comparison against the noise suppressor. CI's `compute_mos` truncates Zimtohrli to 3.5 s and does not compensate delay. ViSQOL aligns internally and is reported alongside as a second opinion. The metrics disagree in places (for example car noise), and neither has been validated on suppressed audio.
- Earlier statements in the project history that the NS is clearly better than the baseline on stationary noise were ViSQOL results and do not hold under Zimtohrli for earlier builds.

**Noise files outside the 8 gate clips** (68 files, split by speaker into a development half, speakers FA and MK, and a test half, speakers FG and ML; the test half has been scored once, for an earlier build under ViSQOL only, and is not otherwise used for decisions)
- Development half, lag-compensated Zimtohrli, delta against the oracle, files within -0.05 (current build):

| | Mean delta | Pass | babble | car | office | road |
|---|---|---|---|---|---|---|
| 16 kHz, policy 1 (default) | +0.026 | 30 of 34 | +0.10 (7/7) | -0.03 (7/9) | +0.05 (8/9) | +0.01 (8/9) |
| 16 kHz, policy 2 | +0.023 | 29 of 34 | +0.02 (6/7) | -0.03 (7/9) | +0.09 (7/9) | +0.02 (9/9) |
| 8 kHz, policy 1 | +0.024 | 28 of 34 | +0.16 (7/7) | -0.03 (6/9) | -0.03 (7/9) | +0.02 (8/9) |

- The 8 gate clips score within +0.01 to +0.02 of the oracle and of unprocessed audio under Zimtohrli, so they cannot separate builds.
- On the same set unprocessed audio scores 0.12 below the oracle (3.46 against 3.58), so the oracle does improve on doing nothing and so does the current build (3.61).
- The previously committed clean-room NS scored -0.30 against the oracle; an intermediate build with a plain MCRA tracker scored -0.06.
- Dev was used to choose among about ten builds, so these figures are optimistic; the test half is reserved for one final run.

**Test half** (34 files of speakers FG and ML, scored once on a clean build of commit `69ade63`; the rule was written down before the run: at policy 1, at both rates, the overall mean must be at least 0 and no noise type may average below -0.05; no constant was changed on the basis of these files)

| Zimtohrli, delay-compensated | Mean delta | Pass | babble | car | office | road |
|---|---|---|---|---|---|---|
| 16 kHz, policy 1 | +0.015 | 29 of 34 | +0.03 | +0.01 | +0.02 | +0.01 |
| 16 kHz, policy 2 | +0.022 | 27 of 34 | -0.07 | +0.02 | +0.08 | +0.04 |
| 8 kHz, policy 1 | +0.036 | 29 of 34 | +0.13 | -0.00 | +0.02 | +0.02 |

- The rule is met at both rates (policy 1). The development half gave +0.026 and +0.024, so the result generalises; the weakest type on both halves is car, at about zero.
- ViSQOL on the same files, report-only: 16 kHz policy 1 +0.056 (24 of 34), where babble is -0.09; 8 kHz +0.003 (19 of 34). The metrics disagree about babble at policy 1.
- The test half is now spent as a holdout. The next independent check is real camera recordings.

**Throughput** (x86 host, `bench_and_test`; the MIPS figure has to come from CI)
- NS throughput on x86: WebRTC baseline 18-19 MS/s, current NS 34-40 MS/s (about 1.9-2.1x). On the CI MIPS build under QEMU: baseline 0.26 MS/s, candidate 0.56 MS/s (+116%, about 2.2x); this was measured for the previous tracker, which has the same cost to within 3%.
- Instruction count per 1000 frames at 16 kHz (callgrind): previously committed NS 100.0M, the first quality-tuned design 150.9M, after speed-only changes 127.1M, current low-delay build 70.4M. The FFT is about half of the work.
- §4.4's 2.0x criterion held in the CI MIPS/QEMU run. The absolute figure of §4.2 (above 0.8 MS/s) is not met under QEMU, but QEMU's absolute numbers are not comparable with the real-hardware figures that criterion was derived from; a real-device measurement is outstanding.

**Latency**
- The vendor-twin NS and the current NS both delay audio by 96 samples (6.0 ms) at 16 kHz. The earlier neo filterbank delayed by 160 samples (10 ms). Replacing it cost little in quality: the ideal-mask ceiling at the policy-1 floor over 42 clips was 3.618 against 3.622, and on the development half at policy 1 the real estimator passed 22 of 34 files against 23 (per-type mean delta within 0.03).

**Test methodology**
- The NS attenuation gates are measured in steady state, over frames 200-259 of uniform white noise; the start-up window (frames 30-59) varies by several dB with the noise realisation and carries only a loose sanity bound.
- Regression guards (speech transparency, speech after digital silence, scale independence, in-place equality, state reset) use synthetic signals and are verified to fail on earlier builds that had those defects.

**Caller-side sweep of 38 vendor `libimp.so` builds**
- Scope: one build per SoC and SDK version (A1, C100, T20, T21, T23, T30, T31, T32, T33, T40, T41; mostly glibc) from `gtxaspec/ingenic-lib` at `ea62f3d`, caller side only (call sites, argument registers, constants). The scripted check lives outside this repository.
- Result: 37 of 38 builds satisfy all four invariants: `ns_process` is called with the literal band count 1 (no build has a multi-band caller), no clamp sits between `trunc.w.s` and the int16 store, `IMP_AI_EnableNs` accepts exactly the rates 8, 16, 32 and 48 kHz with `sltiu 4` on the mode, and `aec_process` receives a pointer to a 16-byte frame on `libimp`'s stack. The remaining build (T20 SDK 3.9.0) has no NS or AEC call site, an older SDK without the API; it is not a failure. Builds of the other variants of each version (uclibc, other compilers) were not checked.

**Modules changed since `475278e` (the pinned shipping oracle), same input and configuration**
- The exported map and every signature are unchanged; `audio_process.h` only gained an in-tree definition of `WebRtcAgcConfig` with the same layout.
- Rule, written before the run: *identical* = bit-exact; *refactor* = int16 modules within 1 LSB on at least 99.9% of samples, float modules within a relative RMS of 1e-5; *real change* = anything else. Inputs: seeded synthetic 5 s signals (speech-like, loud, quiet) at 16 and 8 kHz, plus sustained tones for the howling suppressor.
- HPF, LPF and DRC: bit-exact on every input. HS: bit-exact, except a 2.5 kHz howl at 16 kHz where 0.1% of samples differ by 1 LSB (the notch design changed from `sinhf` to `expf`). DRC+EQ: float rounding only (relative RMS at most 1.2e-6, largest difference 0.05 on values of thousands). NS: float rounding only (relative RMS about 2e-7 at 8 and 16 kHz, policies 0-3), caused by the FFT replacement. These are refactors.
- AGC: a real change (relative RMS 0.14-0.74 against the WebRTC AGC in every mode; the sources differ by design). AEC is a real change and is not yet evaluated; the host build of the `475278e` AEC runs the generic C path, not the device's MIPS path. Both need a quality evaluation, one pull request per module.

### 4.6 Known defects to fix

Defects found while checking the modernisation. They are quality bugs, not accepted behaviour, and each needs a fix with a regression test. A defect inherited from `475278e` is still a defect.

1. **FIXED (see the fix note below): Howling suppression (HS) ignores most of each call (inherited from `475278e`, unchanged in HEAD).** `audio_process_hs_process` works only on whole 128-sample blocks (`num_samples / HD_FFT_SIZE`) and leaves the remainder untouched, with no carry-over between calls. Device impact depends on the call length, which is **not** the 10 ms slicing used by the NS (see item 2: the vendor passes a per-frame count from the audio I/O attributes, probably the configured samples per frame). The mechanism is verified in isolation: any call shorter than 128 samples processes nothing, and for any length the remainder after the last whole 128-sample block is neither filtered nor analysed and no state carries over between calls. With 160 samples per call that leaves 32 samples (20%) unprocessed; with 80 samples per call HS would do nothing. In the harness (10 ms calls) a sustained 2.5 kHz howl passed through unchanged at 8 kHz in both builds. Verified by reading `src/howling.c` and by running both builds on sustained tones. Fix: buffer the input across calls, or process any length with streaming state, and add a test that HS attenuates a steady tone at 8 and 16 kHz with 80- and 160-sample calls.

2. **FIXED (see the fix note below): `audio_process_hs_process` is declared with three arguments but the vendor `libimp` calls it with four (inherited from `475278e`, unchanged in HEAD; argument count verified, meaning of the fourth argument not).** In the 19 of 38 sampled `libimp.so` builds where the call site could be located (T23, T32, T33, T40, T41), the call is `hs_process(handle, X, X, n)`: the same register holds both buffer arguments (an in-place input/output pair) and the fourth argument is loaded from a field at offset 0x10 of a frame structure. The fourth argument is a **byte count** of the captured mono 16-bit frame, not a sample count (verified on T23 SDK 1.3.0, caller side). The base object at the HS call is the one the record thread keeps in its stack slot for the whole loop; the same field is read just before the per-block loop, where it is divided by twice the per-call block size (80 samples at 8 kHz, 160 otherwise, a global that `IMP_AI_EnableAgc` sets) to get the number of blocks, and the buffer pointer advances by that product. The match of offset 0x10 with `numPerFrm` in the public `IMPAudioIOAttr` header is a coincidence of layout, not the same object. A fix that treats the fourth argument as samples would process twice the buffer. openimp's own HS call passes three arguments, but its source says that is "thingino's libaudioProcess (audioProcess-neo) API", so it follows this library's declaration and is not independent evidence of what the vendor passes. The three-argument declaration entered with the restructuring commit `353a2bb`; no source for it is recorded. On T31 SDK 1.1.1 `libimp` contains no `audio_process_hs_*` names at all (HS is not wired there); it is wired on T23, T32, T33, T40 and T41 builds. This library's signature is `hs_process(handle, data, num_samples)`, so it would read the second buffer pointer as the sample count. If HS were enabled on a device the call would treat a pointer as a length and run far past the buffer, or do nothing, depending on the value. HS is off by default on every camera, which is probably why it has gone unnoticed. Needed: confirm the real signature (the vendor library's behaviour must be matched from the caller side only; ask the Thingino maintainers whether HS has ever been enabled on hardware), then fix the signature and add a test that calls it the way `libimp` does. Related unverified item: `hs_create`, `lpf_*` and `drc_*` call sites were not located in the vendor binaries, so their signatures rest on reports only.

**HS fix (items 1 and 2).** `audio_process_hs_process(handle, data, arg2, arg3)` now accepts both call shapes: the vendor `(handle, buf, buf, num_bytes)` and the original `(handle, data, num_samples)` that openimp uses. The vendor call is recognised by pointer equality (the second and third arguments are the same address), which holds wherever the buffer lives; every other call is the sample-count shape, with a count of any size. (Two earlier versions used an address or size threshold: a 16 MB threshold would have misread a MIPS32 heap buffer below 16 MB, and a 65536 threshold made the MIPS throughput benchmark, which passes a whole buffer as the count, segfault under QEMU in CI. No caller uses a distinct output pointer, so that shape was removed.) `test_howling_low_address` maps a buffer at 0x00600000 and crashed the first version.) Any length is processed: samples are filtered as they arrive, the 128-sample analysis block fills across calls, and notches found from a completed block apply from the next sample (so a notch can start up to one block, 8 ms at 16 kHz, later than before). The unit test `test_howling_call_shapes` checks, at 8 kHz with 80-sample calls and 16 kHz with 160-sample calls: more than 3 dB attenuation of a steady tone; bit-identical output for call sizes 80, 160, 100, 333 and the whole stream; the vendor shape equal to the sample-count shape; and a separate output buffer. It fails on the previous code at the first assertion. Sanitizers are clean; output on a signal with no howling is unchanged. The fix keeps the existing behaviour of fading the notch in over the first 32 samples of every 128-sample block.
- Observation, not yet measured: that per-block fade-in applies even when the notch is steady, which would amplitude-modulate the notched band at the block rate (125 Hz at 16 kHz). Measure it on a steady notch before deciding whether it is a defect.

3. **The contract is the root cause of this defect class.** The vendor interface passes lengths inconsistently (NS has no length and implies 80 or 160 samples, `drc_eq_process` has none and implies 10 ms, `hs_process` has a length that is honoured only in multiples of 128 and, per defect 2, may have a different shape altogether, `aec_frame` carries a byte count and a reserved word), and the semantics are not written down anywhere the vendor owns. Each entry point needs an explicit contract: arguments as called by `libimp` (verified from the caller side), units (samples or bytes), the frame length it accepts, in-place use, state lifetime, and what happens on a short or long buffer. Until a versioned v2 interface exists, this section is the contract of record, and every function must be checked against the vendor call site before it is changed.

**Process.** Every module evaluation (AGC, AEC, and any later change) includes a defect sweep next to the quality scores: call-size independence (80, 160 and 480 samples per call), digital silence, full-scale input, 8 and 16 kHz, state reset, and in-place use. Every defect found goes in this section whether or not it is a regression.

**Observations still to investigate (not yet classified as defects).**
- AGC output depended on how the stream was cut into calls (up to 566 counts between 80 and 160-sample calls). Resolved: the rewritten AGC gives bit-identical output for any call size (section 4.8).
- `audio_process_drc_eq_process` always processes `rate / 100` samples and has no length argument (an ABI property); a caller passing a shorter buffer would be read past its end. Confirm `libimp` always passes a full 10 ms frame, and consider documenting or guarding it.
- HPF, LPF and DRC give identical output for 80-, 160- and 480-sample calls in both builds.

### 4.7 Contract audit: how the vendor `libimp` uses each entry point (caller side)

Method: for 38 `libimp.so` builds (one per SoC and SDK version, `gtxaspec/ingenic-lib` at `ea62f3d`), every `audio_process_*` name present in the binary was located, and its call sites were found by function slot (a static or code-built `{name, slot}` table, then `lw $25, slot; jalr`). Two states are kept apart: a name that is **not present** (not wired on that SoC) and a name that is **present but has no located call site** (marked `*`; it may be loaded and never called, or called in a way the script does not match). Only the caller side was read.

| Entry point | Wired on | Located call sites | Caller-side contract seen | Matches this library |
|---|---|---|---|---|
| `aec_process` | all | all | `(handle, &frame)`; frame is a 16-byte stack object | yes |
| `aec_create` | all | none located by the script* | `aec_create(rate, path)`, verified by hand on T23 1.3.0 and T31 1.1.1 (the script's locator misses it) | yes |
| `ns_*`, `agc_*` | all | yes | `ns_process(h, in, 1, out)` four arguments and `ns_set_config(h, rate, policy)`, verified; `agc_process` call sites located on most builds, arguments not re-derived | NS yes; AGC arguments not re-derived here |
| `hpf_create`, `hpf_process` | all | yes | `hpf_process(state, buf, count)` three arguments | yes |
| `hpf_free`, `agc_get_config`, `ns_get_config` | all | none located* | loaded by `dlsym`; no call found | n/a |
| `hs_process` | T23, T32, T33, T40, T41 (not T31, A1, C100, T20, T21, T30) | yes (19 builds) | **`(handle, buf, buf, n)` four arguments**; `n` loaded from offset 0x10 of an audio frame/attribute struct, most likely samples per frame | **no (three arguments)** |
| `hs_create` | same as `hs_process` | none located on T23, T32, T33* | not established | unverified |
| `drc_eq_process` | T32, T33 | yes | `(handle, in, out)` three arguments, no length | yes |
| `drc_eq_set_config` | T32, T33 | yes | **nine arguments**: handle, then eight scalars read from consecutive words (offsets 0x0-0x1c) of a configuration struct; three register slots and five stack slots, all stored before the call and not read back (T32 1.0.4, T32 2.2.0, T33 2.0.2.1) | **no: this library declares eight** |
| `drc_create/set_config/process/free` | **T31** and T41 | yes (T31 `drc_create` and `drc_set_config` not located*) | not yet read | unverified |
| `lpf_create`, `lpf_process` | T32, T33 | yes (through a `libimp` wrapper that halves a byte count) | not yet read | unverified |

Findings from the audit:
- The header comment ties plain DRC to "T41 ZRT", but `libimp` also references it on T31 builds (T31 is the main Thingino SoC), so DRC is reachable from `libimp` on the most common camera (it is off by default), not a niche path.
- `hs_process` argument-count mismatch and the `drc_eq_set_config` argument-count question are defects to resolve before anything is shipped that enables those effects. The `drc_eq_set_config` count is now confirmed on three builds. What is not known is which position the extra argument has: there is no public header for this struct, and the call only shows that the values are loaded from one eight-word structure, not which words are pointers. An extra trailing argument would be harmless under the o32 calling convention, but one in the middle would shift the `freqs`, `gains` and `qs` pointers and crash. The effect is off by default and exists only on T32 and T33.
- Every effect is off by default on every camera, which explains how these mismatches went unnoticed.

**`drc_eq_set_config` trace (caller side, T32 SDK 2.2.0).** `IMP_AI_EnableDrcAndEq(attr, cfg, a2, a3)` checks that the audio attribute has 16-bit samples and a frame length that is a multiple of 10 ms, creates the effect with `drc_eq_create(rate, 1)`-style arguments, then forwards the eight words at offsets 0x0-0x1c of `cfg` unchanged as arguments 2-9 of `drc_eq_set_config`. The extra two arguments of `IMP_AI_EnableDrcAndEq` are saved and never used afterwards. Nothing in `libimp` dereferences the words, and there is no public header for the struct (none in the OpenIMP headers, and OpenIMP has no T32 or T33 build), so the caller side cannot say which words are pointers. For comparison, the DRC-only entry point on T41 ZRT 1.2.0 is called with eight arguments, two of them floats on the stack, while this library's `drc_set_config` has five integer parameters; there the surplus is trailing, so it is harmless under the o32 convention. By analogy the vendor DRC+EQ call may carry an extra scalar (a make-up gain) before the EQ band count, which would shift the pointer arguments, but that is an inference.
- Needed to settle it: the layout of the application-side struct (ask Thingino or the maintainer whether DRC+EQ has ever been enabled on T32 or T33 hardware and by what code), or behaviour of the vendor library, which this project does not read.
- Until then, treat `drc_eq_set_config` as possibly ABI-incompatible with `libimp` on T32 and T33. The effect is off by default and exists only there.

### 4.8 AGC evaluation (oracle `475278e` against the branch; descriptive; does not change §4.2-§4.4)

In the first part of this section (the first development run, the tone gates and the first defect list) "HEAD" means the AGC before the rewrite (`7807a46`); the later part describes the rewritten AGC.

**Rule, written before scoring.** Config: target 3, compression gain 9 (12 for the tone gates), limiter on, modes 2 and 3, 16 kHz, 10 ms calls. Data: TCD-VoIP clean reference clips resampled to 16 kHz, development speakers FA and MK (10 clips each); the test speakers FG and ML are untouched. Input levels (active-speech RMS) -45, -35, -25, -20, -15 dBFS; the first run used -8 dBFS and wrapped around int16 because speech peaks exceed full scale, so the levels were changed before any scored run. Metrics: M1 level error against `min(-target, level + gain cap)` (pass: median |error| at most 1.5 dB and at most 0.5 dB worse than the oracle); M2 gain smoothness, the standard deviation of the frame-to-frame gain change in active speech (pass: at most 1.25 times the oracle); M3 fidelity, Zimtohrli of the output divided by its mean applied gain against the input, full length, lag logged (pass: median change at least -0.05); M4 clipped samples (pass: no more than the oracle); M5 noise gain in pauses (pass: no more than the cap + 0.5 dB). Cross-correlation lag was 0 in all 200 runs, as expected for a gain stage.

**First development run, before the fix: FAIL on M1, M2 and M4; M3 passes overall.**

| Input level (dBFS RMS) | Mode 3: median \|level error\| oracle / HEAD (dB) | Gain-change std oracle / HEAD (dB) | Clipped samples oracle / HEAD | Zimtohrli change (HEAD - oracle) |
|---|---|---|---|---|
| -45 | 0.13 / 0.01 | 0.01 / 0.50 | 0 / 0 | -0.019 |
| -35 | 0.13 / 0.01 | 0.00 / 0.17 | 0 / 0 | -0.013 |
| -25 | 1.05 / 3.11 | 0.23 / 0.26 | 0 / 0 | -0.106 |
| -20 | 3.63 / 7.64 | 0.61 / 0.48 | 4 / 36 | -0.078 |
| -15 | 7.92 / 11.99 | 0.79 / 0.61 | 7 / 171 | -0.053 |

Medians over 20 clips. In mode 2 the oracle itself is 3 dB below the ideal at -45 and -35 dBFS (median error 3.11 dB, HEAD 0.01 dB), and HEAD is again further from the ideal at -20 and -15 dBFS and clips more (207 against 6 samples), so mode 2 also fails M2 and M4 but the level comparison differs at low input. The absolute part of M1 is badly calibrated: it compares an RMS level of speech with a target that both implementations apply to peaks, so the oracle itself exceeds 1.5 dB at high levels; the relative comparison is what carries the result. Overall medians: M1 3.11 against 1.05 dB; M2 0.44 against 0.23; M3 -0.027; clipped samples 207 against 11.

**Tone gates (1 kHz sine, mode 3, target 3).** Oracle output peak lands near -3 dBFS (RMS -6.2 dBFS) for inputs of -10 to 0 dBFS; HEAD settles at RMS -9.0 dBFS for inputs of -15 to 0 dBFS, 3 dB lower, so the §4.2 target-lock gate (within 0.5 dB of the target for -15 to -3 dBFS) is not met by HEAD. A full-scale tone clips 86 samples in HEAD (122 with gain 12) and none in the oracle (the output reaches -32768 during start-up). The gain cap is met (+9.0 dB, oracle +7.9 to +8.0).

**Defects found (to fix; each needs a regression test):**
1. **The AGC target level is 3 dB too low.** `agc.c` sets the target to `32767 / sqrt(2) * 10^(-target / 20)` and treats it as a peak (16384 peak is -9 dBFS RMS for a sine), whereas the oracle, and §4.1, lock the output peak near -target dBFS. The unit test only checks "amplifies" and "no overflow", so the level was never measured.
2. **Clipping on loud input and at start-up.** The gain starts at the maximum, so a loud first frame is boosted before the envelope reacts, and the "soft" limiter keeps only a 1/8 slope above 32000 and then clamps, so large overshoots end at the rail. HEAD clips 207 samples over the development set against 11 for the oracle.
3. **Gain ripple.** The gain follows a 1 ms peak envelope, so its frame-to-frame change is about twice the oracle's at low levels (std 0.50 against 0.01 dB at -45 dBFS), which risks audible pumping.
4. **The `mode` argument is stored and never read**, so modes 0-3 behave identically; the oracle's modes 2 and 3 differ (for example a different level at low input).
5. **Call length.** The frame is cut into ten subframes of `samples / 10`; any remainder (for example 333 samples) is not processed at all. Output also depends on the call size: 80, 160 and 480-sample calls differ by up to 566 and 1498 counts in HEAD (up to 12179 in the oracle, relative RMS 0.63 against 0.02). `libimp` calls with 80 or 160 samples, so the remainder case does not occur there, but the contract should be explicit.

**Amended rule and the fix (written before the fixed AGC was run).** The first dev run did not measure pauses (the clean references are digital silence between words, so M5 showed exactly the cap) and its M1 compared an RMS level with a peak target. Evidence gathered before amending: `libimp` passes mode literal 2 at 42 of 63 `agc_set_config` call sites (the other 21 load it from memory and were not traced); the camera defaults are target 10 dBFS, compression gain 0 and AGC off, in raptor's `rad_main.c` fallbacks (`agc_target_dbfs` 10, `agc_compression_db` 0, `agc_enabled` false), in its agent adapter and in prudynt-t. Amendment: config A (target 10, gain 0, the camera default) is primary and config B (target 3, gain 9) secondary; a white noise bed at -60 and -50 dBFS is added after scaling (masks come from the clean speech); M1' is the candidate's active-speech output level against the oracle's (pass within 1.0 dB at each level) plus the §4.2 tone gate; M5' is the gain on non-active frames (at most the cap + 0.5 dB, at most 1.0 dB above the oracle's, and a spread no more than 1.25 times the oracle's); clipped inputs are reported next to clipped outputs; M2 is judged on the median over all rows of a cell, as originally written. Before the fix, in config A HEAD cut speech by 1.6 dB at -25 dBFS and 5.1 dB at -20 dBFS below the oracle, and in config B it amplified a -60 dBFS noise bed by the full gain cap.

**The fix** rewrites `audio_process_agc_process` as a streaming fixed-point design (the sample loop uses integers only):
- The target is a peak, `32767 * 10^(-target/20)`, as in the shipped AGC. The gain starts at unity and is at most `10^(compressionGaindB/20)`, limited internally to 54 dB.
- A peak envelope is updated once per block of about a millisecond (a power of two in samples) from that block's largest sample: it follows the peak in eighths of the difference per block (about 8 ms attack) and falls over about 300 ms. The gain is recomputed once per block and ramps linearly across the next block (down 30% per block, up 1%).
- Gain is held, never raised, at the noise floor, in pauses and without speech. A pause is an envelope below 22% of a 3-second speech reference; pauses get a slow extra -2.3 dB attenuation (the shipped AGC takes 2.3 dB off steady noise as well). Speech means that the reference exceeds five times a noise-floor estimate (the envelope minimum, which may rise 2.6 dB per second) or that the peak exceeds about -33 dBFS, so steady tones are still boosted as the §4.2 tone gates require while steady noise below -33 dBFS is not.
- With the limiter on the output bends smoothly and cannot reach 32767. Everything advances by sample and block, so the output does not depend on how the stream is cut into calls, and every sample is processed.
- The pause constants and the envelope attack were chosen on the development half only. A first version of the speech test (envelope range only) refused to boost steady tones and left the noise floor estimate near zero for tens of seconds; a regression test that failed it led to the final rule.

**Result of the fixed AGC on the development half (mode 2; 20 clips; the figures are the final build).**

| Cell | M1' level vs oracle | M2 smoothness | M4 clipping | M5' pauses |
|---|---|---|---|---|
| A (target 10, gain 0), no bed | pass (worst -0.4 dB at -20 dBFS) | pass (0.135 vs 0.130) | pass | n/a |
| A, bed -60 | pass | pass | pass | pass (-2.2 vs -2.3 dB) |
| A, bed -50 | pass | pass | pass | fail at one level: -1.1 dB at -35 dBFS speech against the oracle's -2.3 (limit 1.0 dB above) |
| B (target 3, gain 9), no bed | fail (+2.9 to +3.0 dB at -45 to -25 dBFS, +2.4 at -20) | pass | pass | n/a |
| B, bed -60 and -50 | fail (same gap) | pass | pass | fail (pause gain about 3 dB above the oracle's) |

- Config B's gap has two causes. At -45 to -25 dBFS the gain is limited by the cap: HEAD applies the full +9 dB that `compressionGaindB` asks for (tones: +9.0), whereas the shipped AGC reaches about +6 dB on speech (+7.9 on tones, outside its own ±0.5 dB cap gate), and §4.2 requires the cap to match. At -20 dBFS HEAD's gain is about +6.7 dB, below the cap, yet 2.4 dB above the oracle's: that part comes from the envelope design (a slower release closes it to 1.1 dB but pushes config A to -1.3 dB, so it was not adopted). The pause failure follows from the higher speech gain.
- Zimtohrli (config B, bed -60, mode 3, 40 clip-level pairs): median change -0.021 against the oracle, pass.
- Tones (target 3, gain 12 or 9): the output peak settles within 0.1 dB of -target (RMS -6.0 dBFS against the oracle's -6.2) for inputs of -10 to 0 dBFS; a full-scale tone has no sample at the rails including start-up (the oracle also none; HEAD before the fix: 86-122); the cap is met (+9.0 dB). At 8 kHz with 80-sample calls the tone locks at -3.0 dBFS peak from -15 to 0 dBFS inputs (the oracle at -15 dBFS stays at -10.9) and output is bit-identical across call sizes (the oracle differs by up to 6134 counts).
- Noise-only stretches (10 s of white noise, the case a camera is in most of the time): config A gives the noise -2.3 dB, exactly the oracle's; config B gives -2.3 dB where the oracle gives +3.2 dB, so the noise is not boosted; after a speech burst the gain eases back without a step. Digital silence followed by speech is handled (silence stays zero; speech passes at the expected level).
- Throughput: native x86 495 MS/s against 468 for the previous branch AGC; under QEMU MIPS in CI 82.5 MS/s against 47.6 for the integer AGC on `main` in the same run, so the §4.2 gate (above 40 MS/s) is met. An intermediate float version ran at 5.5 MS/s there and an intermediate integer version at 18 and 29 MS/s; the gate was only visible in CI, not on the x86 host.
- The overall condition "M1-M5 pass on the development half" is therefore **not met**: config B fails M1' and M5' (cap honoured, plus the envelope effect at -20 dBFS) and config A fails one level of M5' in the -50 dBFS bed. The test half (FG, ML) has **not been scored**. Whether to accept these documented deviations and run the test half once is a decision for the maintainer.

**Defect status.** (1) target level: fixed. (2) clipping and start-up overshoot: fixed. (3) ripple: fixed (M2 passes in every cell). (5) call length: fixed (bit-identical output for 80, 160, 480, 100 and 333-sample calls; every sample is processed). New: stationary noise was lifted by the full gain cap in noise-only stretches: found by the defect sweep, fixed, with a regression test. (4) `mode` is still stored and ignored; `libimp` passes literal 2 at 42 of 63 sites, so the digital adaptive behaviour is what it gets, and the 21 non-literal sites remain to be traced. `test_agc_regressions` covers the tone level lock, rail samples, call-size independence, gain in pauses, stationary noise and the 64-bit multiply path for gains above 30 dB; it fails on the previous AGC at the first assertion.

**Test half (speakers FG and ML; scored once, now spent).** Same script, cells and rules as the development half, build identical to `450816d`, authorised by the maintainer with the two deviations above accepted in advance; no tuning was done on these speakers.
- Config A (the camera default): level (M1'), smoothness (M2) and clipping (M4) pass in all three cells; the oracle clips 147 input samples at -20 dBFS and the new AGC none. Pauses (M5') pass in the clean and -60 dBFS cells (-2.2 dB against the oracle's -2.3) and fail in the -50 dBFS bed, at the -45 dBFS level only (the pause is not detected at about 5 dB SNR, gain-cap 0.0 against -2.3 dB; the other three levels are within 0.2 dB).
- Config B: as on the development half, the level is +2.4 to +3.5 dB above the oracle (cap honoured; envelope effect at -20 dBFS) and the pause gain follows; in addition the ripple (M2) misses in the -50 dBFS bed (0.305 against a limit of 0.291, driven by the -45 dBFS level at 0.62), which the development half did not show.
- Zimtohrli (config B, bed -60, mode 3, 40 clip-level pairs): median change -0.024, pass.
- Net: the held-out result reproduces the development result for config A, apart from the same low-SNR pause level, and for config B it shows the same cap-related difference plus one low-SNR ripple miss. The pre-registered rule is not fully met; the deviations are the ones listed.

**Further checks of the rewritten AGC (defect sweep).**
- `set_config`: two calls before `process` give the same output as one, a call mid-stream with the same settings changes nothing, and a target change mid-stream ramps instead of stepping (the first rewrite reset the running state on every call, which would have dropped a +9 dB gain to 0 dB in the middle of speech; raptor's `set-agc` re-enables the AGC with new values while it runs, and it is not known whether `libimp` reuses the handle). Now only a change of sample rate restarts the state. Unit tests cover all three.
- Rates 8, 16, 32 and 48 kHz with the 160-sample call shape that `libimp` uses at the higher rates: the tone locks at -3.0 dBFS peak at every rate, speech lands at the same level as the oracle in config A (-24.3 against -24.2 dBFS at -25 dBFS input), steady noise gets -2.3 dB, and the output does not depend on call size. (A first check with 10 ms calls at 32 and 48 kHz made the oracle look inactive; that was the call shape.)
- Not yet covered: the multi-band path (`num_bands` above 1) has no test, and what `libimp` passes for `num_bands` has not been read.
- `num_bands` is the literal 1 at all 19 located `agc_process` call sites in the 38 sampled `libimp` builds, so the multi-band path of the AGC has no known caller (it is kept, untested, for the exported signature).
- The AGC mode: `libimp` exports `IMP_AI_SetAgcMode(int mode)` (accepts 0 to 3) and a global `agcMode` whose initial value is 2; the 21 call sites that load the mode from memory read that global. Neither raptor nor prudynt-t calls `IMP_AI_SetAgcMode`, so on Thingino the library always receives mode 2 and the ignored `mode` argument (defect 4) has no effect there. It stays a gap for other callers: modes 0, 1 and 3 behave like mode 2.

### 4.9 AEC evaluation, "before" row (oracle `475278e` against the branch; descriptive; does not change §4.2-§4.4)

No change was made to `aec.c`. This section records the measurement and the defects so that any fix is a separate, evidence-based decision.

**Facts fixed first.** (a) In all 48 located `aec_process` call sites of the 38 sampled `libimp` builds, the word at offset 0x8 of the frame (`reserved`) is stored from the same register as `near`: it is a second pointer equal to the output buffer, so the in-place write is consistent with the vendor call. (b) The vendor `webrtc_profile.ini` is installed from the SDK (`ingenic-sdk`) and was not available, so both libraries ran with no profile (library defaults: delay-agnostic, extended filter, moderate NLP, `delay_ms` 0). (c) In raptor the AEC is off by default and needs `ao_enabled`. (d) Calls carry two 10 ms frames (raptor's 20 ms capture frames). (e) `IMP_AI_EnableAec` has no rate constants (only a channel test), so 8 and 16 kHz are scored and 32/48 kHz are not.

**Rule, written before scoring** (rate 16 kHz primary, 8 kHz secondary): synthetic streams of far-end speech through a seeded echo path (direct delay d plus an exponentially decaying tail, RT60 150 ms, coupling -6 dB, mild loudspeaker soft clipping, microphone noise -65 dBFS), segments A far-only 8 s, B double talk 6 s, C far-only 5 s, D near-only 4 s, E far-only with a changed echo path 6 s; far and near speakers differ; d of 20, 60 and 100 ms (180 ms at 8 kHz); echo 0 and +6 dB against the near speech. Outputs are advanced by the measured latency (oracle 112 samples at both rates; candidate 160 at 16 kHz and 80 at 8 kHz) before comparison. Pass rules: R1 steady ERLE at least the oracle's minus 3 dB and at least 25 dB where the oracle reaches 25; R2 convergence to 15 dB under 500 ms and at most 100 ms slower than the oracle; R3 double talk: Zimtohrli change at least -0.05, near-speech gain within 1.0 dB, echo return loss at least the oracle's minus 3 dB; R4 recovery after double talk; R5 transparency with the far end silent; R6 reconvergence after a path change; R7 any delay row the oracle cancels (15 dB) and the candidate does not is a fail. The test speakers (FG, ML) are unspent.

**Result on the development half (medians over 4 instances per row; steady ERLE in dB).**

| Rate | Path delay | ERLE oracle / candidate | Convergence to 15 dB (s) | Reconvergence after path change | Double-talk near-speech gain (dB) | Zimtohrli change in double talk | Verdict |
|---|---|---|---|---|---|---|---|
| 16 kHz | 20 ms | 31.9 / 23.8 | 0.91 / 1.52 | 0 / 4.7 s (3 of 4 ever) | -5.0 to -6.4 / -1.5 to -2.7 | +0.20 to +0.25 | R1, R2, R6 fail |
| 16 kHz | 60 ms | 32.6 / 9.2 | 1.20 / 2.54 | 0 / never | -4.3 to -5.8 / -1.1 to -2.0 | -0.43 to -0.55 | R1, R2, R6, R7 fail |
| 16 kHz | 100 ms | 34.3 / 1.8 | 1.21 / never | 0 / never | -4.1 to -5.7 / -0.4 to -0.8 | -0.87 to -0.89 | R1, R2, R6, R7 fail |
| 8 kHz | 20 ms | 34.2 / 22.2 | 0.91 / 2.08 | 0 / 3.3 s (1 to 2 of 4 ever) | -5.6 to -7.8 / -1.3 to -2.4 | -0.27 to -0.68 | R1, R2, R6 fail |
| 8 kHz | 60 ms | 34.9 / 22.0 | 1.21 / 1.90 | 0 / 3.9 s (1 of 4) | -5.0 to -7.0 / -1.2 to -2.2 | -0.18 to -0.30 | R1, R2, R6 fail |
| 8 kHz | 100 ms | 35.3 / 18.1 | 1.20 / 2.31 | 0 / 1.0 s (1 of 4) | -4.9 to -6.3 / -1.0 to -1.9 | -0.51 to -0.69 | R1, R2, R6 fail |
| 8 kHz | 180 ms | 35.1 / 1.4 | 1.27 / never | 0 / never | -4.7 to -6.3 / -0.5 to -0.9 | -0.68 to -0.76 | R1, R2, R6, R7 fail |

- Transparency with the far end silent (R5) is equal to the oracle in every row (level change within 0.01 dB, Zimtohrli change within -0.011), and a far end that is silent throughout leaves the microphone signal unchanged in both.
- Both libraries miss the §4.2 convergence gate (under 500 ms) on this harder synthetic path (long tail, nonlinearity, noise); the oracle needs 0.9 to 1.3 s. The §4.1 profile (20 ms delay, no tail) is easier.
- The shipped AEC attenuates the near-end speech in double talk by 4 to 8 dB; the candidate by 0.4 to 2.7 dB, which is why its double-talk fidelity is better at 20 ms.
- Steady ERLE is below the §4.2 degraded gate (25 dB) for the candidate in every row, and 8 to 33 dB below the oracle once the path delay reaches 60 ms at 16 kHz or 180 ms at 8 kHz.

**Defects found (to fix; each needs a decision and a regression test).**
1. **Delay reach.** The candidate's filter spans 16 partitions of one block, 64 ms at 16 kHz, 128 ms at 8 kHz and about 43 ms above 16 kHz, with no delay estimator, so any speaker-to-microphone latency plus tail beyond that is not cancelled (100 ms at 16 kHz: 1.8 dB). The shipped AEC estimates the delay itself. The profile `delay_ms` delays the far-end reference, which is the right direction (the echo arrives after the reference, so delaying the reference aligns it), but it needs the true latency to be known and put in the profile; there is no estimator. On real cameras the playback-to-capture latency is unmeasured; if it exceeds a few tens of milliseconds the AEC is ineffective.
2. **Convergence and path-change recovery are slow:** 1.5 to 2.5 s to 15 dB against 0.9 to 1.3 s, and after an echo-path change the ERLE recovers to 10 dB or less within 6 s (16 kHz, 20 ms) where the oracle holds 31 to 36 dB.
3. **Steady ERLE is 8 dB below the oracle even with a short path** (23.8 against 31.9 dB at 16 kHz and 20 ms).
4. **Added capture latency of one full 10 ms frame** (oracle 7 ms at 16 kHz, 14 ms at 8 kHz): the candidate buffers whole 64-sample blocks and returns zeros for the first 10 ms.
5. **Both libraries** leave samples that do not fill a whole 10 ms frame unprocessed (a 15 ms call cleans only the first 10 ms), so the contract is "multiples of 10 ms" and a caller that sends anything else leaks echo. Output is otherwise independent of 1, 2 or 4 frames per call.
6. **Throughput:** CI shows 0.03 to 0.04 MS/s for the candidate under QEMU MIPS against the §4.2 gate of 0.15 MS/s (x86 host 3.4 MS/s); the baseline column is the same code, so it says nothing against the WebRTC AEC. Real-hardware throughput is unmeasured.
- Caveats: the oracle host build runs WebRTC's generic C path, not the device's MIPS path; no vendor profile was available; the synthetic path is a model, not a camera chassis. The 8 kHz dev run is on the same speakers and variants (2 pairings, 2 variants).

**Update after the first AEC fixes (commits `7e64e51`, `561639e`; the test half FG, ML is still unspent).**

*Harness correction.* The loudspeaker soft clip used 3 x rms, which for speech leaves a distortion only 11-14 dB below the signal and caps any linear filter near 11-14 dB; the rule said about -30 dB. It is now 12 x rms (-26 to -30 dB). The "before" row above was measured with the 3 x rms harness; both the previous AEC and the fixed one were re-run under the corrected harness for the table below (both libraries always saw identical input, so the first comparison was fair, but its absolute numbers were pessimistic).

*What changed in `aec.c`:* the gradient constraint is applied to one partition per block, round robin (two transforms per block instead of 2 x partitions); the filter spans 32 partitions (128 ms at 16 kHz, 256 ms at 8 kHz); the step size is 0.4 falling to a floor of 0.3 (it was 0.25 falling to 0.05, so path changes were never recovered); and the echo delay is tracked: when the energy peak of the adaptive filter sits in partition 8 or later for 10 checks in a row, the reference is delayed by that amount and the weights shifted down. Tracking is skipped for tonal references (more than 85% of the far-end power in one bin; real speech has a median of 0.3 and 99th percentile 0.75, a tone 1.0; a first threshold of 0.2 switched tracking off on speech). A shift that does not improve the linear cancellation within 120 adapting blocks is undone, and a tracked delay is dropped when the linear cancellation collapses 10 dB below its recent best (path shortened).

*Result (corrected harness, development half, medians over 4 instances; oracle | candidate before | candidate after).*

| Rate, path delay | Steady ERLE A (dB) | Convergence to 15 dB (s) | Reconvergence after path change (s) | Near-speech gain in double talk (dB) |
|---|---|---|---|---|
| 16 kHz, 20 ms | 46.2 / 29.0 / 40.4 | 0.9 / 1.4 / 1.3 | 0.0 / 4.8 / 1.8 | -5.4 / -1.7 / -1.9 |
| 16 kHz, 60 ms | 52.4 / 8.6 / 38.6 | 1.2 / 2.2 / 1.6 | 0.0 / never / 1.9 | -4.6 / -1.1 / -1.7 |
| 16 kHz, 100 ms | 55.7 / 1.6 / 37.6 | 1.2 / never / 1.8 | 0.0 / never / 2.2 | -4.5 / -0.5 / -1.5 |
| 8 kHz, 20 ms | 45.9 / 28.3 / 29.2 | 0.9 / 1.8 / 1.8 | 0.0 / 2.0 / 1.8 | -6.7 / -1.5 / -2.0 |
| 8 kHz, 60 ms | 50.6 / 25.7 / 26.9 | 1.2 / 1.9 / 1.7 | 0.0 / 0.8 / 3.6 | -5.8 / -1.1 / -1.5 |
| 8 kHz, 100 ms | 56.9 / 18.4 / 21.2 | 1.1 / 2.3 / 2.2 | 0.0 / 4.0 / 1.5 | -5.5 / -1.1 / -1.5 |
| 8 kHz, 180 ms | 54.5 / 0.9 / 29.4 | 1.3 / never / 1.8 | 0.0 / never / 1.0 | -5.2 / -0.4 / -1.3 |

- Rule status on the development half: R1 steady ERLE: the candidate now meets the 25 dB absolute gate in every row except 8 kHz at 100 ms (21.2 dB), but is 6 to 28 dB below the oracle, so the relative clause (at most 3 dB below) still fails. R2 convergence: fails (1.3 to 2.2 s against 0.9 to 1.3 s; both are above the 500 ms of §4.2). R3 double talk: the Zimtohrli change against the oracle is positive in all 16 kHz rows (+0.16 to +0.28) and in 7 of 8 8 kHz rows; the exception is 100 ms with echo at the near-end level (-0.25). R5 transparency: unchanged, within 0.011. R6 reconvergence after a path change: improved (never recovering became 1 to 4 s) but still slower than the oracle. R7 delay reach: met in every row, including 180 ms at 8 kHz and 100 ms at 16 kHz.
- Path tracking, extra scenarios (16 kHz): path 100 -> 40 ms: 3.6 s to recover, 14.4 dB at the end (6.4 dB before the back-off was added; oracle 25.1); path 20 -> 100 ms: 3.7 s, 13.2 dB (oracle 19.4). The tracker only lengthens the delay or resets to the configured one.
- Throughput: host 10.75 MS/s against 5.2 before and 3.0 for the WebRTC AEC. Under QEMU MIPS in CI the candidate went from 0.03 to 0.06 MS/s, still below the §4.2 gate of 0.15 MS/s (open).
- Regressions seen: 8 kHz double-talk Zimtohrli at 100 ms with echo at the near-end level is -0.25 (it was -0.23); 8 kHz reconvergence at 60 ms went from 0.8 to 3.6 s in the echo+0 row. Steady ERLE on the 3 x rms harness is not comparable with the table above.

*New defects found.*
1. **Divergence on a harmonic reference (previous AEC).** On a clean voiced-speech-like far end (harmonics of a moving pitch, gated) with a pure delayed echo, the oracle reaches 108 dB ERLE at 20, 60 and 100 ms, the previous candidate -24 to -27 dB (it amplified the echo by about 25 dB), the current one 30.5, 41.2 and 16.9 dB. A stationary tone also exposed a stability margin: with the round-robin constraint a step of 0.8 diverges (linear ERLE -3 dB), with all partitions constrained it is stable (+11 dB); a step of 0.5 already failed the single-tone unit test. A divergence guard (cut the step when the output exceeds the input) is not implemented.
2. **The remaining ERLE gap is not in the gain floor.** With the corrected harness the oracle reaches 46 to 57 dB; lowering the suppressor floor ten-fold moved the candidate by under 1 dB. On the earlier harness the linear filter alone reached only 7.5 to 10.6 dB (about its nonlinearity ceiling) and the suppressor supplied 14 to 16 dB more. The shipped AEC's residual-echo estimation and suppression is the part still missing; candidate levers are a coherence-based residual-echo estimate and a variable step or shadow filter. Not implemented.
3. Unit-test weakness found by the maintainer: the single 500 Hz tone cannot distinguish a delay from its multiples and passes even when the filter is wrong. `test_aec_delay_paths` now covers broadband echo at 20, 60 and 100 ms (16 kHz) and 100 ms (8 kHz) and near-end transparency; it fails on the previous AEC.

*Not done.* The test half has not been scored; R1 (relative), R2 and R6 do not pass on the development half. The oracle host build runs WebRTC's generic C path, no vendor profile was used, and the echo path is a model.

