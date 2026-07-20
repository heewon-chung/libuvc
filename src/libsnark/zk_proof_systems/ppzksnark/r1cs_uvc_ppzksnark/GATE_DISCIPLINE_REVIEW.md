<!-- AC8 gate-discipline manual review note (user-approved resolution path). -->
<!-- Reviewed at commit base c65120d during the eta->gamma single-mode migration. -->

# Gate-Discipline Review: State-Wire Linear Independence (AC8)

The committed-state soundness argument assumes the state-output polynomials
{L_i}_{i in I_j^st} are linearly independent. This is guaranteed by the
**distinct-output-pivot discipline**: every state-output wire must be the
output (C-side pivot) of its own dedicated R1CS gate, and no two state
wires may share a gate output.

No automated checker for this discipline exists in this repository (only
the wire-track disjointness checker, `uvc_check_track_disjointness` in
`r1cs_uvc_ppzksnark_params.hpp`, which verifies track membership, not gate
pivots). Per the approved plan, AC8 is satisfied by this documented manual
review instead of a new validator.

## Per-workload review

| Workload | Circuit | state_size | State-output wires | Producing gate(s) | Verdict |
|---|---|---|---|---|---|
| Sensor fusion (SF, K sensors) | `benchmarks/circuits/sensor_fusion_circuit.hpp` | 1 | `s_out` = wire K+3 | C-side output of constraint K+1 only (`one_t * (sum p_k + beta*s_in) = s_out`) | PASS |
| Hadamard product (d dims) | `benchmarks/circuits/hadamard_circuit.hpp` | d | `s_out[i]` = wire 2d+1+i | Each is the C-side output of its own constraint i (`s_in[i] * t[i] = s_out[i]`); d distinct gates for d state wires | PASS |
| MiMC hash chain (r rounds) | `benchmarks/circuits/mimc_circuit.hpp` | 1 | `s_out` = wire 3 | C-side output of the final round's cube constraint 3r only (`w_sq_r * w_mix_r = s_out`) | PASS |
| Repeated squaring (n constraints) | `benchmarks/circuits/scalable_circuit.hpp` | 1 | `s_out` = wire 3 | C-side output of the final squaring constraint only (`w_{n-2}^2 = s_out`) | PASS |

## Composition argument

`build_composed_constraint_system` maps each step's state-output wires to
fresh composed wires at `(k-1)*new_per_step + ss + ts + 1 .. + ss`
(`uvc_state_output_indices`), and each mapped constraint keeps its C-side
pivot on that fresh wire. Therefore across all steps j = 1..B, every wire
in I^st is the unique output pivot of a distinct gate of C_B, and the
Lagrange contributions {w_i} (hence {L_i}) for i in I^st are linearly
independent as required.

Any NEW workload circuit added to the benchmark suite must be re-reviewed
against this discipline (each state-output wire = C-side output of its own
gate) and this table extended before its measurements are published.
