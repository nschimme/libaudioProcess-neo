#!/usr/bin/env python3
"""
TCD-VoIP & Objective Quality Evaluation Harness
Evaluates Baseline vs Candidate shared libraries using TCD-VoIP speech datasets.
Calculates ViSQOL MOS-LQO (if visqol-python is installed) and SNR / RMS improvements.
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

# Curated evaluation pairs from TCD-VoIP: (condition_name, clean_ref, degraded_file)
EVAL_PAIRS = [
    ("Noise Suppression", "TCD-VOIP-harte2015tcd/ref/R_01_CLIP_MK.wav", "TCD-VOIP-harte2015tcd/Test Set/noise/C_10_NOISE_MK.wav"),
    ("Echo Cancellation", "TCD-VOIP-harte2015tcd/ref/R_01_ECHO_FG.wav", "TCD-VOIP-harte2015tcd/Test Set/echo/C_15_ECHO_FG.wav"),
    ("Clip Limiting",     "TCD-VOIP-harte2015tcd/ref/R_01_CLIP_FA.wav", "TCD-VOIP-harte2015tcd/Test Set/clip/C_01_CLIP_FA.wav"),
    ("Clean Speech Trans", "TCD-VOIP-harte2015tcd/ref/R_01_COMPSPKR_FA.wav", "TCD-VOIP-harte2015tcd/ref/R_01_COMPSPKR_FA.wav"),
]

def ensure_dataset(data_dir):
    os.makedirs(data_dir, exist_ok=True)
    needed = []
    for _, ref_p, deg_p in EVAL_PAIRS:
        if not os.path.exists(os.path.join(data_dir, os.path.basename(ref_p))):
            needed.append(ref_p)
        if not os.path.exists(os.path.join(data_dir, os.path.basename(deg_p))):
            needed.append(deg_p)

    if not needed:
        return

    print("Downloading TCD-VoIP curated evaluation dataset...")
    req = urllib.request.Request(TCD_ZIP_URL, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as resp:
        zf = zipfile.ZipFile(io.BytesIO(resp.read()))
        for _, ref_p, deg_p in EVAL_PAIRS:
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
    try:
        from visqol import VisqolApi
        api = VisqolApi()
        api.create(mode="speech")
        res = api.measure(ref_wav, deg_wav)
        return float(res.moslqo)
    except Exception:
        return None

def main():
    parser = argparse.ArgumentParser(description="Evaluate baseline vs candidate quality on TCD-VoIP dataset.")
    parser.add_argument("baseline_so", help="Path to baseline libaudioProcess.so")
    parser.add_argument("candidate_so", help="Path to candidate libaudioProcess.so")
    parser.add_argument("--data-dir", default="data/tcd_voip", help="Directory for dataset cache")
    parser.add_argument("--process-bin", default="./tests/process_wav_bin", help="Path to process_wav binary")
    args = parser.parse_args()

    ensure_dataset(args.data_dir)

    # Check process_wav binary
    if not os.path.exists(args.process_bin):
        print(f"Compiling {args.process_bin}...")
        res = subprocess.run(["gcc", "-std=c11", "-D_GNU_SOURCE", "-Isrc", "-O2", "tests/process_wav.c", "-ldl", "-lm", "-o", args.process_bin])
        if res.returncode != 0:
            print("Failed to compile process_wav tool.")
            sys.exit(1)

    print("\n=========================================================================================")
    print(f" TCD-VoIP Quality Evaluation: Baseline vs. Candidate")
    print(f"   Baseline:  {args.baseline_so}")
    print(f"   Candidate: {args.candidate_so}")
    print("=========================================================================================\n")

    print(f"{'Condition':<20} | {'Metric':<10} | {'Baseline':<10} | {'Candidate':<10} | {'Delta':<8} | Status")
    print("-" * 89)

    out_dir = os.path.join(args.data_dir, "output")
    os.makedirs(out_dir, exist_ok=True)

    all_passed = True
    for label, ref_p, deg_p in EVAL_PAIRS:
        ref_file = os.path.join(args.data_dir, os.path.basename(ref_p))
        deg_file = os.path.join(args.data_dir, os.path.basename(deg_p))

        base_out = os.path.join(out_dir, f"{os.path.basename(deg_p)}_base.wav")
        cand_out = os.path.join(out_dir, f"{os.path.basename(deg_p)}_cand.wav")

        # Process through baseline
        subprocess.run([args.process_bin, args.baseline_so, deg_file, base_out], check=True)
        # Process through candidate
        subprocess.run([args.process_bin, args.candidate_so, deg_file, cand_out], check=True)

        # 1. ViSQOL MOS if available
        v_base = compute_visqol(ref_file, base_out)
        v_cand = compute_visqol(ref_file, cand_out)

        if v_base is not None and v_cand is not None:
            delta = v_cand - v_base
            status = "PASS" if delta >= -0.05 else "FAIL"
            if status == "FAIL": all_passed = False
            print(f"{label:<20} | {'ViSQOL MOS':<10} | {v_base:<10.2f} | {v_cand:<10.2f} | {delta:<+8.2f} | {status}")
        else:
            # Fallback to Objective SNR
            snr_base = compute_snr(ref_file, base_out)
            snr_cand = compute_snr(ref_file, cand_out)
            delta = snr_cand - snr_base
            status = "PASS" if delta >= -0.5 else "FAIL"
            if status == "FAIL": all_passed = False
            print(f"{label:<20} | {'SNR (dB)':<10} | {snr_base:<10.1f} | {snr_cand:<10.1f} | {delta:<+8.1f} | {status}")

    print("\nQuality Evaluation Summary: " + ("ALL TESTS PASSED" if all_passed else "QUALITY REGRESSION DETECTED"))
    sys.exit(0 if all_passed else 1)

if __name__ == "__main__":
    main()
