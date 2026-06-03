#!/usr/bin/env bash

COB_PATH=/usr/bin/cob
NINJA_PATH=/usr/bin/ninja
MAKE_PATH=/usr/bin/make

# Ensure we bail out if a command fails
set -e

echo "=== Benchmarking Full Builds ==="
hyperfine --warmup 2 \
  --prepare "$COB_PATH -C heavy_repo -t clean" "$COB_PATH -C heavy_repo" \
  --prepare "$NINJA_PATH -C heavy_repo -t clean" "$NINJA_PATH -C heavy_repo" \
  --prepare "$MAKE_PATH -C heavy_repo clean" "$MAKE_PATH -C heavy_repo" \
  --export-markdown build_comparison.md

echo -e "\n=== Benchmarking Clean Routines ==="
# We run a build in the prepare step so there is actually something to clean
hyperfine --warmup 2 \
  --prepare "$COB_PATH -C heavy_repo " "$COB_PATH -C heavy_repo -t clean" \
  --prepare "$NINJA_PATH -C heavy_repo" "$NINJA_PATH -C heavy_repo -t clean" \
  --prepare "$MAKE_PATH -C heavy_repo" "$MAKE_PATH -C heavy_repo clean" \
  --export-markdown clean_comparison.md

echo -e "\n=== Benchmarking CompDB Generation (cob, ninja) ==="

hyperfine --warmup 2 \
  "$COB_PATH -C heavy_repo -t compdb" \
  "$NINJA_PATH -C heavy_repo -t compdb" \
  --export-markdown compdb_comparision.md \
  --shell=none
