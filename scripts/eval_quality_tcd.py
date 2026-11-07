#!/usr/bin/env python3
"""
TCD-VoIP & ViSQOL Objective Speech Quality Evaluation Harness
Evaluates Baseline vs Candidate shared libraries across multiple real VoIP scenarios.
Supports visqol-python (perceptual MOS-LQO 1.0 - 5.0) and objective SNR metrics.
Publishes a rich Markdown summary table to GITHUB_STEP_SUMMARY.
"""

import os
import sys
import argparse
import urllib.request
import zipfile
import io
import wave
import struct
import math
import subprocess

TCD_ZIP_URL = "https://codeload.github.com/nschimme/TCD-VOIP/zip/refs/tags/harte2015tcd"

# Comprehensive evaluation scenarios matching TCD-VoIP dataset structure:
# Format: (Scenario Name, Submodule, Clean Ref, Degraded File, Description)
SCENARIOS = [
    # 1. Noise Suppression Scenarios (Male & Female voices, different noise environments)
    ("Noise Supp (Male MK)", "ns",
     "TCD-VOIP-harte2015tcd/Test Set/noise/ref/R_10_NOISE_MK.wav",
     "TCD-VOIP-harte2015tcd/Test Set/noise/C_10_NOISE_MK.wav",
     "Male speaker with background street/babble noise"),

    ("Noise Supp (Female ML)", "ns",
     "TCD-VOIP-harte2015tcd/Test Set/noise/ref/R_18_NOISE_ML.wav",
     "TCD-VOIP-harte2015tcd/Test Set/noise/C_18_NOISE_ML.wav",
     "Female speaker with ambient acoustic room noise"),

    # 2. Acoustic Echo Scenarios
    ("Echo Rejection (Female FG)", "all",
     "TCD-VOIP-harte2015tcd/Test Set/echo/ref/R_15_ECHO_FG.wav",
     "TCD-VOIP-harte2015tcd/Test Set/echo/C_15_ECHO_FG.wav",
     "Far-end acoustic room echo coupled into microphone"),

    # 3. Dynamic Range & Clipping Protection
    ("Limiter Protection (FA)", "drc",
     "TCD-VOIP-harte2015tcd/Test Set/clip/ref/R_01_CLIP_FA.wav",
     "TCD-VOIP-harte2015tcd/Test Set/clip/C_01_CLIP_FA.wav",
     "Speech with severe analog/digital clipping harmonics"),

    # 4. Clean Speech Transparency (Ensuring filters do not distort pristine voice)
    ("Clean Speech Trans (FA)", "all",
     "TCD-VOIP-harte2015tcd/Test Set/compspkr/ref/R_01_COMPSPKR_FA.wav",
     "TCD-VOIP-harte2015tcd/Test Set/compspkr/ref/R_01_COMPSPKR_FA.wav",
     "Pristine studio speech; checks absence of processing distortion"),

    ("Clean Speech Trans (MK)", "all",
     "TCD-VOIP-harte2015tcd/Test Set/clip/ref/R_01_CLIP_MK.wav",
     "TCD-VOIP-harte2015tcd/Test Set/clip/ref/R_01_CLIP_MK.wav",
     "Pristine male speech; checks harmonic fidelity"),
]

