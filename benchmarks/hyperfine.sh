#!/usr/bin/env bash

# Ensure we bail out if a command fails
set -e

echo "=== Benchmarking Full Builds ==="
hyperfine --warmup 2 \
  --prepare 'cob -C heavy_repo -t clean' "cob -C heavy_repo" \
  --prepare 'ninja -C heavy_repo -t clean' "ninja -C heavy_repo" \
  --prepare 'make -C heavy_repo clean' "make -C heavy_repo" \
  --export-markdown build_comparison.md

echo -e "\n=== Benchmarking Clean Routines ==="
# We run a build in the prepare step so there is actually something to clean
hyperfine --warmup 2 \
  --prepare 'cob -C heavy_repo ' "cob -C heavy_repo -t clean" \
  --prepare 'ninja -C heavy_repo' "ninja -C heavy_repo -t clean" \
  --prepare 'make -C heavy_repo' "make -C heavy_repo clean" \
  --export-markdown clean_comparison.md

echo -e "\n=== Benchmarking CompDB Generation (cob, ninja) ==="

hyperfine --warmup 2 \
  "cob -C heavy_repo -t compdb" \
  "ninja -C heavy_repo -t compdb" \
  --export-markdown compdb_comparision.md \
  --shell=none
