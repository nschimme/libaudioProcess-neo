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

Provenance is tagged on each item: **[reported]** means reported by the Thingino project from its reading of the `libimp` disassembly and `raptor` sources (not independently re-derived here); **[verified]** means checked against this repository's code or tests.

`libimp` embeds its own `audio_process.c`, loads `libaudioProcess.so` with `dlopen()` and resolves everything with `dlsym()` at runtime. A missing symbol is fatal for the whole audio pipeline. **[reported]**

- **Required symbols (19, exact spelling):** `audio_process_{aec,agc,ns,hpf,hs}_{create,process,free}` (15), plus `audio_process_agc_set_config`, `audio_process_agc_get_config`, `audio_process_ns_set_config` and `audio_process_ns_get_config`. Extra exports are ignored. **[reported]** All 19 are exported by the library, which exports 31 symbols in total. **[verified]**
- **Lifecycle:** `create` once per `IMP_AI_EnableX`, `set_config` right after, `process` every 10 ms frame from the capture thread, `free` on disable. **[reported]** `ns_free` releases the only allocation and there are no mutable statics in `ns.c` or `hpf.c`. **[verified]**
- **Frozen layouts:** `struct aec_frame { const int16_t *far; int16_t *near; int reserved; int num_bytes; }` (built on `libimp`'s stack) and `WebRtcAgcConfig { int16_t targetLevelDbfs; int16_t compressionGaindB; uint8_t limiterEnable; }` (passed by value to `agc_set_config`). **[reported]** Both match the definitions in `src/audio_process_internal.h` and `src/audio_process.h`. **[verified]**
- **NS call contract:**
  - `process(ns, const float *const *in, num_bands, float *const *out)` takes float samples **in int16 range**. **[reported]** The NS result is independent of scale (75-80 dB agreement between int16-range and +-1.0 input on a speech-in-noise clip, about 57-60 dB on a digitally silent clip), and in-place processing is bit-identical to separate buffers, including at 32/48 kHz. **[verified]**
  - `libimp` allocates **two 640-byte float band buffers**, so a call carries **at most 160 floats**. **[reported]** The NS therefore uses a band length of **160 samples at 16, 32 and 48 kHz and 80 at 8 kHz**, the same block length as WebRTC's NS (`blockLen`). The 8 kHz figure of 80 is **to be confirmed**: the reported text says "160 samples per 10 ms" for all four rates, but 10 ms at 8 kHz is 80 samples. If `libimp` passes 160 floats at 8 kHz, half of every output buffer would be left unwritten. The call does not carry its length, so the library cannot detect this.
  - Only 8, 16, 32 and 48 kHz reach NS (`IMP_AI_EnableNs` switches on the sample rate). **[reported]** 24 kHz and 44.1 kHz are accepted as an extension and use a full-rate 10 ms band (240 and 441 samples). **[verified]**
  - `num_bands` is reported as 1 (mono only). **[reported]** The NS additionally accepts WebRTC-style pre-split bands (`num_bands > 1` at 32/48 kHz): the upper bands follow the low band's gain and are delayed to stay aligned with it. This path is a compatibility extension, covered by unit tests only, and has never run on real audio. **[verified]**
  - **Open question:** at 32 and 48 kHz, are the 160 floats a downsampled or split low band, or the first 160 samples of a full-rate frame? The NS analyses them as 16 kHz audio, which is only correct in the first case.
  - `set_config(ns, sample_rate, policy)` receives the raw `audio.ns_level` value 0-3, which maps one to one onto WebRTC's `kLowSuppression` .. `kVeryHighSuppression`. **[reported]**
  - `ns_get_config` is a `void(void)` no-op that `libimp` calls and ignores; keep it callable. **[reported]** It is exported with that signature. **[verified]**
- **Default NS policy:** `rad_main.c` clamps `ns_level` to 0-3 and defaults to "moderate". **[reported]** The numeric value after the `RSS_NS_*` to SDK `NS_*` mapping is **pending confirmation**; if "moderate" is WebRTC's `kModerateSuppression` it is policy 1. For reference, the CI quality gate currently scores policy 2.
- **Call chain and runtime:** `raptor.conf [audio]` -> `rad_main.c` -> `hal_audio_enable_ns()` -> `IMP_AI_EnableNs` -> `dlopen` / `dlsym` of the NS entry points. NS and AGC are applied inside `libimp`'s own record thread, so frames returned by `IMP_AI_GetFrame` are already processed. The software AEC path (`audio_process_aec_process`, with `webrtc_profile.ini`) already includes NS, so the two should not be stacked. **[reported]**
- **AGC:** takes `int16` samples (a deliberate domain difference from NS). `agc_target_dbfs` maps to `targetLevelDbfs` and `agc_compression_db` to `compressionGaindB`. The mic level (`in_mic_level` in, `*out_mic_level` out) and `*saturation_warning` round-trip on every frame. **[reported]**
- **HPF/HS:** `int16` in place with caller-owned state buffers, no hidden globals. **[reported]**
- **Library:** MIPS o32, C linkage, `SONAME=libaudioProcess.so`, built against the target's libc flavour. **[reported]** The Makefile sets the SONAME. **[verified]**
- **Free to change:** everything internal, the INI schema (nothing else reads it) and extra exports. **[reported]**

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
- **STFT:** a 2L-sample window with a hop of L samples, where L is the band length (80 samples at 8 kHz, 160 at 16/32/48 kHz, 240 at 24 kHz), zero-padded to a real FFT of 256 points (8 kHz), 512 points (16, 24, 32 and 48 kHz) or 1024 points (44.1 kHz). At 32/48 kHz the band is analysed as 16 kHz audio. The window is a Vorbis power-complementary window, which has the same perfect-reconstruction property as the sine window with much lower sidelobes.
- **Noise tracking (MCRA-style):** speech presence per bin from the ratio of smoothed power to a minimum tracked over about 1.2 s. The noise estimate follows `|Y|^2` with these guards, each added to fix a measured failure: the upward update is capped (and frozen in speech frames), it falls quickly toward lower power, and it has an absolute floor so digital silence cannot drive it to zero.
- **Gain:** decision-directed a priori SNR feeds a Wiener gain, blended with the policy floor by an OM-LSA speech-presence probability (local frequency smoothing plus a frame-level term; the absence-prior cap is set per policy).
- **Policy floors** are fitted so that stationary-noise attenuation meets the §4.2 gates.
- **Latency:** one call of L samples (10 ms at 8 and 16 kHz; the overlap-add delay).
- **Synthesis:** inverse real FFT and overlap-add.
- **Known limits:** held-out results are strong on stationary noise (car, road) and weak on babble and office noise; the per-bin rules cannot reliably separate babble from speech. See the status notes in the project history for the per-type numbers.

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

**Held-out noise files** (68 noise files outside the 8 gate clips, split by speaker into a development half, speakers FA and MK, and a test half, speakers FG and ML; the test half has been scored once, for one earlier build, and is not otherwise used for decisions)
- All 68 files, earlier build, policy 2: 41 pass, mean delta -0.002. By type: car 17 of 18 pass (mean +0.25), road 14 of 18 (+0.12), office 9 of 18 (-0.08), babble 1 of 14 (-0.38).
- Development half, current build, policy 2: 20 of 34 pass (babble 0 of 7, car 8 of 9, office 5 of 9, road 7 of 9).
- Development half, current build, policy 1: 23 of 34 pass, mean delta -0.030; babble 1 of 7 (mean -0.25), car 9 of 9 (+0.13), office 6 of 9 (-0.05), road 7 of 9 (-0.01).
- On the development half the baseline improves on unprocessed audio by about 0.25 MOS on average (3.11 against 2.86 at policy 2), so avoiding harm is not the bar.
- The NS is clearly better than the baseline on stationary noise (car, road) and behind it on babble and office noise.

**Throughput** (x86 host, `bench_and_test`; the MIPS figure has to come from CI)
- NS throughput: WebRTC baseline 18.4 MS/s, the previously committed clean-room NS 32.8 MS/s, current NS 19.9 MS/s (+8% over the baseline).
- Instruction count per 1000 frames at 16 kHz (callgrind): previously committed NS 100.0M, current NS 150.9M. The extra cost is in the per-bin presence smoothing and `expf`, not in the FFT.
- The §4.4 criterion of 2.0x the oracle is not met on x86 by the current build.

**Test methodology**
- The NS attenuation gates are measured in steady state, over frames 200-259 of uniform white noise; the start-up window (frames 30-59) varies by several dB with the noise realisation and carries only a loose sanity bound.
- Regression guards (speech transparency, speech after digital silence, scale independence, in-place equality, state reset) use synthetic signals and are verified to fail on earlier builds that had those defects.
