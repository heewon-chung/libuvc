<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# scripts

## Purpose
Benchmark execution and result summarization scripts. Used to run comparative benchmarks (UVC vs Groth16 vs Nova) and process results into paper-ready formats.

## Key Files

| File | Description |
|------|-------------|
| `run_benchmarks.sh` | Main benchmark runner for UVC/Groth16 comparative experiments |
| `run_iot_benchmarks.sh` | IoT-focused benchmarks (sensor fusion, resource-constrained scenarios) |
| `summarize_results.py` | Python script to parse benchmark logs and generate summary tables/CSV |

## For AI Agents

### Working In This Directory
- Scripts assume the project is built in `../build/` with benchmark executables compiled
- `run_benchmarks.sh` calls `bench_comparative` and `bench_r1cs_uvc_ppzksnark` binaries
- `run_iot_benchmarks.sh` is tailored for the IEEE IoT Journal submission experiments
- `summarize_results.py` reads from `../results/` and outputs to `../benchmark-results/`
- All scripts are executable (`chmod +x`)

### Testing Requirements
- Ensure build is complete before running benchmark scripts
- Verify output directory exists before running summarization

<!-- MANUAL: -->
