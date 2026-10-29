#!/usr/bin/env bash
# Fetches and verifies the CEF binary distribution pinned in native/cef.lock.json.
# Usage: ./Scripts/FetchCef.sh [platform] [--force]
# Prints the resolved CEF root directory as the final output line.

set -euo pipefail

fail() {
	echo "FetchCef: $*" >&2
	exit 1
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
lock_path="$repo_root/native/cef.lock.json"
[ -f "$lock_path" ] || fail "lock file not found: $lock_path"

force=0
platform=""
for arg in "$@"; do
	case "$arg" in
		--force) force=1 ;;
		-*) fail "unknown option '$arg'" ;;
		*) platform="$arg" ;;
	esac
done

if [ -z "$platform" ]; then
	os="$(uname -s)"
	machine="$(uname -m)"
	case "$os/$machine" in
		Darwin/arm64) platform="macosarm64" ;;
		Darwin/x86_64) platform="macosx64" ;;
		Linux/x86_64) platform="linux64" ;;
		*) fail "unsupported host '$os/$machine'; pass a platform argument" ;;
	esac
fi

# Query a single field of the pinned entry: field <platform> <name>
field() {
	python3 - "$lock_path" "$1" "$2" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1]))
entry = lock["files"].get(sys.argv[2])
if entry is None:
    sys.exit(1)
value = lock["version"] if sys.argv[3] == "version" else entry[sys.argv[3]]
print(value)
PY
}

version="$(field "$platform" version)" || fail "platform '$platform' is not pinned in $lock_path"
name="$(field "$platform" name)"
sha1="$(field "$platform" sha1)"
base_url="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["baseUrl"])' "$lock_path")"

cef_root="${CEF_ROOT:-$repo_root/native/cef}"
target_dir="$cef_root/$version/$platform"
marker_path="$target_dir/.cef-sha1"
expected_marker="$sha1  $name"

if [ "$force" -eq 0 ] && [ -f "$marker_path" ] && [ -d "$target_dir/include" ]; then
	if [ "$(cat "$marker_path")" = "$expected_marker" ]; then
		echo "CEF $version ($platform) already present, skipping download."
		echo "$target_dir"
		exit 0
	fi
fi

hash_file() {
	if command -v sha1sum >/dev/null 2>&1; then
		sha1sum "$1" | awk '{print $1}'
	else
		shasum -a 1 "$1" | awk '{print $1}'
	fi
}

cache_dir="${TMPDIR:-/tmp}/uxwebview-cef"
mkdir -p "$cache_dir"
archive_path="$cache_dir/$name"
archive_valid=0

if [ "$force" -eq 0 ] && [ -f "$archive_path" ]; then
	if [ "$(hash_file "$archive_path")" = "$sha1" ]; then
		archive_valid=1
	else
		echo "Cached archive hash mismatch, discarding it."
		rm -f "$archive_path"
	fi
fi

if [ "$archive_valid" -eq 0 ]; then
	echo "Downloading $base_url$name"
	if command -v curl >/dev/null 2>&1; then
		curl -fL --retry 3 -o "$archive_path" "$base_url$name" || fail "download failed"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$archive_path" "$base_url$name" || fail "download failed"
	else
		fail "curl or wget is required"
	fi
fi

actual_sha1="$(hash_file "$archive_path")"
if [ "$actual_sha1" != "$sha1" ]; then
	rm -f "$archive_path"
	fail "SHA1 mismatch for $name: expected $sha1, got $actual_sha1"
fi

staging_dir="$cache_dir/extract-$platform"
rm -rf "$staging_dir"
mkdir -p "$staging_dir"

echo "Extracting $name"
tar -xjf "$archive_path" --strip-components=1 -C "$staging_dir" || {
	rm -rf "$staging_dir"
	fail "extraction failed"
}
[ -d "$staging_dir/include" ] || {
	rm -rf "$staging_dir"
	fail "unexpected archive layout: include/ is missing after extraction"
}

rm -rf "$target_dir"
mkdir -p "$(dirname "$target_dir")"
mv "$staging_dir" "$target_dir"
printf '%s' "$expected_marker" > "$marker_path"
rm -f "$archive_path"

echo "CEF $version ($platform) ready at $target_dir"
echo "$target_dir"
