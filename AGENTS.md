<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# Dev (libsnark-uvc)

## Purpose
Fork of [libsnark](https://github.com/scipr-lab/libsnark) extended with two new preprocessing zkSNARK proof systems: `r1cs_vc_ppzksnark` (Verifiable Computation based on Groth16 without zero-knowledge) and `r1cs_uvc_ppzksnark` (Updatable VC with linearly updatable proofs). Built with CMake targeting C++11.

## Key Files

| File | Description |
|------|-------------|
| `CMakeLists.txt` | Build configuration: curves, OpenMP, architecture detection (Apple Silicon support) |
| `.gitmodules` | Git submodule references for dependencies (libff, libfqfft, etc.) |
| `LICENSE` | MIT license |
| `README.md` | Original libsnark README |

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `src/libsnark/` | Main source code: proof systems, gadgets, relations, reductions (see `src/libsnark/AGENTS.md`) |
| `benchmark-results/` | Benchmark data, analysis scripts, and LaTeX table outputs (see `benchmark-results/AGENTS.md`) |
| `scripts/` | Shell/Python scripts for running benchmarks and summarizing results (see `scripts/AGENTS.md`) |
| `nova-bench/` | Rust-based Nova proof system benchmarks for comparison (see `nova-bench/AGENTS.md`) |
| `depends/` | External dependencies as git submodules (libff, libfqfft, gtest, ate-pairing, xbyak) |
| `results/` | Timestamped benchmark run outputs (logs, system info, LaTeX tables) |
| `tinyram_examples/` | Example TinyRAM programs for testing RAM-based proof systems |

## For AI Agents

### Working In This Directory
- **Build system**: CMake with architecture-specific defaults (ALT_BN128 on ARM64, BN128 on x86)
- **Custom code lives in**: `src/libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/` and `r1cs_uvc_ppzksnark/`
- Everything else under `src/libsnark/` is upstream libsnark code -- avoid modifying unless necessary
- Apple Silicon (arm64) is supported with special CMake flags

### Build Commands
```bash
mkdir -p build && cd build
cmake .. -DCURVE=ALT_BN128 -DWITH_PROCPS=OFF -DWITH_SUPERCOP=OFF -DUSE_ASM=OFF
make -j$(nproc)
```

### Testing Requirements
```bash
cd build
make check        # Build and run all tests
ctest             # Run tests
# Key UVC-specific tests:
make test_r1cs_uvc_ppzksnark_test && ./test_r1cs_uvc_ppzksnark_test
make test_uvc_prover_verifier && ./test_uvc_prover_verifier
make bench_comparative && ./bench_comparative
```

### Common Patterns
- Header-only template implementations: declarations in `.hpp`, implementations in `.tcc`
- Parameterized by elliptic curve via `ppT` template parameter
- Uses libff field/curve types (`Fr<ppT>`, `G1<ppT>`, `G2<ppT>`)

## Dependencies

### External
- `libff` - Finite field arithmetic
- `libfqfft` - FFT for polynomial operations (QAP/SSP/SAP reductions)
- `GMP` / `GMPXX` - Arbitrary precision arithmetic
- `OpenSSL` (libcrypto) - Cryptographic primitives
- `Boost` (program_options) - CLI argument parsing
- `gtest` - Unit testing framework

<!-- MANUAL: -->