def ensure_dataset(data_dir):
    os.makedirs(data_dir, exist_ok=True)
    needed = []
    for _, _, ref_p, deg_p, _ in SCENARIOS:
        if not os.path.exists(os.path.join(data_dir, os.path.basename(ref_p))):
            needed.append(ref_p)
        if not os.path.exists(os.path.join(data_dir, os.path.basename(deg_p))):
            needed.append(deg_p)

    if not needed:
        return

    print("Downloading TCD-VoIP evaluation dataset...")
    req = urllib.request.Request(TCD_ZIP_URL, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as resp:
        zf = zipfile.ZipFile(io.BytesIO(resp.read()))
        for _, _, ref_p, deg_p, _ in SCENARIOS:
            for p in (ref_p, deg_p):
                dest = os.path.join(data_dir, os.path.basename(p))
                if not os.path.exists(dest):
                    print(f"  Extracting {os.path.basename(p)}...")
                    with open(dest, "wb") as f_out:
                        f_out.write(zf.read(p))

def compute_snr(ref_wav, deg_wav):
    """Compute SNR in dB between reference and degraded/processed WAV."""
    with wave.open(ref_wav, 'rb') as r, wave.open(deg_wav, 'rb') as d:
        n = min(r.getnframes(), d.getnframes())
        r_raw = r.readframes(n)
        d_raw = d.readframes(n)
        r_s = struct.unpack(f"{n}h", r_raw)
        d_s = struct.unpack(f"{n}h", d_raw)

        sig_pwr = sum(s * s for s in r_s)
        noise_pwr = sum((s - d) * (s - d) for s, d in zip(r_s, d_s))
        if noise_pwr < 1e-12: return 999.0
        if sig_pwr < 1e-12: return 0.0
        return 10.0 * math.log10(sig_pwr / noise_pwr)

def compute_visqol(ref_wav, deg_wav):
    """Compute ViSQOL MOS-LQO (1.0 to 5.0) in speech mode with automatic 16kHz resampling."""
    try:
        import soundfile as sf
        import scipy.signal as sps
        from visqol import VisqolApi

        # ViSQOL speech lattice mode expects 16kHz audio
        x, sr_x = sf.read(ref_wav)
        y, sr_y = sf.read(deg_wav)

        ref_path = ref_wav
        deg_path = deg_wav

        if sr_x != 16000:
            x_16k = sps.resample(x, int(len(x) * 16000 / sr_x))
            ref_path = f"{ref_wav}_16k.wav"
            sf.write(ref_path, x_16k, 16000)

        if sr_y != 16000:
            y_16k = sps.resample(y, int(len(y) * 16000 / sr_y))
            deg_path = f"{deg_wav}_16k.wav"
            sf.write(deg_path, y_16k, 16000)

        api = VisqolApi()
        api.create(mode="speech")
        res = api.measure(ref_path, deg_path)

        if ref_path != ref_wav and os.path.exists(ref_path):
            os.remove(ref_path)
        if deg_path != deg_wav and os.path.exists(deg_path):
            os.remove(deg_path)

        return float(res.moslqo)
    except Exception as e:
        return None

def write_github_summary(results, baseline_so, candidate_so):
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return

    with open(summary_path, "a") as f:
        f.write("\n## 🎙️ Speech Quality Evaluation: TCD-VoIP & ViSQOL\n\n")
        f.write(f"- **Baseline Library**: `{os.path.basename(baseline_so)}`\n")
        f.write(f"- **Candidate Library**: `{os.path.basename(candidate_so)}`\n\n")
        f.write("| Scenario | Subsystem | Metric | Baseline | Candidate | Delta | Status |\n")
        f.write("|:---|:---:|:---:|:---:|:---:|:---:|:---:|\n")
        for r in results:
            status_badge = "✅ PASS" if r["pass"] else "❌ FAIL"
            f.write(f"| **{r['name']}** | `{r['mod']}` | {r['metric']} | {r['base']:.2f} | {r['cand']:.2f} | **{r['delta']:+.2f}** | {status_badge} |\n")
        f.write("\n> *Quality Criterion: Candidate perceptual score (ViSQOL MOS) or SNR must equal or exceed baseline (Δ ≥ -0.05).*\n\n")

def main():
    parser = argparse.ArgumentParser(description="Evaluate baseline vs candidate quality on TCD-VoIP dataset.")
    parser.add_argument("baseline_so", help="Path to baseline libaudioProcess.so")
    parser.add_argument("candidate_so", help="Path to candidate libaudioProcess.so")
    parser.add_argument("--data-dir", default="data/tcd_voip", help="Directory for dataset cache")
    parser.add_argument("--process-bin", default="./tests/process_wav_bin", help="Path to process_wav binary")
    args = parser.parse_args()

    ensure_dataset(args.data_dir)

    # Detect target architecture of the candidate library to choose runner and compiler
    file_info = subprocess.check_output(["file", args.candidate_so]).decode("utf-8", errors="ignore")
    is_mips = "MIPS" in file_info
    runner_cmd = []

    if is_mips:
        process_bin_mips = args.process_bin + "_mips"
        if not os.path.exists(process_bin_mips):
            print(f"Compiling {process_bin_mips} for MIPS...")
            res = subprocess.run(["mipsel-linux-gnu-gcc", "-std=c11", "-D_GNU_SOURCE", "-Isrc", "-O2", "tests/process_wav.c", "-Wl,--no-as-needed", "-lm", "-ldl", "-o", process_bin_mips])
            if res.returncode != 0:
                print("Failed to compile process_wav tool for MIPS.")
                sys.exit(1)
        args.process_bin = process_bin_mips
        runner_cmd = ["qemu-mipsel", "-E", "LD_PRELOAD=/usr/mipsel-linux-gnu/lib/libm.so.6", "-L", "/usr/mipsel-linux-gnu"]
    else:
        if not os.path.exists(args.process_bin):
            print(f"Compiling {args.process_bin} for host...")
            res = subprocess.run(["gcc", "-std=c11", "-D_GNU_SOURCE", "-Isrc", "-O2", "tests/process_wav.c", "-Wl,--no-as-needed", "-lm", "-ldl", "-o", args.process_bin])
            if res.returncode != 0:
                print("Failed to compile process_wav tool.")
                sys.exit(1)

    print("\n=========================================================================================")
    print(f" TCD-VoIP Quality Evaluation: Baseline vs. Candidate")
    print(f"   Baseline:  {args.baseline_so}")
    print(f"   Candidate: {args.candidate_so}")
    print("=========================================================================================\n")

    print(f"{'Scenario':<25} | {'Metric':<10} | {'Baseline':<10} | {'Candidate':<10} | {'Delta':<8} | Status")
    print("-" * 92)

    out_dir = os.path.join(args.data_dir, "output")
    os.makedirs(out_dir, exist_ok=True)

    results = []
    all_passed = True

    for name, mod, ref_p, deg_p, desc in SCENARIOS:
        ref_file = os.path.join(args.data_dir, os.path.basename(ref_p))
        deg_file = os.path.join(args.data_dir, os.path.basename(deg_p))

        base_out = os.path.join(out_dir, f"{os.path.basename(deg_p)}_{mod}_base.wav")
        cand_out = os.path.join(out_dir, f"{os.path.basename(deg_p)}_{mod}_cand.wav")

        # Process through baseline
        subprocess.run(runner_cmd + [args.process_bin, args.baseline_so, deg_file, base_out, mod], check=True)
        # Process through candidate
        subprocess.run(runner_cmd + [args.process_bin, args.candidate_so, deg_file, cand_out, mod], check=True)

        # ViSQOL MOS or SNR
        v_base = compute_visqol(ref_file, base_out)
        v_cand = compute_visqol(ref_file, cand_out)

        if v_base is not None and v_cand is not None:
            metric_name = "ViSQOL MOS"
            base_val = v_base
            cand_val = v_cand
            delta = cand_val - base_val
            passed = delta >= -0.05
        else:
            metric_name = "SNR (dB)"
            base_val = compute_snr(ref_file, base_out)
            cand_val = compute_snr(ref_file, cand_out)
            delta = cand_val - base_val
            passed = delta >= -0.5

        if not passed:
            all_passed = False

        status_str = "PASS" if passed else "FAIL"
        print(f"{name:<25} | {metric_name:<10} | {base_val:<10.2f} | {cand_val:<10.2f} | {delta:<+8.2f} | {status_str}")

        results.append({
            "name": name,
            "mod": mod,
            "metric": metric_name,
            "base": base_val,
            "cand": cand_val,
            "delta": delta,
            "pass": passed
        })

    print("-" * 92)
    print("Quality Evaluation Summary: " + ("ALL TESTS PASSED" if all_passed else "QUALITY REGRESSION DETECTED"))

    write_github_summary(results, args.baseline_so, args.candidate_so)
    sys.exit(0 if all_passed else 1)

if __name__ == "__main__":
    main()
