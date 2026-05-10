<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# benchmark-results

## Purpose
Processed benchmark data and analysis outputs. Contains raw CSV results from benchmark runs, Python analysis scripts for generating LaTeX tables, and the resulting table outputs used in the paper.

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `csv/` | Raw benchmark CSV data files |
| `scripts/` | Python scripts for parsing and analyzing benchmark results |
| `tables/` | Generated LaTeX table outputs for inclusion in the paper |

## For AI Agents

### Working In This Directory
- The pipeline flows: `Dev/scripts/` runs benchmarks → `Dev/results/` stores raw logs → `benchmark-results/scripts/` processes into CSV/tables
- LaTeX tables in `tables/` are auto-generated; edit the scripts, not the tables directly
- CSV data in `csv/` corresponds to specific benchmark runs in `../results/`

<!-- MANUAL: -->
