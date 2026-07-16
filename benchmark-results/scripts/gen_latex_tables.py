#!/usr/bin/env python3
"""
Generate LaTeX table fragments from benchmark CSV files.

Produces tables matching the JISA paper Tables 2 and 3 format,
plus a crossover analysis showing at which step j UVC beats Groth16.

Usage:
    python gen_latex_tables.py --input-dir ../csv --output-dir ../tables
"""

import argparse
import csv
import os
import sys
from collections import defaultdict


def read_csv(filepath):
    if not os.path.exists(filepath):
        print(f"  Warning: {filepath} not found, skipping.")
        return []
    with open(filepath, 'r') as f:
        return list(csv.DictReader(f))


def fmt_ms(val_str):
    """Format millisecond value for LaTeX."""
    if not val_str:
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
    if not val_str:
        return '--'
    v = int(val_str)
    if v < 1000:
        return str(v)
    elif v < 1000000:
        return f'{v/1000:.1f}k'
    else:
        return f'{v/1000000:.1f}M'


def dedup_groth16(rows):
    """Deduplicate Groth16 rows."""
    seen = {}
    for r in rows:
        key = (r['circuit'], r['n'], r['step'])
        if key not in seen:
            seen[key] = r
    return list(seen.values())


def gen_uvc_table(uvc_rows, output_path):
    """Generate Table 2: UVC performance across labeled schemes and B values."""
    groups = defaultdict(list)
    for r in uvc_rows:
        key = (r.get('display_scheme', 'UVC (pre-fix)'), r['circuit'], int(r['n']), int(r['B']))
        groups[key].append(r)

    with open(output_path, 'w') as f:
        f.write('% Auto-generated UVC benchmark table\n')
        f.write('\\begin{table}[htbp]\n')
        f.write('\\centering\n')
        f.write('\\caption{UVC scheme performance: setup, prove, and verify times.}\n')
        f.write('\\label{tab:uvc-bench}\n')
        f.write('\\scriptsize\n')
        f.write('\\begin{tabular}{lllrrrrrrr}\n')
        f.write('\\toprule\n')
        f.write('Scheme & Circuit & $n$ & $B$ & Step $j$ & Setup (ms) & Prove (ms) & Verify (ms) & CRS $|\\mathbb{G}_1|$ & Proof (B) \\\\\n')
        f.write('\\midrule\n')

        for key in sorted(groups.keys()):
            scheme, circuit, n, B = key
            rows = sorted(groups[key], key=lambda r: int(r['step']))
            first = True
            for r in rows:
                scheme_col = scheme if first else ''
                circ_col = f'\\texttt{{{circuit}}}' if first else ''
                n_col = str(n) if first else ''
                b_col = str(B) if first else ''
                f.write(f'{scheme_col} & {circ_col} & {n_col} & {b_col} & {r["step"]} & '
                        f'{fmt_ms(r["setup_ms"])} & {fmt_ms(r["prove_ms"])} & '
                        f'{fmt_ms(r["verify_ms"])} & {fmt_size(r.get("crs_g1", r.get("crs_g1_published", "")))} & '
                        f'{r.get("proof_bytes", "--")} \\\\\n')
                first = False
            f.write('\\midrule\n')

        f.write('\\bottomrule\n')
        f.write('\\end{tabular}\n')
        f.write('\\end{table}\n')

    print(f"  UVC table written to {output_path}")


def gen_comparison_table(uvc_rows, g16_rows, output_path):
    """Generate Table 3: UVC vs Groth16 comparison with speedup."""
    # Group UVC by (circuit, n, B) to show each B separately
    uvc_groups = defaultdict(list)
    for r in uvc_rows:
        key = (r['circuit'], int(r['n']), int(r['B']))
        uvc_groups[key].append(r)

    g16_by_key = {}
    for r in g16_rows:
        key = (r['circuit'], r['n'], r['step'])
        g16_by_key[key] = r

    with open(output_path, 'w') as f:
        f.write('% Auto-generated comparison table: UVC vs Groth16\n')
        f.write('\\begin{table}[htbp]\n')
        f.write('\\centering\n')
        f.write('\\caption{UVC vs.\\ Groth16 re-prove: proving time comparison.}\n')
        f.write('\\label{tab:comparison}\n')
        f.write('\\scriptsize\n')
        f.write('\\begin{tabular}{llrrrrrr}\n')
        f.write('\\toprule\n')
        f.write('Circuit & $n$ & $B$ & Step $j$ & UVC Prove (ms) & G16 Setup+Prove (ms) & Speedup \\\\\n')
        f.write('\\midrule\n')

        for uvc_key in sorted(uvc_groups.keys()):
            circuit, n, B = uvc_key
            rows = sorted(uvc_groups[uvc_key], key=lambda r: int(r['step']))
            first = True

            for r in rows:
                step = r['step']
                g16_key = (circuit, str(n), step)
                if g16_key not in g16_by_key:
                    continue

                g16 = g16_by_key[g16_key]
                uvc_prove = float(r['prove_ms'])
                g16_setup = float(g16['setup_ms'])
                g16_prove = float(g16['prove_ms'])
                g16_total = g16_setup + g16_prove
                speedup = g16_total / uvc_prove if uvc_prove > 0 else float('inf')

                circ_col = f'\\texttt{{{circuit}}}' if first else ''
                n_col = str(n) if first else ''
                b_col = str(B) if first else ''

                f.write(f'{circ_col} & {n_col} & {b_col} & {step} & '
                        f'{fmt_ms(r["prove_ms"])} & {fmt_ms(str(g16_total))} & '
                        f'{speedup:.1f}$\\times$ \\\\\n')
                first = False

            if not first:
                f.write('\\midrule\n')

        f.write('\\bottomrule\n')
        f.write('\\end{tabular}\n')
        f.write('\\end{table}\n')

    print(f"  Comparison table written to {output_path}")


