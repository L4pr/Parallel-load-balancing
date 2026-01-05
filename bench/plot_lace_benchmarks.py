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
VARIANTS = {
    "chase":         {"label": "Chase-Lev",       "mark": "o"},
    "blocking":      {"label": "Blocking",        "mark": "s"},
    "lace":          {"label": "Lace (New)",      "mark": "^"},
    "lace_original": {"label": "Lace (Original)", "mark": "v"}
}

# The benchmark prefixes
PATTERNS = ["fib",
            "uts",
            "nqueens",
            "matmul"
            ]

# Directory where your bash script saved the JSONs
DATA_DIR = "../data/pc"

# ---------------------------------------------------------
# Helper Functions (Preserving original logic)
# ---------------------------------------------------------
def stat(x):
    # Original logic: sort and keep all points
    x = sorted(x)[:]
    err = stdev(x) / (np.sqrt(len(x)) if len(x) > 1 else 1)
    return median(x), err, min(x)

def load_data():
    """
    Reads all JSON files and organizes them.
    Includes 'serial' in the search to act as the baseline.
    """
    bench_data = {p: {} for p in PATTERNS}

    # We add "serial" to the suffixes to check for baseline files
    all_suffixes = list(VARIANTS.keys()) + ["serial"]

    for bench_name in PATTERNS:
        for suffix in all_suffixes:
            filename = f"{bench_name}_{suffix}.json"
            filepath = os.path.join(DATA_DIR, filename)

            if not os.path.exists(filepath):
                continue

            with open(filepath, 'r') as f:
                try:
                    data = json.load(f)
                except Exception:
                    continue

            variant_data = {}
            for run in data.get("benchmarks", []):
                if run.get("run_type") != "iteration":
                    continue

                th = int(float(run.get("green_threads", run.get("threads", 1))) + 0.5)
                if th not in variant_data:
                    variant_data[th] = []
                variant_data[th].append(run["real_time"])

            bench_data[bench_name][suffix] = sorted(variant_data.items())

    return bench_data

# ---------------------------------------------------------
# Main Script
# ---------------------------------------------------------
parser = argparse.ArgumentParser()
parser.add_argument("-o", "--output_file", help="Output file", default="benchmark_plot.pdf")
parser.add_argument("-r", "--rel", help="plot relative speedup", action="store_true")
args = parser.parse_args()

all_data = load_data()

# Create 2x2 grid
fig, axs = plt.subplots(2, 2, figsize=(8, 8), sharex=True, sharey=False)
axs_flat = axs.flatten()

count = 0
for ax_abs, p in zip(axs_flat, PATTERNS):
    print(f"Plotting {p}...")

    # --- 1. SEARCH FOR SERIAL BASELINE (Original Logic) ---
    tS = -1
    tSerr = -1

    # Look for the 'serial' entry for this benchmark
    if "serial" in all_data[p]:
        # Original: tS, tSerr, _ = stat(v[0][1])
        # v[0] is the first thread entry (T=1), v[0][1] is the list of times
        tS, tSerr, _ = stat(all_data[p]["serial"][0][1])
    else:
        print(f"  No serial file found for {p}, falling back to fastest T=1...")
        # Fallback to find fastest T=1 if serial file is missing
        t1_times = []
        for suffix in VARIANTS.keys():
            if suffix in all_data[p] and all_data[p][suffix][0][0] == 1:
                t1_times.append(stat(all_data[p][suffix][0][1]))
        if t1_times:
            best_stat = min(t1_times, key=lambda x: x[0])
            tS, tSerr = best_stat[0], best_stat[1]

    if tS == -1:
        tS = 1.0 # Prevent crash
        print(f"  Error: No baseline found for {p}")

    # --- 2. PLOTTING VARIANTS ---
    ymax = 0
    xmax = 0

    for suffix, data_points in all_data[p].items():
        # Don't plot the serial line itself as a curve
        if suffix == "serial" or suffix not in VARIANTS:
            continue

        label_info = VARIANTS[suffix]

        x = np.asarray([item[0] for item in data_points])
        stats = [stat(item[1]) for item in data_points]
        y   = np.asarray([s[0] for s in stats])
        err = np.asarray([s[1] for s in stats])

        xmax = max(xmax, max(x) if len(x) > 0 else 0)

        # SPEEDUP CALCULATION (Original Logic)
        t_speedup = tS / y

        # ERROR PROPAGATION (Original Logic)
        f_yerr = err / y
        f_serr = tSerr / tS
        terr = t_speedup * np.sqrt(f_yerr**2 + f_serr**2)

        if args.rel:
            t_speedup /= x
            terr /= x

        ax_abs.errorbar(
            x, t_speedup, yerr=terr,
            label=label_info["label"] if count == 0 else None,
            capsize=2,
            marker=label_info["mark"],
            markersize=4,
            linewidth=1
        )
        ymax = max(ymax, max(t_speedup))

    # --- 3. FORMATTING ---
    if not args.rel:
        # Ideal line goes from (1,1) to (max, max)
        ax_abs.plot([1, xmax], [1, xmax], color="black", linestyle="dashed", linewidth=1, alpha=0.5)

    ax_abs.set_xticks(range(0, int(xmax + 20), 20))
    ax_abs.set_xlim(0, xmax + 5)
    ax_abs.set_title(f"\\textit{{{p.capitalize()}}}")
    ax_abs.set_ylim(bottom=0)
    ax_abs.grid(True, linestyle='--', alpha=0.5)

    count += 1

# --- 4. GLOBAL ELEMENTS ---
fig.supxlabel("\\textbf{Cores}")
fig.supylabel("\\textbf{Efficiency}" if args.rel else "\\textbf{Speedup}")

handles, labels = axs_flat[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=4, frameon=False, bbox_to_anchor=(0.5, 0.98))

fig.tight_layout(rect=(0, 0, 1, 0.92))

if args.output_file:
    print(f"Saving to {args.output_file}...")
    plt.savefig(args.output_file, bbox_inches="tight")
else:
    plt.show()