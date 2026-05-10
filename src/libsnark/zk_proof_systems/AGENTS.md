<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# zk_proof_systems

## Purpose
All zero-knowledge proof system implementations. Contains preprocessing zkSNARKs (ppzkSNARK), proof-carrying data (PCD), authenticated data structure SNARKs (ppzkadSNARK), and plain zkSNARKs. The custom VC and UVC proof systems live under `ppzksnark/`.

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `ppzksnark/` | Preprocessing zkSNARKs: Groth16 variants, BACS, TBCS, USCS, RAM, **VC**, **UVC** (see `ppzksnark/AGENTS.md`) |
| `pcd/` | Proof-Carrying Data: multi-predicate and single-predicate R1CS PCD (upstream) |
| `ppzkadsnark/` | Authenticated data structure SNARKs with signature/PRF examples (upstream) |
| `zksnark/` | Plain (non-preprocessing) zkSNARKs for RAM computations (upstream) |

## For AI Agents

### Working In This Directory
- **Custom code**: Only `ppzksnark/r1cs_vc_ppzksnark/` and `ppzksnark/r1cs_uvc_ppzksnark/` are custom
- All other directories are upstream libsnark -- avoid modifying
- The `ppzksnark/README.md` explains the naming convention and proof system taxonomy

<!-- MANUAL: -->
