# Evaluating the noise suppressor: setup and rules

How the quality numbers in `docs/modern_replacement_architecture.md` §4.5 were produced, so they can be reproduced. This is a
description of the method; the proposed CI gate is a separate draft.

## Metric rules
- **Trusted metric: Zimtohrli, full length, delay-compensated.** Zimtohrli does no alignment of its own: a clean clip delayed by 96
  samples drops from 4.96 to 4.34, and the effect is not monotonic. Advance each processed output by its known delay before scoring
  (96 samples at 16 kHz, 48 at 8 kHz, for both this library and the WebRTC oracle) and log the cross-correlation lag between input and
  output as a check; a file whose measured lag disagrees with the known delay by more than 2 samples is suspect.
- `compute_mos` in `scripts/eval_quality_tcd.py` truncates Zimtohrli to 3.5 s and does not compensate delay; do not use it for
  Zimtohrli comparisons. ViSQOL aligns internally and is reported alongside as a second opinion. Never compute a delta across the
  two metrics (their scales differ).
- Score `delta = candidate - oracle` per file; a file passes at `delta >= -0.05`. Report per noise type and overall, and report the
  unprocessed ("raw") score as the zero line.
- Write the pass rule down before scoring a candidate, and do not choose constants on data that is also used to report the result.

## Data and splits
- Dataset: TCD-VoIP (about 265 MB). `scripts/eval_quality_tcd.py` (`ensure_dataset`, `load_test_battery`, `extract_files`) downloads and
  unpacks it. The 76 noise files are babble 16 and car, office, road 20 each, at noise gains 5 to 55.
- The 8 CI gate clips are the first two files of each type (all at gains 45 to 55, the mildest conditions). The other 68 files are split
  by speaker suffix: development = `FA`, `MK`; test = `FG`, `ML`. The test half has been scored once and is spent as a holdout.
- 8 kHz: resample input and reference to 8 kHz, process at 8 kHz, then compare at 8 kHz (Zimtohrli) or upsample both to 16 kHz (ViSQOL).
- The TCD clips carry almost nothing above 8 kHz (the 8-16 kHz band is 13-48 dB down), so they cannot evaluate 32/48 kHz behaviour.

## Building the oracle (the WebRTC NS) on a host
No MIPS cross compiler is needed; leave out the MIPS assembly sources.
```
git worktree add ../oracle 1f0a840          # the WebRTC NS; use 475278e for exactly what Thingino ships
cd ../oracle
SRCS=$(make -n CROSS_COMPILE= | grep -o ' [^ ]*\.c$' | grep -v _mips | sort -u | tr '\n' ' ')
gcc -std=c11 -D_GNU_SOURCE -O2 -fPIC -DWEBRTC_POSIX -Isrc -shared \
    -Wl,--version-script=src/libaudioProcess.map -Wl,--no-as-needed $SRCS -lm -lpthread -o libbase.so
```
Build the candidate from the working tree with the same flags minus `-DWEBRTC_POSIX` and the WebRTC sources.

## Running a library over a file
`tests/process_wav.c` loads a library with `dlopen`, runs the selected module over a WAV file in 10 ms blocks, and writes the result. It
hardcodes NS policy 2; for other policies use a scratch copy that reads the policy from an environment variable. The shipping default
when NS is enabled is policy 1.

## Python environment
Needs `zimtohrli`, `visqol` (speech mode), `soundfile`, `scipy`, `numpy`, `pandas` and `openpyxl` (the dataset spreadsheet). Scoring:
resample both signals to 48 kHz with `scipy.signal.resample_poly` and call `zimtohrli.mos_from_signals(reference, degraded)`.

## Regression tests that guard the behaviour
`make test` runs synthetic-signal guards that need no dataset: oracle attenuation in steady state (frames 200 to 259 of white noise),
speech transparency in light noise at 16 and 8 kHz, speech after digital silence, scale independence, in-place equality, re-convergence
after a 20 dB noise rise, an int16-range output, state reset, and the accepted rates and policies. Each was checked to fail on an earlier
build that had the defect it guards against.
