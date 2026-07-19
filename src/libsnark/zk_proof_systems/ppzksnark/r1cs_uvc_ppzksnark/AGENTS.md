<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-07-19 -->

# r1cs_uvc_ppzksnark (Updatable Verifiable Computation)

## Purpose
**[CUSTOM -- CORE CONTRIBUTION]** Updatable Verifiable Computation proof system for bounded state-transition compositions. It binds each proof to its composition step and state evolution while reusing a preprocessed CRS.

## Key Files

| File | Description |
|------|-------------|
| `r1cs_uvc_ppzksnark.hpp` | Type declarations for proving keys, verification keys, proofs, and state-transition circuits |
| `r1cs_uvc_ppzksnark.tcc` | Template implementations of the generator, prover, and state-bound verifier |
| `r1cs_uvc_ppzksnark_params.hpp` | Type aliases parameterized by `ppT` |
| `GATE_DISCIPLINE_REVIEW.md` | Review record for verifier gate and pairing discipline |

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `tests/` | Unit and integration tests for setup, composition, prover, verifier, and workload circuits |
| `benchmarks/` | Comparative benchmark harness and circuit definitions |
| `benchmarks/circuits/` | Benchmark circuit implementations (MiMC, PageRank, sensor fusion, and more) |

## For AI Agents

### Working In This Directory
- This is the **primary implementation** of the paper's contribution.
- The generator API is `generator(st, B)`.
- The prover API is `prover(pk, step, primary, auxiliary, prev_proof)`; it performs a zero-extension admissibility preflight and throws `std::invalid_argument` on violation.
- The only verifier API is `verifier(vk, step, primary, reported_s_j, proof, D_prev)`. The caller holds and advances `D_prev`; step 1 requires the genesis guard (`D_prev == 0`).
- State-output scalars are partitioned onto the gamma track: state wires are zero on the delta/L-query track, while `g_D` is accumulated from `vk.st_ABC_g1`. Do not reintroduce a C/D double count.
- `scripts/check_pairing_count.sh` extracts the final verifier function body and enforces the one-Miller-loop, one-double-Miller-loop, one-final-exponentiation pairing discipline.

### Key Test Files

| File | Description |
|------|-------------|
| `tests/test_uvc_prover_verifier.cpp` | Focused state-bound prover/verifier, genesis, step-binding, and pairing-structure tests |
| `tests/test_uvc_generator.cpp` | Key-generation, CRS-layout, and state-track alias tests |
| `tests/test_build_composed_cs.cpp` | Tests for building composed constraint systems (`C_j`) |
| `tests/test_build_composed_assignment.cpp` | Tests for composing assignments across steps |
| `tests/test_complex_circuits.cpp` | Integration tests, including production workload fixtures |

### Benchmark Files

| File | Description |
|------|-------------|
| `benchmarks/bench_comparative.cpp` | Head-to-head UVC versus Groth16 comparison harness |
| `benchmarks/bench_utils.hpp` | Benchmark timing and reporting utilities |
| `benchmarks/circuits/mimc_circuit.hpp` | MiMC hash circuit |
| `benchmarks/circuits/sensor_fusion_circuit.hpp` | IoT sensor fusion circuit |
| `benchmarks/circuits/hadamard_circuit.hpp` | Hadamard product circuit |
| `benchmarks/circuits/scalable_circuit.hpp` | Parametrically scalable circuit |

### Testing Requirements
```bash
cd build
make test_uvc_generator && ./test_uvc_generator
make test_uvc_prover_verifier && ./test_uvc_prover_verifier
make test_build_composed_cs && ./test_build_composed_cs
make test_complex_circuits && ./test_complex_circuits
```

### Architecture
- Setup precomputes a CRS for up to `B` compositions.
- `step_data` materializes only steps `2..B`; step 1 has no delta block.
- Verification consumes the reported state and caller-held previous state commitment in one state-bound check.
