<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# r1cs_vc_ppzksnark (Verifiable Computation)

## Purpose
**[CUSTOM]** Non-zero-knowledge Groth16 variant for Verifiable Computation. This is the base VC scheme: it removes the randomization from Groth16 so that proof components (A, B, C group elements) are deterministic functions of the witness. This determinism is what enables the linear updatability in the UVC extension.

## Key Files

| File | Description |
|------|-------------|
| `r1cs_vc_ppzksnark.hpp` | Type declarations: proving key, verification key, keypair, proof |
| `r1cs_vc_ppzksnark.tcc` | Template implementations: generator, prover, verifier algorithms |
| `r1cs_vc_ppzksnark_params.hpp` | Type aliases parameterized by pairing type `ppT` |
| `tests/test_r1cs_vc_ppzksnark.cpp` | Unit test: end-to-end generate → prove → verify |

## For AI Agents

### Working In This Directory
- This is the **base layer** that `r1cs_uvc_ppzksnark` builds upon
- Key difference from Groth16 (`r1cs_gg_ppzksnark`): no random blinding factors (delta, r, s)
- Proof structure: `(A, B, C)` where `A = Σ a_i · [u_i(x)]_1`, `B = Σ a_i · [v_i(x)]_2`, etc.
- The prover is deterministic given the same witness -- this is essential for updatability

### Testing Requirements
```bash
cd build && make zk_proof_systems_r1cs_vc_ppzksnark_test && ctest -R r1cs_vc
```

### Architecture
- `generator(cs)` → `(pk, vk)` -- generates proving/verification keys from R1CS
- `prover(pk, primary, auxiliary)` → `proof` -- produces proof from witness
- `verifier(vk, primary, proof)` → `bool` -- verifies proof against public input

<!-- MANUAL: -->
