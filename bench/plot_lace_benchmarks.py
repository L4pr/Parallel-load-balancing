import json
import os
import matplotlib.pyplot as plt
import numpy as np
from statistics import median, stdev

# ===========================================
# 1. STYLE CONFIGURATION (The "Original" Look)
# ===========================================

# We use Matplotlib's internal engine to mimic LaTeX exactly
# without requiring an external latex installation.
plt.rcParams.update({
    "text.usetex": False,                 # Disable external dependency
    "font.family": "serif",               # Use Serif (Roman) fonts
    "mathtext.fontset": "cm",             # Use 'Computer Modern' (Standard LaTeX font)
    "font.serif": ["Computer Modern Roman", "DejaVu Serif", "serif"]
})

# ===========================================
# 2. DATA CONFIGURATION
# ===========================================
DATA_DIR = "../data/pc"
OUTPUT_FILE = "benchmark_comparison.pdf"

BENCHMARKS = {
    "Fibonacci": "fib",
    # "UTS": "uts",
    # "N-Queens": "nqueens",
    # "Matrix Multiplication": "matmul"
}

# Define markers and colors to match your preference
VARIANTS = {
    "chase":         {"label": "Chase-Lev",       "c": "#1f77b4", "m": "o", "ls": "-"},
    "blocking":      {"label": "Blocking",        "c": "#ff7f0e", "m": "s", "ls": "--"},
    # "lace":          {"label": "Lace (New)",      "c": "#2ca02c", "m": "^", "ls": "-."},
    # "lace_original": {"label": "Lace (Original)", "c": "#d62728", "m": "v", "ls": ":"}
}

# ===========================================
# 3. HELPER FUNCTIONS
# ===========================================

def get_stat(data_list):
    """
    Replicates the 'stat' function from the original script:
    Returns median and relative error.
    """
    if not data_list:
        return 0, 0

    x = sorted(data_list)
    n = len(x)
    med = median(x)

    if n > 1:
        # Standard Error calculation
        err_abs = stdev(x) / np.sqrt(n)
        # We need relative error because we divide baselines later
        err_rel = err_abs / med if med != 0 else 0
    else:
        err_abs = 0
        err_rel = 0

    return med, err_rel

def load_raw_data(filepath):
    """
    Parses JSON and extracts raw iteration times.
    Ignores the internal 'label' field to solve the lace_original issue.
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

    raw_results = {}

    for run in data.get("benchmarks", []):
        # Skip aggregate rows (mean, median, etc.)
        if run.get("run_type") == "aggregate":
            continue

        # Extract Thread Count
        if "green_threads" in run:
            threads = int(float(run["green_threads"]) + 0.5)
        else:
            threads = int(run.get("threads", 1))

        time_ms = float(run["real_time"])

        if threads not in raw_results:
            raw_results[threads] = []
        raw_results[threads].append(time_ms)

    return raw_results

# ===========================================
# 4. MAIN PLOTTING LOGIC
# ===========================================

def main():
    # Create the 2x2 grid
    fig, axs = plt.subplots(2, 2, figsize=(10, 8), sharex=True)
    axs = axs.flatten()

    print(f"Reading data from: {DATA_DIR}")
    global_max_threads = 0

    for i, (bench_name, file_prefix) in enumerate(BENCHMARKS.items()):
        ax = axs[i]

        # -------------------------------------------------
        # A. Load Data
        # -------------------------------------------------
        variant_data = {}
        for suffix in VARIANTS.keys():
            path = os.path.join(DATA_DIR, f"{file_prefix}_{suffix}.json")
            data = load_raw_data(path)
            if data:
                variant_data[suffix] = data

        # -------------------------------------------------
        # B. Calculate Baseline (Fastest T=1)
        # -------------------------------------------------
        # Since we don't have a "serial" run, we find the fastest
        # single-thread run across all 4 algorithms to act as the baseline.
        t1_medians = []
        for v_data in variant_data.values():
            if 1 in v_data:
                m, _ = get_stat(v_data[1])
                t1_medians.append(m)

        baseline = min(t1_medians) if t1_medians else None

        if not baseline:
            ax.text(0.5, 0.5, "No Data", ha='center', va='center')
            ax.set_title(f"$\\it{{{bench_name}}}$")
            continue

        # -------------------------------------------------
        # C. Plot Lines
        # -------------------------------------------------
        current_max_threads = 0
        for suffix, style in VARIANTS.items():
            raw = variant_data.get(suffix)
            if not raw: continue

            # Get sorted thread counts
            threads = sorted(raw.keys())
            current_max_threads = max(current_max_threads, max(threads))
            global_max_threads = max(global_max_threads, current_max_threads)

            x_vals = []
            y_vals = []
            y_errs = []

            for t in threads:
                med_time, rel_err = get_stat(raw[t])

                # Speedup = Baseline / Current_Time
                speedup = baseline / med_time

                # Propagate Error: Speedup * Relative_Error
                err = speedup * rel_err

                x_vals.append(t)
                y_vals.append(speedup)
                y_errs.append(err)

            # Plot with Error Bars (capsize=2 matches original)
            ax.errorbar(x_vals, y_vals, yerr=y_errs,
                        label=style["label"],
                        color=style["c"],
                        marker=style["m"],
                        linestyle=style["ls"],
                        markersize=4,
                        capsize=2,      # Matches original script
                        elinewidth=1,
                        alpha=0.9)

        # -------------------------------------------------
        # D. Add Ideal Scaling Line
        # -------------------------------------------------
        ax.plot([1, current_max_threads], [1, current_max_threads],
                color='black', linestyle=':', linewidth=1, alpha=0.6, label="Ideal")

        # -------------------------------------------------
        # E. Subplot Styling
        # -------------------------------------------------
        # Use MathText italics: $\it{Name}$
        ax.set_title(f"$\\it{{{bench_name}}}$", fontsize=12)
        ax.grid(True, which='major', linestyle='--', alpha=0.5)
        ax.set_ylim(bottom=0)
        ax.set_xlim(left=0)

    # ===========================================
    # 5. GLOBAL STYLING (The "Original" Look)
    # ===========================================

    # Set X-Axis Ticks (every 14 cores, similar to original, or 10/20)
    # Adjust 'step' based on your machine (original used 14, likely for 14-core grouping)
    step = 20
    ticks = list(range(0, global_max_threads + step, step))
    for ax in axs:
        ax.set_xticks(ticks)
        ax.set_xlim(0, global_max_threads * 1.05)

    # Use MathText Bold: $\bf{Text}$
    fig.supxlabel(r"$\bf{Cores}$", y=0.02, fontsize=12)
    fig.supylabel(r"$\bf{Speedup}$", x=0.02, fontsize=12)

    # Shared Legend
    handles, labels = axs[-1].get_legend_handles_labels()
    fig.legend(handles, labels,
               loc="upper center",
               bbox_to_anchor=(0.5, 0.98),
               ncol=5,
               frameon=False, fontsize=10)

    plt.tight_layout(rect=[0.03, 0.05, 1, 0.92])

    print(f"Saving plot to {OUTPUT_FILE}...")
    plt.savefig(OUTPUT_FILE, dpi=300)
    print("Done.")

if __name__ == "__main__":
    main()