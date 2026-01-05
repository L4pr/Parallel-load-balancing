#!/bin/bash

# Stop execution if any command fails
set -e

# ---------------------------------------------------------
# 0. Update and Build
# ---------------------------------------------------------
echo "Updating repository..."
git pull

echo "Configuring CMake..."
cmake --preset=benchmark-nix

echo "Building benchmarks..."
cmake --build --preset=benchmark-nix

# Configuration
REPS=5
OUT_DIR="data/pc"
BIN_DIR="./build/bench/bench"

# Ensure output directory exists
mkdir -p "$OUT_DIR"

echo "---------------------------------------------------------"
echo "Starting benchmarks with ${REPS} repetitions..."
echo "Output directory: ${OUT_DIR}"
echo "---------------------------------------------------------"

# ---------------------------------------------------------
# 1. Fibonacci
# ---------------------------------------------------------
echo "Running Fibonacci..."

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="fib_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/fib_chase.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_blocking" \
    --benchmark_time_unit=ms --benchmark_filter="fib_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/fib_blocking.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_lace" \
    --benchmark_time_unit=ms --benchmark_filter="fib_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/fib_lace.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="fib_lace" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/fib_lace_original.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="fib_serial" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/fib_serial.json" \
    --benchmark_repetitions=$REPS

# ---------------------------------------------------------
# 2. UTS (Unbalanced Tree Search)
# ---------------------------------------------------------
echo "Running UTS..."

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="uts_libfork.*T3L" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/uts_chase.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_blocking" \
    --benchmark_time_unit=ms --benchmark_filter="uts_libfork.*T3L" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/uts_blocking.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_lace" \
    --benchmark_time_unit=ms --benchmark_filter="uts_libfork.*T3L" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/uts_lace.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="uts_lace.*T3L" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/uts_lace_original.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="uts_serial.*T3L" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/uts_serial.json" \
    --benchmark_repetitions=$REPS

# ---------------------------------------------------------
# 3. N-Queens
# ---------------------------------------------------------
echo "Running N-Queens..."

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="nqueens_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/nqueens_chase.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_blocking" \
    --benchmark_time_unit=ms --benchmark_filter="nqueens_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/nqueens_blocking.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_lace" \
    --benchmark_time_unit=ms --benchmark_filter="nqueens_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/nqueens_lace.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="nqueens_lace" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/nqueens_lace_original.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="nqueens_serial" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/nqueens_serial.json" \
    --benchmark_repetitions=$REPS

# ---------------------------------------------------------
# 4. Matrix Multiplication
# ---------------------------------------------------------
echo "Running MatMul..."

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="matmul_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/matmul_chase.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_blocking" \
    --benchmark_time_unit=ms --benchmark_filter="matmul_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/matmul_blocking.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_lace" \
    --benchmark_time_unit=ms --benchmark_filter="matmul_libfork" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/matmul_lace.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="matmul_lace" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/matmul_lace_original.json" \
    --benchmark_repetitions=$REPS

"${BIN_DIR}/bench_chase_lev" \
    --benchmark_time_unit=ms --benchmark_filter="matmul_serial" \
    --benchmark_out_format=json --benchmark_out="${OUT_DIR}/matmul_serial.json" \
    --benchmark_repetitions=$REPS

echo "Done! All results saved to ${OUT_DIR}"