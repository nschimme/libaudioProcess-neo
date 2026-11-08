# Modern Replacement Architecture for WebRTC Subsystems in libaudioProcess-neo

## 1. Executive Summary & Motivation

`libaudioProcess-neo` currently serves as a 74 KB drop-in C11 replacement for Ingenic's proprietary 510–733 KB `libaudioProcess.so`. While the custom modules (`biquad.c`, `hpf.c`, `lpf.c`, `drc.c`, `eq.c`, `howling.c`) are lightweight and highly optimized, the vendored WebRTC M47 subsystem (`src/webrtc/`):
- Accounts for **> 80% of total binary footprint** (~60 KB of 78 KB).
- Relies on 2008-era fixed-point subband splitters (`splitting_filter.c`) and legacy code originally designed for desktop PC voice chat.
- Carries legacy code complexity (over 20,000 lines across WebRTC AEC, AGC, and NS).

By designing modern, clean-room C11 replacement algorithms using a **black-box oracle strategy**, we can:
1. Maintain **100% ABI compatibility** with Ingenic's `libimp.so` (matching all 31 exported symbols in `src/audio_process.h` and `libaudioProcess.map`).
2. Maintain **100% INI configuration compatibility** with `webrtc_profile.ini`.
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

#### Proposed Modern Design: Single-Pass Minimum Statistics + MMSE Spectral Subtraction
- **Full-Band STFT**:
  - Computes a single 128-point (8 kHz) or 256-point (16 kHz) real FFT on 10ms frames with 50% Hanning overlap, eliminating QMF subband filterbanks.
- **Minimum Statistics Noise Floor Tracker**:
  - Recursively tracks spectral power minima $P_{\text{noise}}(k)$ in each frequency bin over a 1.2-second sliding window without requiring a separate voice activity detector (VAD).
- **Over-Subtraction Spectral Gain Mask**:
  $$G(k) = \max\left(\beta,\; 1 - \alpha(\text{policy}) \cdot \frac{P_{\text{noise}}(k)}{P_{\text{noisy}}(k)}\right)$$
  where $\alpha$ is derived from `policy` (0=1.0, 1=1.5, 2=2.2, 3=3.0) and $\beta$ is a small spectral floor (-15 dB to -25 dB) to prevent "musical noise" artifacts.
- **Synthesis**: Invert via inverse real FFT and overlap-add back to the output frame.
- **Complexity**: ~250 lines of C11 vs. 3,500 lines of WebRTC NS.

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
- **Specialized for $N = 128$ and $N = 256$**:
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
