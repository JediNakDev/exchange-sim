#!/bin/sh
# Usage: research_digests.sh <exsim_research binary> <output file>
#
# Runs every config/research/*.conf over seeds 1..3 and writes one SHA-256
# line per artifact, sorted by path. The research runner promises
# byte-identical artifacts for the same config and seed on any platform and
# compiler, so CI requires these files to be equal across its matrix.
# status.json (wall clock, process id) and manifest.json (records the
# compiler) are the only artifacts that legitimately differ and are skipped.
set -eu
binary=$1
output=$2
work=$(mktemp -d)
if command -v sha256sum > /dev/null 2>&1; then hash_file="sha256sum"; else hash_file="shasum -a 256"; fi
for config in config/research/*.conf; do
    name=$(basename "$config" .conf)
    "$binary" run "$config" --out "$work/$name" --seeds 1..3 --quiet
done
(cd "$work" && find . -type f ! -name status.json ! -name manifest.json | LC_ALL=C sort |
    while read -r file; do $hash_file "$file"; done) > "$output"
echo "$(wc -l < "$output") artifact digests written to $output"
