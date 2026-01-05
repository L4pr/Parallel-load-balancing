import json
import os
import matplotlib.pyplot as plt
import numpy as np
from statistics import median, stdev

# ===========================================
# CONFIGURATION & STYLE SETUP
# ===========================================

# Try to enable LaTeX rendering. Fallback if texlive isn't installed.
try:
    plt.rcParams["text.usetex"] = True
    plt.rcParams['font.family'] = 'serif'
except:
    print("Warning: LaTeX not detected. Falling back to standard fonts.")
    plt.rcParams["text.usetex"] = False

DATA_DIR = "../data/pc"
OUTPUT_FILE = "benchmark_comparison.pdf"

# The 4 Benchmarks to plot (rows in the shell script)
BENCHMARKS = {
    "Fibonacci": "fib",
    # "UTS": "uts",
    # "N-Queens": "nqueens",
    # "Matrix Multiplication": "matmul"
}

# The 4 Variants to compare (columns in the shell script)
# Defining distinct styles for each.
VARIANTS = {
    "chase":         {"label": "Chase-Lev",       "c": "#1f77b4", "m": "o", "ls": "-"},
    "blocking":      {"label": "Blocking",        "c": "#ff7f0e", "m": "s", "ls": "--"},
    # "lace":          {"label": "Lace (New)",      "c": "#2ca02c", "m": "^", "ls": "-."},
    # "lace_original": {"label": "Lace (Original)", "c": "#d62728", "m": "v", "ls": ":"}
}

# ===========================================
# HELPER FUNCTIONS
# ===========================================

def get_stat(data_list):
    """
    Calculates median and standard error relative to the median.
    Adapted from the provided example script.
    """
    if not data_list:
        return 0, 0

    # Use all data points available
    x = sorted(data_list)
    n = len(x)

    med = median(x)

    if n > 1:
        # Standard error of the mean approximation
        err_abs = stdev(x) / np.sqrt(n)
        # Relative error for error propagation later
        err_rel = err_abs / med if med != 0 else 0
    else:
        err_abs = 0
        err_rel = 0

    return med, err_rel

def load_raw_data(filepath):
    """
    Reads JSON and collects ALL raw repetition times for each thread count.
    Returns: {thread_count: [time_rep1, time_rep2, ...]}
    """
    if not os.path.exists(filepath):
        print(f"!! Missing file: {filepath}")
        return None

    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
    except Exception as e:
        print(f"!! Error reading {filepath}: {e}")
        return None

    raw_results = {}

    for run in data.get("benchmarks", []):
        # We only want individual iterations, not aggregates
        if run.get("run_type") == "aggregate":
            continue

        # Determine thread count
        if "green_threads" in run:
            # Often stored as float in JSON, convert to int
            threads = int(float(run["green_threads"]) + 0.5)
        else:
            threads = int(run.get("threads", 1))

        time_ms = float(run["real_time"])

        if threads not in raw_results:
            raw_results[threads] = []
        raw_results[threads].append(time_ms)

    return raw_results

# ===========================================
# MAIN PLOTTING LOGIC
# ===========================================

def main():
    # Setup figure with 2x2 grid
    fig, axs = plt.subplots(2, 2, figsize=(10, 8), sharex=True)
    axs = axs.flatten()

    print(f"Reading data from: {DATA_DIR}")

    global_max_threads = 0

    for i, (bench_readable_name, file_prefix) in enumerate(BENCHMARKS.items()):
        ax = axs[i]
        print(f"Processing {bench_readable_name}...")

        # 1. Load ALL raw data for this benchmark across all variants
        # Structure: variant_data["chase"][thread_count] = [list of times]
        variant_raw_data = {}
        for suffix in VARIANTS.keys():
            filename = f"{file_prefix}_{suffix}.json"
            path = os.path.join(DATA_DIR, filename)
            raw = load_raw_data(path)
            if raw:
                variant_raw_data[suffix] = raw

        # 2. Find the Baseline: The absolute fastest median time at T=1
        # This ensures True Speedup comparison.
        t1_medians = []
        for raw in variant_raw_data.values():
            if 1 in raw:
                m, _ = get_stat(raw[1])
                t1_medians.append(m)

        baseline_time_s = min(t1_medians) if t1_medians else None

        if not baseline_time_s:
            ax.text(0.5, 0.5, "No T=1 Data Found", ha='center', va='center')
            ax.set_title(f"\\textit{{{bench_readable_name}}}")
            continue

        # 3. Process and plot each variant
        current_max_threads = 0
        for suffix, style in VARIANTS.items():
            raw = variant_raw_data.get(suffix)
            if not raw: continue

            # Sort by thread count
            sorted_threads = sorted(raw.keys())
            current_max_threads = max(current_max_threads, max(sorted_threads) if sorted_threads else 0)

            x_threads = []
            y_speedup = []
            y_errors = []

            for t in sorted_threads:
                med_time, rel_err = get_stat(raw[t])

                # Calculate Speedup: Baseline / Current
                speedup = baseline_time_s / med_time

                # Propagate error (simple approximation for division)
                # Error in speedup = speedup * relative_error_of_time
                abs_err_speedup = speedup * rel_err

                x_threads.append(t)
                y_speedup.append(speedup)
                y_errors.append(abs_err_speedup)

            # Plotting with error bars and styles
            ax.errorbar(x_threads, y_speedup, yerr=y_errors,
                        label=style["label"],
                        color=style["c"],
                        marker=style["m"],
                        linestyle=style["ls"],
                        markersize=5,
                        capsize=2, alpha=0.9)

        global_max_threads = max(global_max_threads, current_max_threads)

        # 4. Add Ideal Scaling Line
        ax.plot([1, current_max_threads], [1, current_max_threads],
                color='black', linestyle=':', linewidth=1, alpha=0.5, label="Ideal")

        # 5. Axes Styling per subplot
        ax.set_title(f"\\textit{{{bench_readable_name}}}")
        ax.grid(True, which='major', linestyle='--', alpha=0.5)
        ax.set_ylim(bottom=0)
        ax.set_xlim(left=0.5)

    # ===========================================
    # GLOBAL FIGURE STYLING
    # ===========================================

    # Set standard ticks if we found data
    if global_max_threads > 0:
        # Create sensible ticks, e.g., 1, 10, 20, 30... up to max
        ticks = [1] + list(range(10, global_max_threads + 5, 10))
        for ax in axs:
            ax.set_xticks(ticks)
            ax.set_xlim(0, global_max_threads * 1.05)

    # Shared labels using LaTeX bold
    fig.supxlabel("\\textbf{Cores}", y=0.02)
    fig.supylabel("\\textbf{Speedup}", x=0.02)

    # Shared Legend at the top
    # We grab handles/labels from the last plotted axis
    handles, labels = axs[-1].get_legend_handles_labels()
    # Filter out 'Ideal' line from legend if desired, or keep it. Let's keep it.
    fig.legend(handles, labels,
               loc="upper center",
               bbox_to_anchor=(0.5, 0.98),
               ncol=5,
               frameon=False, fontsize=11)

    # Adjust layout to make room for legend and labels
    plt.tight_layout(rect=[0.03, 0.05, 1, 0.92])

    print(f"Saving plot to {OUTPUT_FILE}...")
    plt.savefig(OUTPUT_FILE, dpi=300)
    print("Done.")

if __name__ == "__main__":
    main()