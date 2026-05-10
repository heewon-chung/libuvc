<!-- Parent: ../../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# libsnark (Source Code)

## Purpose
Core C++ source for libsnark -- a library for constructing and verifying zero-knowledge proofs using preprocessing zkSNARKs. This fork adds `r1cs_vc_ppzksnark` (non-ZK Groth16 variant for Verifiable Computation) and `r1cs_uvc_ppzksnark` (Updatable VC with linearly updatable proofs).

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `zk_proof_systems/` | All proof system implementations: ppzkSNARK, PCD, zkSNARK (see `zk_proof_systems/AGENTS.md`) |
| `relations/` | Constraint system and arithmetic program definitions (R1CS, QAP, SAP, SSP, BACS, TBCS, USCS) |
| `reductions/` | Reductions between representations (R1CS→QAP, R1CS→SAP, BACS→R1CS, TBCS→USCS, RAM→R1CS) |
| `gadgetlib1/` | First-generation gadget library: hash gadgets, Merkle trees, verifier circuits, CPU checkers |
| `gadgetlib2/` | Second-generation gadget library: cleaner API with protoboard, variables, constraints |
| `common/` | Shared utilities: data structures, routing algorithms, default type definitions |
| `knowledge_commitment/` | Knowledge commitment scheme types (KC multiexp, KC elements) |

## For AI Agents

### Working In This Directory
- **Custom code (modify freely)**: `zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/` and `r1cs_uvc_ppzksnark/`
- **Upstream code (avoid modifying)**: Everything else is from the original libsnark repo
- Template pattern: `.hpp` for declarations, `.tcc` for template implementations (included at bottom of `.hpp`)
- All proof systems are parameterized by `ppT` (pairing parameters template)

### Key Type Hierarchy
- `r1cs_constraint_system<FieldT>` -- constraint systems (in `relations/`)
- `qap_instance<FieldT>` -- QAP polynomials from R1CS reduction (in `relations/arithmetic_programs/`)
- `r1cs_vc_ppzksnark_*<ppT>` -- VC proof system types (custom)
- `r1cs_uvc_ppzksnark_*<ppT>` -- UVC proof system types (custom)

<!-- MANUAL: -->
