<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# ppzksnark (Preprocessing zkSNARKs)

## Purpose
Collection of preprocessing zero-knowledge Succinct Non-interactive ARguments of Knowledge. Each subdirectory implements a complete proof system with generator, prover, verifier, and associated types. This project adds two new systems: `r1cs_vc_ppzksnark` and `r1cs_uvc_ppzksnark`.

## Key Files

| File | Description |
|------|-------------|
| `README.md` | Taxonomy of proof systems and naming conventions |

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `r1cs_uvc_ppzksnark/` | **[CUSTOM]** Updatable Verifiable Computation -- linearly updatable proofs (see `r1cs_uvc_ppzksnark/AGENTS.md`) |
| `r1cs_vc_ppzksnark/` | **[CUSTOM]** Verifiable Computation -- Groth16 without ZK (see `r1cs_vc_ppzksnark/AGENTS.md`) |
| `r1cs_gg_ppzksnark/` | Groth16 (Generic Group model) ppzkSNARK for R1CS (upstream) |
| `r1cs_ppzksnark/` | Original PGHR13 ppzkSNARK for R1CS (upstream) |
| `r1cs_se_ppzksnark/` | Simulation-Extractable ppzkSNARK for R1CS (upstream) |
| `bacs_ppzksnark/` | ppzkSNARK for BACS (Bilinear Arithmetic Circuit Satisfiability) (upstream) |
| `tbcs_ppzksnark/` | ppzkSNARK for TBCS (Two-input Boolean Circuit Satisfiability) (upstream) |
| `uscs_ppzksnark/` | ppzkSNARK for USCS (Unitary-Square Constraint System) (upstream) |
| `ram_ppzksnark/` | ppzkSNARK for RAM computations (upstream) |

## For AI Agents

### Working In This Directory
- **Only modify** `r1cs_vc_ppzksnark/` and `r1cs_uvc_ppzksnark/` -- the rest are upstream
- `r1cs_gg_ppzksnark` is the closest upstream relative (Groth16) -- useful for reference
- All proof systems follow the same API pattern: `*_generator`, `*_prover`, `*_verifier` functions
- Each system defines: `*_proving_key`, `*_verification_key`, `*_keypair`, `*_proof` types

### Naming Convention
- `r1cs_` = operates on R1CS constraint systems
- `vc_` = Verifiable Computation (no zero-knowledge)
- `uvc_` = Updatable Verifiable Computation
- `gg_` = Generic Group model (Groth16)
- `ppzk` = preprocessing zero-knowledge
- `snark` = Succinct Non-interactive ARgument of Knowledge

<!-- MANUAL: -->