def gen_nova_comparison_table(uvc_rows, nova_rows, output_path):
    """Generate table comparing UVC vs Nova folding scheme."""
    # Use smallest B for UVC (tightest comparison)
    uvc_groups = defaultdict(list)
    for r in uvc_rows:
        key = (r['circuit'], int(r['n']), int(r['B']))
        uvc_groups[key].append(r)

    # Nova keyed by (circuit, n, step) — keep last entry for dedup
    nova_by_key = {}
    for r in nova_rows:
        key = (r['circuit'], r['n'], r['step'])
        nova_by_key[key] = r

    # Find the smallest B for each (circuit, n) in UVC
    smallest_B = {}
    for (circuit, n, B) in uvc_groups.keys():
        cn_key = (circuit, n)
        if cn_key not in smallest_B or B < smallest_B[cn_key]:
            smallest_B[cn_key] = B

    with open(output_path, 'w') as f:
        f.write('% Auto-generated comparison table: UVC vs Nova\n')
        f.write('\\begin{table}[htbp]\n')
        f.write('\\centering\n')
        f.write('\\caption{UVC vs.\\ Nova IVC: per-step and cumulative cost comparison.}\n')
        f.write('\\label{tab:nova-comparison}\n')
        f.write('\\scriptsize\n')
        f.write('\\begin{tabular}{llrrrrrrr}\n')
        f.write('\\toprule\n')
        f.write(' & & & \\multicolumn{2}{c}{UVC} & \\multicolumn{3}{c}{Nova} \\\\\n')
        f.write('\\cmidrule(lr){4-5} \\cmidrule(lr){6-8}\n')
        f.write('Circuit & $n$ & Step $j$ & Prove (ms) & Verify (ms) & Fold (ms) & Compress (ms) & Verify (ms) \\\\\n')
        f.write('\\midrule\n')

        for cn_key in sorted(smallest_B.keys()):
            circuit, n = cn_key
            B = smallest_B[cn_key]
            uvc_key = (circuit, n, B)
            rows = sorted(uvc_groups[uvc_key], key=lambda r: int(r['step']))

            first = True
            for r in rows:
                step = r['step']
                nova_key = (circuit, str(n), step)
                if nova_key not in nova_by_key:
                    continue

                nova = nova_by_key[nova_key]
                circ_col = f'\\texttt{{{circuit}}}' if first else ''
                n_col = str(n) if first else ''

                f.write(f'{circ_col} & {n_col} & {step} & '
                        f'{fmt_ms(r["prove_ms"])} & {fmt_ms(r["verify_ms"])} & '
                        f'{fmt_ms(nova.get("fold_ms", ""))} & '
                        f'{fmt_ms(nova.get("compress_ms", ""))} & '
                        f'{fmt_ms(nova.get("verify_ms", ""))} \\\\\n')
                first = False

            if not first:
                f.write('\\midrule\n')

        f.write('\\bottomrule\n')
        f.write('\\end{tabular}\n')
        f.write('\\end{table}\n')

    print(f"  Nova comparison table written to {output_path}")


def gen_crossover_analysis(uvc_rows, g16_rows, output_path):
    """Analyze at which step j UVC cumulative prove time < Groth16 re-prove."""
    uvc_groups = defaultdict(list)
    for r in uvc_rows:
        key = (r['circuit'], r['n'], r['B'])
        uvc_groups[key].append(r)

    g16_by_key = {}
    for r in g16_rows:
        key = (r['circuit'], r['n'], r['step'])
        g16_by_key[key] = r

    with open(output_path, 'w') as f:
        f.write('% Crossover analysis: step j* where UVC cumulative cost < Groth16 re-prove\n')
        f.write('% UVC cost at step j: setup + sum of prove times for steps 1..j\n')
        f.write('% Groth16 cost at step j: setup(C_j) + prove(C_j)\n\n')

        for key in sorted(uvc_groups.keys()):
            circuit, n, B = key
            rows = sorted(uvc_groups[key], key=lambda r: int(r['step']))

            f.write(f'% Circuit: {circuit}, n={n}, B={B}\n')

            uvc_setup = float(rows[0]['setup_ms'])
            uvc_cumulative = uvc_setup  # One-time setup cost

            crossover_step = None
            for r in rows:
                step = r['step']
                uvc_prove = float(r['prove_ms'])
                uvc_cumulative += uvc_prove

                g16_key = (circuit, n, step)
                if g16_key in g16_by_key:
                    g16 = g16_by_key[g16_key]
                    g16_total = float(g16['setup_ms']) + float(g16['prove_ms'])

                    marker = ''
                    if uvc_cumulative < g16_total and crossover_step is None:
                        crossover_step = step
                        marker = ' <-- CROSSOVER'

                    f.write(f'%   j={step}: UVC_cumul={uvc_cumulative:.1f}ms, '
                            f'G16_total={g16_total:.1f}ms{marker}\n')

            if crossover_step:
                f.write(f'%   -> Crossover at j*={crossover_step}\n')
            else:
                f.write(f'%   -> No crossover within measured steps\n')
            f.write('\n')

    print(f"  Crossover analysis written to {output_path}")


