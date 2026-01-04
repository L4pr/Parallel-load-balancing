import json
import os
import matplotlib.pyplot as plt

# ---------------------------------------------------------
# Configuration
# ---------------------------------------------------------
DATA_DIR = "../data/pc"
OUTPUT_FILE = "../data/benchmark_comparison.pdf"

# Map readable Benchmark names to the file prefix used in the shell script
BENCHMARKS = {
    "Fibonacci": "fib",
    # "UTS": "uts",
    # "N-Queens": "nqueens",
    # "Matrix Multiplication": "matmul"
}

# ---------------------------------------------------------
# VARIANT CONFIGURATION
# This dictionary controls the Legend Label.
# It maps the filename suffix (key) to the Display Label (value).
# Even if lace_original has no label in the JSON, it gets labeled here.
# ---------------------------------------------------------
VARIANTS = {
    "chase":         {"label": "Chase-Lev",       "color": "#1f77b4", "marker": "o"},
    "blocking":      {"label": "Blocking",        "color": "#ff7f0e", "marker": "s"},
    # "lace":          {"label": "Lace (New)",      "color": "#2ca02c", "marker": "^"},
    # "lace_original": {"label": "Lace (Original)", "color": "#d62728", "marker": "v"}
}

def load_benchmark_data(filepath):
    """
    Parses Google Benchmark JSON.
    It ONLY reads 'real_time' and 'threads'. It ignores internal labels.
    """
    if not os.path.exists(filepath):
        print(f"Warning: File not found: {filepath}")
        return None

    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return None

    results = {}

    for run in data.get("benchmarks", []):
        # We look for the aggregate 'mean' run
        is_aggregate = run.get("run_type") == "aggregate" and run.get("aggregate_name") == "mean"
        if not is_aggregate:
            continue

        # Extract Thread count
        # Priority: 'green_threads' (custom) > 'threads' (standard) > default 1
        if "green_threads" in run:
            threads = int(run["green_threads"])
        else:
            threads = int(run.get("threads", 1))

        time_ms = float(run["real_time"])
        results[threads] = time_ms

    return results

def get_baseline_time(all_results):
    """
    Finds the fastest single-threaded time across all variants to normalize speedup.
    """
    t1_times = []
    for variant_data in all_results.values():
        if variant_data and 1 in variant_data:
            t1_times.append(variant_data[1])

    if not t1_times:
        return None
    return min(t1_times)

def main():
    # Set a nice style
    plt.style.use('ggplot')

    # Create a 2x2 grid for the 4 benchmarks
    fig, axs = plt.subplots(2, 2, figsize=(14, 10))
    axs = axs.flatten()

    print(f"Reading data from: {DATA_DIR}")

    for i, (bench_name, file_prefix) in enumerate(BENCHMARKS.items()):
        ax = axs[i]

        # 1. Load data for every variant of this benchmark
        variant_results = {}
        for suffix in VARIANTS.keys():
            # Construct filename: e.g., "fib" + "_" + "lace_original" + ".json"
            filename = f"{file_prefix}_{suffix}.json"
            path = os.path.join(DATA_DIR, filename)

            # Load the data (ignoring missing labels in JSON)
            data = load_benchmark_data(path)

            if data:
                # Sort data by thread count to ensure the line plots correctly
                variant_results[suffix] = dict(sorted(data.items()))

        # 2. Get baseline (Fastest T=1 time across all 4 files)
        baseline = get_baseline_time(variant_results)

        if not baseline:
            ax.text(0.5, 0.5, "No Data", ha='center', va='center')
            ax.set_title(bench_name)
            continue

        # 3. Plot the lines
        max_threads = 0
        for suffix, info in VARIANTS.items():
            data = variant_results.get(suffix)
            if not data:
                continue

            threads = list(data.keys())
            times = list(data.values())

            # Calculate Speedup: Baseline / Current_Time
            speedups = [baseline / t for t in times]

            max_threads = max(max_threads, max(threads))

            # Use the label defined in VARIANTS dictionary
            ax.plot(threads, speedups,
                    label=info["label"],
                    color=info["color"],
                    marker=info["marker"],
                    markersize=6, linewidth=2, alpha=0.8)

        # 4. Add Ideal Scaling Line
        ax.plot([1, max_threads], [1, max_threads], 'k--', label="Ideal Linear", alpha=0.3)

        # 5. Styling
        ax.set_title(bench_name, fontsize=12, fontweight='bold')
        ax.set_xlabel("Threads")
        ax.set_ylabel("Relative Speedup")
        ax.legend(fontsize=10)
        ax.grid(True, which='both', linestyle='--', alpha=0.7)

        # Keep axes tight but readable
        ax.set_xlim(0, max_threads * 1.05)
        ax.set_ylim(0, max_threads * 1.1)

    plt.suptitle("Libfork vs Lace: Relative Speedup Comparison", fontsize=16)
    plt.tight_layout(rect=[0, 0.03, 1, 0.95])

    print(f"Saving plot to {OUTPUT_FILE}...")
    plt.savefig(OUTPUT_FILE)
    print("Done.")

if __name__ == "__main__":
    main()