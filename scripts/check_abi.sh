#!/bin/sh
# Fail if libaudioProcess.so lacks any symbol that libimp resolves with dlsym().
# usage: scripts/check_abi.sh <libaudioProcess.so> [nm-tool]
so="${1:?usage: check_abi.sh <libaudioProcess.so> [nm-tool]}"
nm_tool="${2:-nm}"
required="
audio_process_aec_create audio_process_aec_process audio_process_aec_free
audio_process_agc_create audio_process_agc_process audio_process_agc_free
audio_process_agc_set_config audio_process_agc_get_config
audio_process_ns_create audio_process_ns_process audio_process_ns_free
audio_process_ns_set_config audio_process_ns_get_config
audio_process_hpf_create audio_process_hpf_process audio_process_hpf_free
audio_process_hs_create audio_process_hs_process audio_process_hs_free
"
exports=$("$nm_tool" -D --defined-only "$so") || exit 2
missing=0; count=0
for sym in $required; do
	count=$((count + 1))
	if ! printf '%s\n' "$exports" | grep -q " [TtWw] $sym\$"; then
		echo "ABI: required symbol missing: $sym" >&2
		missing=1
	fi
done
[ "$missing" -eq 0 ] && echo "ABI: all $count required symbols present in $so"
exit "$missing"