def gen_setup_comparison_table(uvc_rows, g16_rows, nova_rows, output_path):
    """Generate table comparing setup costs across all three systems."""
    # Get unique (circuit, n) combinations
    circuits = set()
    for r in uvc_rows:
        circuits.add((r['circuit'], int(r['n'])))

    uvc_by_cn = defaultdict(dict)
    for r in uvc_rows:
        cn = (r['circuit'], int(r['n']))
        B = int(r['B'])
        uvc_by_cn[cn][B] = r

    g16_by_cn = defaultdict(dict)
    for r in g16_rows:
        cn = (r['circuit'], int(r['n']))
        step = int(r['step'])
        g16_by_cn[cn][step] = r

    # Nova: one setup per (circuit, n)
    nova_by_cn = {}
    for r in nova_rows:
        cn = (r['circuit'], int(r['n']))
        nova_by_cn[cn] = r

    with open(output_path, 'w') as f:
        f.write('% Auto-generated setup cost comparison\n')
        f.write('\\begin{table}[htbp]\n')
        f.write('\\centering\n')
        f.write('\\caption{Setup cost comparison across UVC, Groth16, and Nova.}\n')
        f.write('\\label{tab:setup-comparison}\n')
        f.write('\\scriptsize\n')
        f.write('\\begin{tabular}{llrrrrr}\n')
        f.write('\\toprule\n')
        f.write(' & & \\multicolumn{2}{c}{UVC} & Groth16 & Nova \\\\\n')
        f.write('\\cmidrule(lr){3-4}\n')
        f.write('Circuit & $n$ & $B$ & Setup (ms) & Setup at $j{=}1$ (ms) & Setup (ms) \\\\\n')
        f.write('\\midrule\n')

        for cn in sorted(circuits):
            circuit, n = cn
            b_dict = uvc_by_cn.get(cn, {})
            nova = nova_by_cn.get(cn, {})
            g16_step1 = g16_by_cn.get(cn, {}).get(1, {})

            first = True
            for B in sorted(b_dict.keys()):
                r = b_dict[B]
                circ_col = f'\\texttt{{{circuit}}}' if first else ''
                n_col = str(n) if first else ''
                g16_col = fmt_ms(g16_step1.get('setup_ms', '')) if first else ''
                nova_col = fmt_ms(nova.get('setup_ms', '')) if first else ''

                f.write(f'{circ_col} & {n_col} & {B} & {fmt_ms(r["setup_ms"])} & '
                        f'{g16_col} & {nova_col} \\\\\n')
                first = False

            f.write('\\midrule\n')

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

    uvc_rows = read_csv(os.path.join(args.input_dir, 'uvc_results.csv'))
    for row in uvc_rows:
        row['display_scheme'] = 'UVC (pre-fix)'
    uvc_bound_rows = read_csv(os.path.join(args.input_dir, 'uvc_bound_results.csv'))
    for row in uvc_bound_rows:
        row['display_scheme'] = 'UVC (state-bound)'
    uvc_rows.extend(uvc_bound_rows)
    g16_rows = read_csv(os.path.join(args.input_dir, 'groth16_results.csv'))
    nova_rows = read_csv(os.path.join(args.input_dir, 'nova_results.csv'))

    # Deduplicate Groth16
    g16_rows = dedup_groth16(g16_rows)

    if uvc_rows:
        gen_uvc_table(uvc_rows, os.path.join(args.output_dir, 'table_uvc.tex'))

    if uvc_rows and g16_rows:
        gen_comparison_table(uvc_rows, g16_rows, os.path.join(args.output_dir, 'table_comparison.tex'))
        gen_crossover_analysis(uvc_rows, g16_rows, os.path.join(args.output_dir, 'crossover_analysis.tex'))

    if uvc_rows and nova_rows:
        gen_nova_comparison_table(uvc_rows, nova_rows, os.path.join(args.output_dir, 'table_nova_comparison.tex'))

    if uvc_rows and g16_rows and nova_rows:
        gen_setup_comparison_table(uvc_rows, g16_rows, nova_rows, os.path.join(args.output_dir, 'table_setup_comparison.tex'))

    if not uvc_rows and not g16_rows:
        print("No CSV data found. Run benchmarks first.")
        sys.exit(1)


if __name__ == '__main__':
    main()
