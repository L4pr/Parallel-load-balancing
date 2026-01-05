import numpy as np
from matplotlib import pyplot as plt
import argparse
import json
import os
from statistics import median, stdev

# EXACTLY as original: Use system LaTeX
plt.rcParams["text.usetex"] = True

# ---------------------------------------------------------
# Configuration: Map your new file suffixes to labels
# ---------------------------------------------------------
# The script will look for files named like: {benchmark}_{suffix}.json
VARIANTS = {
    "chase":         {"label": "Chase-Lev",       "mark": "o"},
    "blocking":      {"label": "Blocking",        "mark": "s"},
    "lace":          {"label": "Lace (New)",      "mark": "^"},
    "lace_original": {"label": "Lace (Original)", "mark": "v"}
}

# The benchmark prefixes you generated
PATTERNS = ["fib", "uts", "nqueens", "matmul"]

# Directory where your bash script saved the JSONs
DATA_DIR = "data/pc"

# ---------------------------------------------------------
# Helper Functions (Preserving original logic)
# ---------------------------------------------------------
def stat(x):
    # Original logic: exclude the last element (often an outlier in some suites)
    # or just sort. Your original script had `x = sorted(x)[:-1]`.
    # I will stick to that if that was your intention, or standard sort.
    # Let's use standard sort to be safe unless you want to drop data.
    x = sorted(x)[:]

    err = stdev(x) / (np.sqrt(len(x)) if len(x) > 1 else 0)
    return median(x), err, min(x)

def load_data():
    """
    Reads all JSON files and organizes them by Benchmark -> Variant -> Threads.
    """
    # Structure: benchmarks[benchmark_name][variant_name][thread_count] = [times...]
    bench_data = {p: {} for p in PATTERNS}

    for bench_name in PATTERNS:
        for suffix in VARIANTS.keys():
            filename = f"{bench_name}_{suffix}.json"
            filepath = os.path.join(DATA_DIR, filename)

            if not os.path.exists(filepath):
                print(f"Skipping missing file: {filepath}")
                continue

            with open(filepath, 'r') as f:
                try:
                    data = json.load(f)
                except json.JSONDecodeError:
                    print(f"Error decoding {filepath}")
                    continue

            # Organize data for this variant
            variant_data = {}

            for run in data.get("benchmarks", []):
                # Only look at raw iterations, ignoring the aggregate rows
                if run.get("run_type") != "iteration":
                    continue

                # Parse thread count
                # Priority: green_threads > threads > 1
                if "green_threads" in run:
                    th = int(float(run["green_threads"]) + 0.5)
                else:
                    th = int(run.get("threads", 1))

                if th not in variant_data:
                    variant_data[th] = []

                variant_data[th].append(run["real_time"])

            # Sort by thread count
            sorted_variant = sorted(variant_data.items())
            bench_data[bench_name][suffix] = sorted_variant

    return bench_data

# ---------------------------------------------------------
# Main Script
# ---------------------------------------------------------
parser = argparse.ArgumentParser()
parser.add_argument("-o", "--output_file", help="Output file", default="benchmark_plot.pdf")
parser.add_argument("-r", "--rel", help="plot relative speedup", action="store_true")
args = parser.parse_args()

# Load all data into memory first
all_data = load_data()

# Create 2x2 grid (since you have 4 benchmarks)
fig, axs = plt.subplots(2, 2, figsize=(6, 5), sharex="col", sharey="row")
axs_flat = axs.flatten()

count = 0

for ax_abs, p in zip(axs_flat, PATTERNS):
    print(f"Plotting {p}...")

    # 1. Find Baseline (Fastest T=1 across all variants for this benchmark)
    # The original script looked for "serial". Since we don't have that,
    # we simulate it by finding the best single-thread time among the loaded variants.
    tS = float('inf')
    tSerr = 0

    # Scan for the best T=1
    baseline_found = False
    for suffix, data_points in all_data[p].items():
        # data_points is list of (threads, [times])
        for th, times in data_points:
            if th == 1:
                med, err, _ = stat(times)
                if med < tS:
                    tS = med
                    tSerr = err
                    baseline_found = True

    if not baseline_found:
        print(f"  No T=1 data found for {p}, skipping normalization.")
        tS = 1 # Avoid division by zero if plotting raw time

    # 2. Plotting Loop
    ymax = 0
    xmax = 0

    for suffix, data_points in all_data[p].items():
        if not data_points:
            continue

        label_info = VARIANTS[suffix]
        label = label_info["label"]
        mark = label_info["mark"]

        # Extract X (threads) and Y (stats)
        x = np.asarray([item[0] for item in data_points])
        # stat returns (median, err, min)
        # We unzip them into separate arrays
        stats = [stat(item[1]) for item in data_points]
        y   = np.asarray([s[0] for s in stats])
        err = np.asarray([s[1] for s in stats])

        xmax = max(xmax, max(x))

        # Calculate Speedup (T_serial / T_parallel)
        # Or Efficiency if args.rel is True
        t = tS / y

        # Propagate Error
        f_yerr = err / y
        f_serr = tSerr / tS
        terr = t * np.sqrt(f_yerr**2 + f_serr**2)

        if args.rel:
            t /= x
            terr /= x

        # Plot
        # Only label the first subplot to avoid legend duplicates later (optional,
        # but your original script handled legend globally at the end)
        ax_abs.errorbar(
            x, t, yerr=terr,
            label=label if count == 0 else None,
            capsize=2,
            marker=mark,
            markersize=4,
            linewidth=1
        )

        ymax = max(ymax, max(t))

    # 3. Formatting

    # Ideal Scaling Line
    if not args.rel:
        ideal_x = np.arange(1, xmax + 1)
        ax_abs.plot(ideal_x, ideal_x, color="black", linestyle="dashed", linewidth=1)

    # Ticks matching your original script (every 14 cores? or logical steps?)
    # Your original used: range(0, int(xmax + 1.5), 14)
    # I will adapt to a more standard step like 20 since you have 120 cores.
    step = 20
    ax_abs.set_xticks(range(0, int(xmax + 5), step))
    ax_abs.set_xlim(0, xmax + 5)

    # Title using LaTeX italics
    ax_abs.set_title(f"\\textit{{{p.capitalize()}}}")

    ax_abs.set_ylim(bottom=0)

    count += 1

# 4. Global Figure Elements

fig.supxlabel("\\textbf{Cores}")

if args.rel:
    fig.supylabel("\\textbf{Efficiency}")
else:
    fig.supylabel("\\textbf{Speedup}")

# Extract handles and labels from the first axis for the global legend
handles, labels = axs_flat[0].get_legend_handles_labels()

# Add "Ideal" manually if you plotted it but didn't label it in the loop
# (The original script logic for legend is slightly complex, simple approach here:)
fig.legend(
    handles, labels,
    loc="upper center",
    ncol=4,  # Adjust columns based on number of variants
    frameon=False,
    bbox_to_anchor=(0.5, 0.98) # Push to top
)

# Tight layout rect to make room for legend at top
fig.tight_layout(rect=(0, 0, 1, 0.90))

if args.output_file:
    print(f"Saving to {args.output_file}...")
    plt.savefig(args.output_file, bbox_inches="tight")
else:
    plt.show()