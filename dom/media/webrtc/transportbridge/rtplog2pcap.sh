#!/bin/bash
# Convert a Firefox RtpLogger text log to a pcap file.
# Each distinct PeerConnection UUID becomes a distinct IP flow.
# Pairs of UUIDs (in sorted order) share the same IP pair with src/dst swapped.
#
# Usage: rtplog2pcap.sh <input.txt> <output.pcap>
#
# To produce the input log, set the environment variable:
#   MOZ_LOG=RtpLogger:5
# then capture stderr to a file while running Firefox.

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <input.txt> <output.pcap>" >&2
  exit 1
fi

input="$1"
output="$2"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

n=1
while IFS= read -r uuid; do
  if (( n % 2 == 1 )); then
    src="10.0.0.$n"
    dst="10.0.0.$((n+1))"
  else
    src="10.0.0.$n"
    dst="10.0.0.$((n-1))"
  fi
  rg -F "$uuid" "$input" \
    | rg 'RTP_PACKET|RTCP_PACKET' \
    | sed 's/.*|>> //' \
    | text2pcap -D -n -l 1 -i 17 -4 "$src,$dst" -u 1234,1235 \
        -t '%H:%M:%S.%f' - "$tmp/$n.pcap" 2>/dev/null \
    || true
  ((n++))
done < <(rg -oP '(?<=RtpLogger )\{[^}]+\}' "$input" | sort -u)

shopt -s nullglob
pcaps=("$tmp"/*.pcap)
if [[ ${#pcaps[@]} -eq 0 ]]; then
  echo "No packets found in '$input'" >&2
  exit 1
fi

mergecap -w "$output" "${pcaps[@]}"
