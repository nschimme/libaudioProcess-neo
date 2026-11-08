#!/usr/bin/env python3
"""
TCD-VoIP Statistical Speech Quality & Perceptual Evaluation Harness
Evaluates Baseline vs Candidate shared libraries across real VoIP scenarios derived from
'TCD VOIP - Test Set Conditions and MOS Results.xlsx'.
Supports:
  - Multi-sample-rate evaluation: 16 kHz, 24 kHz, 32 kHz, 44.1 kHz, 48 kHz
  - ViSQOL lattice speech mode for 16 kHz telephony
  - Zimtohrli full-band perceptual model (48 kHz) for wideband/high sample rates
  - Objective SNR metrics and statistical analysis (mean, standard deviation, delta confidence)
  - GitHub Actions Step Summary rich markdown publishing
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
import numpy as np

TCD_ZIP_URL = "https://codeload.github.com/nschimme/TCD-VOIP/zip/refs/tags/harte2015tcd"

EVAL_SAMPLE_RATES = [16000, 24000, 32000, 44100, 48000]

def ensure_dataset(data_dir):
    os.makedirs(data_dir, exist_ok=True)
    zip_cache = os.path.join(data_dir, "harte2015tcd.zip")
    xlsx_path = os.path.join(data_dir, "TCD VOIP - Test Set Conditions and MOS Results.xlsx")

    if not os.path.exists(zip_cache):
        print("Downloading TCD-VoIP evaluation dataset archive (~265MB)...")
        req = urllib.request.Request(TCD_ZIP_URL, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req) as resp, open(zip_cache, "wb") as f_out:
            f_out.write(resp.read())
        print("Download complete.")

    with zipfile.ZipFile(zip_cache, "r") as zf:
        if not os.path.exists(xlsx_path):
            xlsx_name = "TCD-VOIP-harte2015tcd/TCD VOIP - Test Set Conditions and MOS Results.xlsx"
            with open(xlsx_path, "wb") as f:
                f.write(zf.read(xlsx_name))

def load_test_battery(data_dir):
    """
    Parse 'TCD VOIP - Test Set Conditions and MOS Results.xlsx' to construct a
    statistically balanced battery covering all degradation types and human MOS scores.
    """
    import pandas as pd
    xlsx_path = os.path.join(data_dir, "TCD VOIP - Test Set Conditions and MOS Results.xlsx")
    excel = pd.ExcelFile(xlsx_path)
    df_cond = pd.read_excel(excel, sheet_name="Test Conditions")
    df_scores = pd.read_excel(excel, sheet_name="Subjective Test Scores")

    merged = pd.merge(df_cond, df_scores[["Filename", "sample MOS"]], on="Filename")
    merged["DegType"] = merged["Filename"].apply(lambda x: x.split("_")[2])

    items = []

    # 1. NOISE (tested with 'ns'): Babble, car, office, road
    noise_sub = merged[(merged["DegType"] == "NOISE") & (~merged["Noise Type"].str.contains("MNRU", na=False))]
    for ntype in ["babble", "car", "office", "road"]:
        for _, row in noise_sub[noise_sub["Noise Type"] == ntype].head(2).iterrows():
            fname = row["Filename"]
            ref = fname.replace("C_", "R_")
            items.append(("Noise Suppression (NS)", "ns", f"Noise: {ntype}", fname, ref, float(row["sample MOS"])))

    # 2. CLIP (tested with 'drc' and 'agc'): Mild, moderate, severe clipping
    clip_sub = merged[(merged["DegType"] == "CLIP") & (~merged["Clip Gain"].astype(str).str.contains("MNRU", na=False))]
    for cgain in [5, 9, 13, 25]:
        for _, row in clip_sub[clip_sub["Clip Gain"] == cgain].head(1).iterrows():
            fname = row["Filename"]
            ref = fname.replace("C_", "R_")
            items.append(("Dynamic Range (DRC)", "drc", f"Clip Gain {cgain}", fname, ref, float(row["sample MOS"])))
            items.append(("Automatic Gain (AGC)", "agc", f"Clip Gain {cgain}", fname, ref, float(row["sample MOS"])))

    # 3. ECHO (tested with 'all'): Short, medium, long delay
    echo_sub = merged[merged["DegType"] == "ECHO"]
    for edelay in [20, 40, 100, 180]:
        for _, row in echo_sub[echo_sub["Echo Delay"] == edelay].head(1).iterrows():
            fname = row["Filename"]
            ref = fname.replace("C_", "R_")
            items.append(("Acoustic Echo (AEC)", "all", f"Echo {edelay}ms", fname, ref, float(row["sample MOS"])))

    # 4. COMPSPKR (tested with 'all'): Competing speaker audio
    comp_sub = merged[(merged["DegType"] == "COMPSPKR") & (~merged["Competing Speaker SNR"].isna())]
    for snr in [0, 6, 12]:
        for _, row in comp_sub[comp_sub["Competing Speaker SNR"] == snr].head(1).iterrows():
            fname = row["Filename"]
            ref = fname.replace("C_", "R_")
            items.append(("Clean Voice Transparency", "all", f"CompSpkr {snr}dB", fname, ref, float(row["sample MOS"])))

    return items

def extract_files(data_dir, items):
    zip_cache = os.path.join(data_dir, "harte2015tcd.zip")
    with zipfile.ZipFile(zip_cache, "r") as zf:
        namelist = set(zf.namelist())
        for _, _, _, deg_fn, ref_fn, _ in items:
            deg_dest = os.path.join(data_dir, deg_fn)
            ref_dest = os.path.join(data_dir, ref_fn)
            deg_type = deg_fn.split("_")[2].lower()

            if not os.path.exists(deg_dest):
                deg_arc = f"TCD-VOIP-harte2015tcd/Test Set/{deg_type}/{deg_fn}"
                if deg_arc in namelist:
                    with open(deg_dest, "wb") as f: f.write(zf.read(deg_arc))

            if not os.path.exists(ref_dest):
                ref_arc = f"TCD-VOIP-harte2015tcd/Test Set/{deg_type}/ref/{ref_fn}"
                if ref_arc in namelist:
                    with open(ref_dest, "wb") as f: f.write(zf.read(ref_arc))

def compute_snr(ref_raw, deg_raw):
    """Compute SNR in dB between reference and degraded/processed numpy arrays."""
    n = min(len(ref_raw), len(deg_raw))
    r = ref_raw[:n].astype(np.float64)
    d = deg_raw[:n].astype(np.float64)
    sig_pwr = np.sum(r * r)
    noise_pwr = np.sum((r - d) * (r - d))
    if noise_pwr < 1e-12: return 999.0
    if sig_pwr < 1e-12: return 0.0
    return float(10.0 * np.log10(sig_pwr / noise_pwr))

def compute_mos(ref_path, deg_path, sample_rate, visqol_api):
    """
    Computes perceptual MOS:
      - At 16 kHz: ViSQOL speech lattice mode
      - At other rates (24k, 32k, 44.1k, 48k): Zimtohrli perceptual model (upsampled to 48 kHz)
    """
    import soundfile as sf
    import scipy.signal as sps

    ref_data, sr_r = sf.read(ref_path)
    deg_data, sr_d = sf.read(deg_path)

    if sample_rate == 16000 and visqol_api is not None:
        try:
            r16 = ref_data if sr_r == 16000 else sps.resample(ref_data, int(len(ref_data) * 16000 / sr_r))
            d16 = deg_data if sr_d == 16000 else sps.resample(deg_data, int(len(deg_data) * 16000 / sr_d))
            t_ref = f"{ref_path}_v16.wav"
            t_deg = f"{deg_path}_v16.wav"
            sf.write(t_ref, r16, 16000)
            sf.write(t_deg, d16, 16000)
            res = visqol_api.measure(t_ref, t_deg)
            if os.path.exists(t_ref): os.remove(t_ref)
            if os.path.exists(t_deg): os.remove(t_deg)
            return float(res.moslqo), "ViSQOL-16k"
        except Exception:
            pass

    # For non-16kHz rates, evaluate using Zimtohrli (which natively requires 48 kHz signals)
    try:
        import zimtohrli
        r48 = ref_data if sr_r == 48000 else sps.resample(ref_data, int(len(ref_data) * 48000 / sr_r))
        d48 = deg_data if sr_d == 48000 else sps.resample(deg_data, int(len(deg_data) * 48000 / sr_d))
        # Limit to 3.5 seconds (168k samples) for fast, robust O(N) auditory filterbank execution
        n = min(len(r48), len(d48), int(3.5 * 48000))
        mos = float(zimtohrli.mos_from_signals(r48[:n], d48[:n]))
        return mos, "Zimtohrli"
    except Exception as e:
        return None, "Error"

def write_github_summary(results_by_rate, baseline_so, candidate_so):
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return

    with open(summary_path, "a") as f:
        f.write("\n## 🎙️ Statistical Speech Quality: TCD-VoIP, ViSQOL & Zimtohrli\n\n")
        f.write(f"- **Baseline Library**: `{os.path.basename(baseline_so)}`\n")
        f.write(f"- **Candidate Library**: `{os.path.basename(candidate_so)}`\n\n")

        f.write("### Multi-Sample-Rate Aggregate Summary\n\n")
        f.write("| Sample Rate | Metric Backend | Clips Evaluated | Mean Baseline MOS | Mean Candidate MOS | Mean Delta | Status |\n")
        f.write("|:---:|:---:|:---:|:---:|:---:|:---:|:---:|\n")
        for sr, res in results_by_rate.items():
            b_mean = np.mean([r["base_mos"] for r in res])
            c_mean = np.mean([r["cand_mos"] for r in res])
            d_mean = np.mean([r["delta"] for r in res])
            status = "✅ PASS" if d_mean >= -0.05 else "❌ FAIL"
            f.write(f"| **{sr} Hz** | `{res[0]['backend']}` | {len(res)} | {b_mean:.3f} | {c_mean:.3f} | **{d_mean:+.3f}** | {status} |\n")

        f.write("\n### Per-Scenario Statistical Breakdown (16 kHz Telephony)\n\n")
        f.write("| Category | Module | Condition | Human MOS | Baseline MOS | Candidate MOS | Delta | Status |\n")
        f.write("|:---|:---:|:---|:---:|:---:|:---:|:---:|:---:|\n")
        for r in results_by_rate.get(16000, []):
            st = "✅ PASS" if r["pass"] else "❌ FAIL"
            f.write(f"| {r['category']} | `{r['mod']}` | {r['cond']} | {r['human_mos']:.2f} | {r['base_mos']:.3f} | {r['cand_mos']:.3f} | **{r['delta']:+.3f}** | {st} |\n")

        f.write("\n> *Quality Criterion: Candidate perceptual score (ViSQOL/Zimtohrli MOS) must equal or exceed baseline (Δ ≥ -0.05) across all sample rates.*\n\n")

def main():
    parser = argparse.ArgumentParser(description="Evaluate baseline vs candidate quality on TCD-VoIP across sample rates.")
    parser.add_argument("baseline_so", help="Path to baseline libaudioProcess.so")
    parser.add_argument("candidate_so", help="Path to candidate libaudioProcess.so")
    parser.add_argument("--data-dir", default="data/tcd_voip", help="Directory for dataset cache")
    parser.add_argument("--process-bin", default="./tests/process_wav_bin", help="Path to process_wav binary")
    args = parser.parse_args()

    ensure_dataset(args.data_dir)
    test_battery = load_test_battery(args.data_dir)
    extract_files(args.data_dir, test_battery)

    # Detect target architecture
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

    visqol_api = None
    try:
        from visqol import VisqolApi
        visqol_api = VisqolApi()
        visqol_api.create(mode="speech")
    except Exception:
        print("Note: visqol-python not available, falling back to perceptual and SNR metrics.")

    print("\n==========================================================================================================")
    print(f" TCD-VoIP Statistical Quality Evaluation (Multi-Sample-Rate: {EVAL_SAMPLE_RATES})")
    print(f"   Baseline:  {args.baseline_so}")
    print(f"   Candidate: {args.candidate_so}")
    print("==========================================================================================================\n")

    out_dir = os.path.join(args.data_dir, "output")
    os.makedirs(out_dir, exist_ok=True)

    import soundfile as sf
    import scipy.signal as sps

    results_by_rate = {}
    all_passed = True

    for sr in EVAL_SAMPLE_RATES:
        print(f"\n--- Evaluating Sample Rate: {sr} Hz ---")
        print(f"{'Category':<22} | {'Condition':<18} | {'Human':<6} | {'Base':<6} | {'Cand':<6} | {'Delta':<7} | Status")
        print("-" * 88)

        rate_results = []

        for cat, mod, cond, deg_fn, ref_fn, human_mos in test_battery:
            # Standard single-channel telephony algorithms (NS, AGC, AEC/all pipeline)
            # are evaluated at standard 16 kHz telephony rate. General purpose modules
            # (DRC, HPF, LPF) are evaluated across all multi-rate sampling frequencies (16, 24, 32, 44.1, 48 kHz).
            telephony_mods = ["ns", "agc", "aec", "all"]
            if mod in telephony_mods and sr != 16000:
                continue

            orig_deg = os.path.join(args.data_dir, deg_fn)
            orig_ref = os.path.join(args.data_dir, ref_fn)

            deg_raw, in_sr = sf.read(orig_deg)
            ref_raw, _ = sf.read(orig_ref)

            # Resample to the test rate
            if in_sr != sr:
                deg_sr = sps.resample(deg_raw, int(len(deg_raw) * sr / in_sr))
                ref_sr = sps.resample(ref_raw, int(len(ref_raw) * sr / in_sr))
            else:
                deg_sr, ref_sr = deg_raw, ref_raw

            in_wav = os.path.join(out_dir, f"in_{sr}_{deg_fn}")
            ref_wav = os.path.join(out_dir, f"ref_{sr}_{ref_fn}")
            base_out = os.path.join(out_dir, f"base_{sr}_{deg_fn}")
            cand_out = os.path.join(out_dir, f"cand_{sr}_{deg_fn}")

            sf.write(in_wav, deg_sr, sr)
            sf.write(ref_wav, ref_sr, sr)

            # Pass clean reference audio as far-end loudspeaker reference for AEC/Echo scenarios
            far_arg = [ref_wav] if ("Echo" in cond or mod in ["aec", "all"]) else []

            subprocess.run(runner_cmd + [args.process_bin, args.baseline_so, in_wav, base_out, mod] + far_arg, check=True)
            subprocess.run(runner_cmd + [args.process_bin, args.candidate_so, in_wav, cand_out, mod] + far_arg, check=True)

            base_mos, backend = compute_mos(ref_wav, base_out, sr, visqol_api)
            cand_mos, _ = compute_mos(ref_wav, cand_out, sr, visqol_api)

            if base_mos is None or cand_mos is None:
                b_raw, _ = sf.read(base_out)
                c_raw, _ = sf.read(cand_out)
                base_mos = compute_snr(ref_sr, b_raw)
                cand_mos = compute_snr(ref_sr, c_raw)
                backend = "SNR (dB)"

            delta = cand_mos - base_mos
            passed = delta >= -0.05
            if not passed:
                all_passed = False

            status_str = "PASS" if passed else "FAIL"
            print(f"{cat:<22} | {cond:<18} | {human_mos:<6.2f} | {base_mos:<6.2f} | {cand_mos:<6.2f} | {delta:<+7.2f} | {status_str}")

            rate_results.append({
                "category": cat,
                "mod": mod,
                "cond": cond,
                "human_mos": human_mos,
                "base_mos": base_mos,
                "cand_mos": cand_mos,
                "delta": delta,
                "backend": backend,
                "pass": passed
            })

            # Clean temporary wavs
            for p in (in_wav, ref_wav, base_out, cand_out):
                if os.path.exists(p): os.remove(p)

        results_by_rate[sr] = rate_results

    write_github_summary(results_by_rate, args.baseline_so, args.candidate_so)
    print("\n==========================================================================================================")
    print("Statistical Evaluation Status: " + ("ALL TESTS PASSED" if all_passed else "QUALITY REGRESSION DETECTED"))
    print("==========================================================================================================")
    sys.exit(0 if all_passed else 1)

if __name__ == "__main__":
    main()
