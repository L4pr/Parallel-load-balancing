import numpy as np
from matplotlib import pyplot as plt
import argparse
import json
import os
from statistics import median, stdev

# EXACTLY as original: Use system LaTeX
plt.rcParams["text.usetex"] = True

# ---------------------------------------------------------
# Configuration
# ---------------------------------------------------------

# TOGGLE: Set to True to compare each variant against its own T=1 run.
#         Set to False to compare all variants against the 'serial' file.
USE_SELF_BASELINE = False

VARIANTS = {
    "chase":         {"label": "Chase-Lev",       "mark": "o"},
    "blocking":      {"label": "Blocking",        "mark": "s"},
    "lace":          {"label": "Lace (New)",      "mark": "^"},
    "lace_original": {"label": "Lace (Original)", "mark": "v"}
}

PATTERNS = ["fib", "uts", "nqueens", "matmul"]
DATA_DIR = "../data/server"

# ---------------------------------------------------------
# Helper Functions
# ---------------------------------------------------------
def stat(x):
    x = sorted(x)[:]
    err = stdev(x) / (np.sqrt(len(x)) if len(x) > 1 else 1)
    return median(x), err, min(x)

def load_data():
    bench_data = {p: {} for p in PATTERNS}
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

# sharex=False ensures Core numbers appear on all graphs
fig, axs = plt.subplots(2, 2, figsize=(8, 8), sharex=False, sharey=False)
axs_flat = axs.flatten()

count = 0
for ax_abs, p in zip(axs_flat, PATTERNS):
    print(f"Plotting {p}...")

    # --- 1. PRE-CALCULATE SERIAL BASELINE (Only if NOT using self-baseline) ---
    global_tS = -1
    global_tSerr = -1

    if not USE_SELF_BASELINE:
        if "serial" in all_data[p]:
            # Use the dedicated serial file
            global_tS, global_tSerr, _ = stat(all_data[p]["serial"][0][1])
        else:
            print(f"  No serial file found for {p}, falling back to fastest T=1...")
            # Fallback: Find the fastest T=1 among all variants
            t1_times = []
            for suffix in VARIANTS.keys():
                if suffix in all_data[p] and all_data[p][suffix][0][0] == 1:
                    t1_times.append(stat(all_data[p][suffix][0][1]))
            if t1_times:
                best_stat = min(t1_times, key=lambda x: x[0])
                global_tS, global_tSerr = best_stat[0], best_stat[1]

        if global_tS == -1:
            global_tS = 1.0 # Prevent crash
            print(f"  Error: No baseline found for {p}")

    # --- 2. PLOTTING VARIANTS ---
    ymax = 0
    xmax = 0

    for suffix, data_points in all_data[p].items():
        if suffix == "serial" or suffix not in VARIANTS:
            continue

        label_info = VARIANTS[suffix]

        # Get X and Y data
        x = np.asarray([item[0] for item in data_points])
        stats = [stat(item[1]) for item in data_points]
        y   = np.asarray([s[0] for s in stats])
        err = np.asarray([s[1] for s in stats])

        xmax = max(xmax, max(x) if len(x) > 0 else 0)

        # --- DETERMINE BASELINE FOR THIS LINE ---
        if USE_SELF_BASELINE:
            # Baseline is THIS variant's T=1 time (assumed to be index 0)
            if data_points[0][0] == 1:
                tS, tSerr, _ = stat(data_points[0][1])
            else:
                # If T=1 is missing, cannot compute self-speedup easily
                tS, tSerr = y[0], err[0]
        else:
            # Baseline is the global serial file calculated earlier
            tS, tSerr = global_tS, global_tSerr

        # --- SPEEDUP CALCULATION ---
        t_speedup = tS / y

        # Error Propagation
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