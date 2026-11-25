#!/usr/bin/env bash
# Download a Nasdaq TotalView-ITCH 5.0 sample day into data/ and verify it.
#
# Usage: tools/download_itch.sh [FILENAME]
#   FILENAME defaults to 01302019.NASDAQ_ITCH50.gz (Nasdaq's free sample for
#   30 January 2019).
#
# - Resumable: the payload is written to FILENAME.part and curl continues from
#   wherever that partial file ends (`-C -`), so re-running after an
#   interruption picks up where it stopped instead of starting over.
# - Verified: the official .md5sum file is fetched and the MD5 of the
#   downloaded bytes must match it before FILENAME.part is renamed to FILENAME.
#   You can also pass the hash yourself with ITCH_MD5=<hash>.
#   Fallback: Nasdaq's IIS server lists the .md5sum files but answers 404 when
#   they are requested (observed 2026-11-25). If no MD5 is available the script
#   instead checks (a) the byte count equals the server's Content-Length and
#   (b) `gzip -t` passes, which checks the CRC32 and length stored in every
#   gzip member's trailer, i.e. every decompressed byte is checked. It prints
#   the MD5 it computed so it can be recorded and compared later.
# - Idempotent: if FILENAME already exists and verifies, nothing is
#   downloaded.
set -euo pipefail

BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
FILE="${1:-01302019.NASDAQ_ITCH50.gz}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
data_dir="$repo_root/data"
mkdir -p "$data_dir"

target="$data_dir/$FILE"
part="$target.part"
sumfile="$target.md5sum"

# Portable MD5: GNU md5sum prints "hash  file"; BSD md5 -q prints just the hash.
md5_of() {
    if command -v md5sum >/dev/null 2>&1; then
        md5sum "$1" | awk '{print $1}'
    else
        md5 -q "$1"
    fi
}

expected="${ITCH_MD5:-}"
if [[ -z "$expected" ]]; then
    echo "Fetching checksum: $FILE.md5sum"
    if curl --fail --silent --show-error --location --retry 3 \
            --output "$sumfile" "$BASE_URL/$FILE.md5sum"; then
        expected="$(awk '{print tolower($1)}' "$sumfile")"
    else
        rm -f "$sumfile"
        echo "warning: could not fetch $FILE.md5sum; will verify with gzip CRC32 + size instead." >&2
    fi
fi
expected="$(printf '%s' "$expected" | tr 'A-F' 'a-f')"
if [[ -n "$expected" && ! "$expected" =~ ^[0-9a-f]{32}$ ]]; then
    echo "error: '$expected' is not an MD5 hash" >&2
    exit 1
fi
echo "Expected MD5: ${expected:-<unavailable>}"

remote_size="$(curl --fail --silent --show-error --location --head "$BASE_URL/$FILE" \
               | tr -d '\r' | awk 'tolower($1)=="content-length:"{print $2}' | tail -1)"
echo "Remote size: ${remote_size:-unknown} bytes"

# verify FILE -> 0 if it passes every check we can do.
verify() {
    local f="$1"
    if [[ -n "$remote_size" ]]; then
        local sz; sz="$(wc -c < "$f" | tr -d ' ')"
        if [[ "$sz" != "$remote_size" ]]; then
            echo "size mismatch: have $sz, server says $remote_size" >&2
            return 1
        fi
    fi
    if [[ -n "$expected" ]]; then
        local got; got="$(md5_of "$f")"
        echo "MD5: $got"
        [[ "$got" == "$expected" ]] || { echo "MD5 mismatch" >&2; return 1; }
    else
        echo "No MD5 available: running gzip -t (CRC32 of all decompressed data)..."
        gzip -t "$f" || { echo "gzip integrity test failed" >&2; return 1; }
        echo "MD5 (computed, unverified against Nasdaq): $(md5_of "$f")"
    fi
}

if [[ -f "$target" ]]; then
    echo "Found existing $target, verifying..."
    if verify "$target"; then
        echo "OK: already downloaded and verified."
        exit 0
    fi
    echo "Existing file failed verification; moving it aside and re-downloading." >&2
    mv "$target" "$target.bad.$(date +%s)"
fi


# Retry loop around curl's resume so a dropped connection is retried with
# resume from the current .part size, not from zero.
attempt=0
max_attempts=20
while :; do
    attempt=$((attempt + 1))
    have=0
    [[ -f "$part" ]] && have="$(wc -c < "$part" | tr -d ' ')"
    if [[ -n "$remote_size" && "$have" -eq "$remote_size" ]]; then
        break
    fi
    echo "Attempt $attempt: resuming at byte $have"
    if curl --fail --location --retry 5 --retry-delay 5 \
            --speed-limit 1024 --speed-time 60 \
            -C - --output "$part" "$BASE_URL/$FILE"; then
        break
    fi
    if (( attempt >= max_attempts )); then
        echo "error: download failed after $attempt attempts; re-run to resume." >&2
        exit 1
    fi
    sleep 5
done

echo "Verifying download..."
if ! verify "$part"; then
    echo "error: verification failed. The partial file is kept at $part; delete it to start fresh." >&2
    exit 1
fi

mv "$part" "$target"
echo "OK: $target verified."
