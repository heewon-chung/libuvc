#!/usr/bin/env python3
"""
Generate LaTeX table fragments from benchmark CSV files.

Produces tables matching the JISA paper Tables 2 and 3 format,
plus a crossover analysis showing at which step j UVC beats Groth16.

All data comes from the shared loader bench_data.load_run(); every
generator receives the already-indexed `data` dict.

Usage:
    python gen_latex_tables.py --input-dir ../csv --output-dir ../tables
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "scripts"))

import bench_data
from bench_data import GROTH16_MODES


CANONICAL_UVC_HEADER = bench_data.CANONICAL_UVC_HEADER


def fmt_ms(val_str):
    """Format millisecond value for LaTeX."""
    if val_str is None or val_str == '':
        return '--'
    v = float(val_str)
    if v < 1:
        return f'{v:.3f}'
    elif v < 100:
        return f'{v:.1f}'
    elif v < 10000:
        return f'{v:.0f}'
    else:
        return f'{v/1000:.1f}k'


def fmt_size(val_str):
    """Format size value for LaTeX."""
    if val_str is None or val_str == '':
        return '--'
    v = int(float(val_str))
    if v < 1000:
        return str(v)
    elif v < 1000000:
        return f'{v/1000:.1f}k'
    else:
        return f'{v/1000000:.1f}M'


def g16_row(data, mode, circuit, n, B, step):
    """One Groth16 mode row, with the legacy fallback for on-demand."""
    row = data["g16_modes"].get((mode, circuit, n, B, step))
    if row is None and mode == "ondemand":
        row = data["g16_legacy"].get((circuit, n, step))
    return row


def uvc_configs(data):
    return bench_data.uvc_configs(data)


def gen_uvc_table(data, output_path):
    """Generate Table 2: UVC performance across B values."""
    with open(output_path, 'w') as f:
        f.write('% Auto-generated UVC benchmark table\n')
        f.write('\\begin{table}[htbp]\n')
        f.write('\\centering\n')
        f.write('\\caption{UVC scheme performance: setup, prove, and verify times.}\n')
        f.write('\\label{tab:uvc-bench}\n')
        f.write('\\scriptsize\n')
        f.write('\\begin{tabular}{lllrrrrrrr}\n')
        f.write('\\toprule\n')
        f.write('Scheme & Circuit & $n$ & $B$ & Step $j$ & Setup (ms) & Prove (ms) & '
                'Verify (ms) & CRS $|\\mathbb{G}_1|$ & Proof (B) \\\\\n')
        f.write('\\midrule\n')

        for (circuit, n, B) in uvc_configs(data):
            steps = bench_data.uvc_steps(data, circuit, n, B)
            first = True
            for s in steps:
                r = data["uvc"][(circuit, n, B, s)]
                scheme_col = 'UVC' if first else ''
                circ_col = f'\\texttt{{{circuit}}}' if first else ''
                n_col = str(n) if first else ''
                b_col = str(B) if first else ''
                f.write(f'{scheme_col} & {circ_col} & {n_col} & {b_col} & {r["step"]} & '
                        f'{fmt_ms(r["setup_ms"])} & {fmt_ms(r["prove_ms"])} & '
                        f'{fmt_ms(r["verify_ms"])} & '
                        f'{fmt_size(r.get("crs_g1", r.get("crs_g1_published", "")))} & '
                        f'{r.get("proof_bytes", "--")} \\\\\n')
                first = False
            f.write('\\midrule\n')

        f.write('\\bottomrule\n')
        f.write('\\end{tabular}\n')
        f.write('\\end{table}\n')

    print(f"  UVC table written to {output_path}")


def gen_comparison_table(data, output_path):
    """Table 3: UVC vs the three Groth16 modes at step = B."""
    configs = uvc_configs(data)
    with open(output_path, 'w') as f:
        f.write('% Auto-generated comparison table: UVC vs Groth16 (three modes)\n')
        f.write('\\begin{table}[htbp]\n')
        f.write('\\centering\n')
        f.write('\\caption{UVC vs.\\ Groth16 baselines at step $j=B$: proving time.}\n')
        f.write('\\label{tab:comparison}\n')
        f.write('\\scriptsize\n')
        f.write('\\begin{tabular}{llrrrrrr}\n')
        f.write('\\toprule\n')
        f.write('Circuit & $n$ & $B$ & UVC prove & G16 on-demand & G16 on-demand & '
                'G16 fixed-CRS & G16 single-step \\\\\n')
        f.write(' & & & (ms) & setup+prove (ms) & prove-only (ms) & prove (ms) & prove (ms) \\\\\n')
        f.write('\\midrule\n')

        for (c, n, B) in configs:
            uvc = data["uvc"][(c, n, B, B)]
            g = {m: g16_row(data, m, c, n, B, B) for m in GROTH16_MODES}
            cell = lambda r, k: fmt_ms(r[k]) if r else "---"
            od_total = ((float(g["ondemand"]["setup_ms"]) + float(g["ondemand"]["prove_ms"]))
                        if g["ondemand"] else None)
            f.write(" & ".join([
                f"\\texttt{{{c}}}", str(n), str(B),
                fmt_ms(uvc["prove_ms"]),
                fmt_ms(str(od_total)) if od_total is not None else "---",
                cell(g["ondemand"], "prove_ms"),
                cell(g["fixedcrs"], "prove_ms"),
                cell(g["singlestep"], "prove_ms"),
            ]) + " \\\\\n")

        f.write('\\bottomrule\n')
        f.write('\\end{tabular}\n')
        f.write('\\end{table}\n')

    print(f"  Comparison table written to {output_path}")


def gen_nova_comparison_table(data, output_path):
    """UVC vs Nova: per-step medians with IQR, plus both cumulative series."""
    configs = uvc_configs(data)

    with open(output_path, 'w') as f:
        f.write('% Auto-generated comparison table: UVC vs Nova\n')
        f.write('\\begin{table}[htbp]\n')
        f.write('\\centering\n')
        f.write('\\caption{UVC vs.\\ Nova IVC: per-step medians (IQR across runs) and '
                'cumulative cost at $j=B$.}\n')
        f.write('\\label{tab:nova-comparison}\n')
        f.write('\\scriptsize\n')
        f.write('\\begin{tabular}{llrrrrrrr}\n')
        f.write('\\toprule\n')
        f.write(' & & & \\multicolumn{2}{c}{UVC} & \\multicolumn{3}{c}{Nova} & \\\\\n')
        f.write('\\cmidrule(lr){4-5} \\cmidrule(lr){6-8}\n')
        f.write('Circuit & $n$ & Step $j$ & Prove (ms) & Verify (ms) & '
                'Fold (ms, IQR) & Compress (ms, IQR) & Verify (ms) & '
                'Cumulative at $B$ (ms) \\\\\n')
        f.write('\\midrule\n')

        for (circuit, n, B) in configs:
            first = True
            for step in bench_data.uvc_steps(data, circuit, n, B):
                nova = data["nova"].get((circuit, n, step))
                if not nova:
                    continue
                r = data["uvc"][(circuit, n, B, step)]
                circ_col = f'\\texttt{{{circuit}}}' if first else ''
                n_col = str(n) if first else ''
                f.write(f'{circ_col} & {n_col} & {step} & '
                        f'{fmt_ms(r["prove_ms"])} & {fmt_ms(r["verify_ms"])} & '
                        f'{fmt_ms(nova.get("fold_ms"))} ({fmt_ms(nova.get("fold_ms_iqr"))}) & '
                        f'{fmt_ms(nova.get("compress_ms"))} ({fmt_ms(nova.get("compress_ms_iqr"))}) & '
                        f'{fmt_ms(nova.get("verify_ms"))} & \\\\\n')
                first = False

            if first:
                continue
            fold_cum = bench_data.cumulative_nova(data, "foldonly", circuit, n, B)
            comp_cum = bench_data.cumulative_nova(data, "compressed", circuit, n, B)
            f.write('Nova (fold-only) & & & & & & & & '
                    + (fmt_ms(str(fold_cum)) if fold_cum is not None else '---') + ' \\\\\n')
            f.write('Nova (compressed) & & & & & & & & '
                    + (fmt_ms(str(comp_cum)) if comp_cum is not None else '---') + ' \\\\\n')
            f.write('\\midrule\n')

        f.write('\\bottomrule\n')
        f.write('\\end{tabular}\n')
        f.write('\\end{table}\n')

    print(f"  Nova comparison table written to {output_path}")


def gen_crossover_analysis(data, output_path):
    """Crossover step j* where UVC cumulative < Groth16 cumulative, per mode."""
    configs = uvc_configs(data)

    with open(output_path, 'w') as f:
        f.write('% Crossover analysis: step j* where UVC cumulative cost < Groth16 cumulative\n')
        f.write('% ' + bench_data.ESTIMATOR_NOTE + '\n')
        f.write('% on-demand uses the proving-only Groth16 series (proving_only=True)\n\n')

        for (circuit, n, B) in configs:
            f.write(f'% Circuit: {circuit}, n={n}, B={B}\n')
            steps = bench_data.measured_steps(B)

            for mode in GROTH16_MODES:
                proving_only = (mode == "ondemand")
                crossover_step = None
                for step in steps:
                    uvc_cum = bench_data.cumulative_uvc(data, circuit, n, B, step)
                    g16_cum = bench_data.cumulative_groth16(
                        data, mode, circuit, n, B, step, proving_only=proving_only)
                    if g16_cum is None:
                        continue
                    marker = ''
                    if uvc_cum < g16_cum and crossover_step is None:
                        crossover_step = step
                        marker = ' <-- CROSSOVER'
                    f.write(f'%   [{mode}] j={step}: UVC_cumul={uvc_cum:.1f}ms, '
                            f'G16_cumul={g16_cum:.1f}ms{marker}\n')
                if crossover_step:
                    f.write(f'%   -> [{mode}] crossover at j*={crossover_step}\n')
                else:
                    f.write(f'%   -> [{mode}] no crossover within measured steps\n')
            f.write('\n')

    print(f"  Crossover analysis written to {output_path}")


def gen_setup_comparison_table(data, output_path):
    """Setup cost across UVC, the three Groth16 modes, and Nova."""
    configs = uvc_configs(data)

    with open(output_path, 'w') as f:
        f.write('% Auto-generated setup cost comparison\n')
        f.write('\\begin{table}[htbp]\n')
        f.write('\\centering\n')
        f.write('\\caption{Setup cost comparison across UVC, the three Groth16 modes, and Nova.}\n')
        f.write('\\label{tab:setup-comparison}\n')
        f.write('\\scriptsize\n')
        f.write('\\begin{tabular}{llrrrrr}\n')
        f.write('\\toprule\n')
        f.write(' & & \\multicolumn{2}{c}{UVC} & \\multicolumn{3}{c}{Groth16} & Nova \\\\\n')
        f.write('\\cmidrule(lr){3-4} \\cmidrule(lr){5-7}\n')
        f.write('Circuit & $n$ & $B$ & Setup (ms) & On-demand at $j{=}1$ (ms) & '
                'Fixed-CRS ($C_B$) (ms) & Single-step (ms) & Setup (ms) \\\\\n')
        f.write('\\midrule\n')

        prev_cn = None
        for (circuit, n, B) in configs:
            r = data["uvc"][(circuit, n, B, 1)]
            od = g16_row(data, "ondemand", circuit, n, B, 1)
            fx = g16_row(data, "fixedcrs", circuit, n, B, 1)
            ss = g16_row(data, "singlestep", circuit, n, B, 1)
            nova = data["nova"].get((circuit, n, 1))

            first = (circuit, n) != prev_cn
            prev_cn = (circuit, n)
            circ_col = f'\\texttt{{{circuit}}}' if first else ''
            n_col = str(n) if first else ''

            f.write(f'{circ_col} & {n_col} & {B} & {fmt_ms(r["setup_ms"])} & '
                    f'{fmt_ms(od["setup_ms"]) if od else "---"} & '
                    f'{fmt_ms(fx["setup_ms"]) if fx else "---"} & '
                    f'{fmt_ms(ss["setup_ms"]) if ss else "---"} & '
                    f'{fmt_ms(nova.get("setup_ms")) if nova else "---"} \\\\\n')

        f.write('\\bottomrule\n')
        f.write('\\end{tabular}\n')
        f.write('\\end{table}\n')

    print(f"  Setup comparison table written to {output_path}")


def main():
    parser = argparse.ArgumentParser(description='Generate LaTeX tables from benchmark CSVs')
    parser.add_argument('--input-dir', required=True, help='Directory with CSV files')
    parser.add_argument('--output-dir', default='.', help='Output directory for .tex files')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    data = bench_data.load_run(args.input_dir)

    has_uvc = bool(data["uvc"])
    has_g16 = bool(data["g16_modes"]) or bool(data["g16_legacy"])
    has_nova = bool(data["nova"])

    if has_uvc:
        gen_uvc_table(data, os.path.join(args.output_dir, 'table_uvc.tex'))

    if has_uvc and has_g16:
        gen_comparison_table(data, os.path.join(args.output_dir, 'table_comparison.tex'))
        gen_crossover_analysis(data, os.path.join(args.output_dir, 'crossover_analysis.tex'))

    if has_uvc and has_nova:
        gen_nova_comparison_table(data, os.path.join(args.output_dir, 'table_nova_comparison.tex'))

    if has_uvc and has_g16 and has_nova:
        gen_setup_comparison_table(data, os.path.join(args.output_dir, 'table_setup_comparison.tex'))

    if not has_uvc and not has_g16:
        print("No CSV data found. Run benchmarks first.")
        sys.exit(1)


if __name__ == '__main__':
    main()
