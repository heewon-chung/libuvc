<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# r1cs_uvc_ppzksnark (Updatable Verifiable Computation)

## Purpose
**[CUSTOM -- CORE CONTRIBUTION]** Updatable Verifiable Computation proof system. Implements the linearly updatable QAP family so that when a circuit composition grows from C_j to C_{j+1}, the proof (A, B, C) can be updated incrementally by adding contributions only from new wire indices (I_{j+1} \ I_j), avoiding full re-computation and recursive proof composition.

## Key Files

| File | Description |
|------|-------------|
| `r1cs_uvc_ppzksnark.hpp` | Type declarations: updatable proving/verification keys, proof, update data |
| `r1cs_uvc_ppzksnark.tcc` | Template implementations: generator, prover, verifier, **update_proof** algorithms |
| `r1cs_uvc_ppzksnark_params.hpp` | Type aliases parameterized by `ppT` |

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `tests/` | Unit tests, integration tests, and benchmarks for UVC proof system |
| `benchmarks/` | Comparative benchmark harness and circuit definitions |
| `benchmarks/circuits/` | Benchmark circuit implementations (MiMC, PageRank, sensor fusion, etc.) |

## For AI Agents

### Working In This Directory
- This is the **primary implementation** of the paper's contribution
- Key algorithm: `update_proof(old_proof, delta_witness, update_key)` → `new_proof`
- The update is linear: `A_j = A_{j-1} + Σ_{new indices} a_i · u_i(x)`
- Proof update cost is proportional to `|I_{j+1} \ I_j|` (new wires only), not total circuit size

### Key Test Files

| File | Description |
|------|-------------|
| `tests/test_r1cs_uvc_ppzksnark.cpp` | Core UVC proof system test (generate → prove → update → verify) |
| `tests/test_uvc_prover_verifier.cpp` | Focused prover/verifier correctness tests |
| `tests/test_uvc_generator.cpp` | Key generation tests |
| `tests/test_build_composed_cs.cpp` | Tests for building composed constraint systems (C_j → C_{j+1}) |
| `tests/test_build_composed_assignment.cpp` | Tests for composing witness assignments across steps |
| `tests/test_complex_circuits.cpp` | Integration tests with real circuit patterns |
| `tests/bench_r1cs_uvc_ppzksnark.cpp` | Performance benchmarks |

### Benchmark Files

| File | Description |
|------|-------------|
| `benchmarks/bench_comparative.cpp` | Head-to-head UVC vs Groth16 comparison harness |
| `benchmarks/bench_utils.hpp` | Benchmark timing and reporting utilities |
| `benchmarks/circuits/mimc_circuit.hpp` | MiMC hash circuit (symmetric crypto benchmark) |
| `benchmarks/circuits/pagerank_circuit.hpp` | PageRank iteration circuit (graph computation) |
| `benchmarks/circuits/sensor_fusion_circuit.hpp` | IoT sensor fusion circuit (IEEE IoT paper) |
| `benchmarks/circuits/hadamard_circuit.hpp` | Hadamard product circuit (linear algebra) |
| `benchmarks/circuits/scalable_circuit.hpp` | Parametrically scalable test circuit |

### Testing Requirements
```bash
cd build
make zk_proof_systems_r1cs_uvc_ppzksnark_test && ctest -R r1cs_uvc
make test_uvc_prover_verifier && ./test_uvc_prover_verifier
make test_build_composed_cs && ./test_build_composed_cs
make bench_comparative && ./bench_comparative
```

### Architecture
- `generator(cs_family)` → `(pk, vk, update_keys)` -- generates keys for the entire QAP family
- `prover(pk, primary, auxiliary)` → `proof` -- initial proof generation
- `update_proof(proof, update_key, delta_primary, delta_auxiliary)` → `updated_proof` -- incremental update
- `verifier(vk, primary, proof)` → `bool` -- verification (same interface as base VC)

<!-- MANUAL: -->
