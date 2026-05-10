# Implementation Plan: Updatable Verifiable Computation on libsnark

## Requirements Summary

Implement the Updatable Verifiable Computation (UVC) scheme from the paper "Updatable Verifiable Computation without Recursive Proof Compositions" by forking and modifying the libsnark library. The implementation covers:

1. **VC Scheme (Paper Section 4.1)**: A Groth16 variant with randomization removed (no r,s blinding), suitable for verifiable computation (not ZK).
2. **Updatable VC Scheme (Paper Section 4.2)**: Incremental proof update mechanism using the linearly updatable QAP family with shared modulus P_B(X).
3. **Linearly Updatable QAP Family (Paper Section 3.3)**: QAP extension structure enabling linear proof updates across circuit compositions C_1, ..., C_B.

**Curve**: BN128 (alt_bn128, libsnark default)
**Target directory**: `Dev/`

---

## Acceptance Criteria

1. **AC-1**: `VC.Setup(R)` generates a valid CRS (σ₁, σ₂) for a given R1CS/QAP relation, without randomization parameters r, s.
2. **AC-2**: `VC.Prove(σ, φ, w)` produces a proof π = ([A]₁, [B]₂, [C]₁) that passes `VC.Verify(σ, φ, π)` for valid statement/witness pairs.
3. **AC-3**: `VC.Verify` rejects proofs with invalid witnesses (soundness spot-check with ≥10 random invalid inputs).
4. **AC-4**: `UVC.Setup(R^i_ST)` generates a CRS covering all B circuit compositions, with per-step polynomial data {u_i(x), v_i(x), w_i(x)}_{i∈I_j} for j ∈ [1,B].
5. **AC-5**: `UVC.Prove` for j=1 produces a valid proof via `VC.Prove`, and for j>1 produces an incrementally updated proof π_j from π_{j-1} by adding only new wire contributions from I_j \ I_{j-1}.
6. **AC-6**: `UVC.Verify(σ, (t₁, j, s_j), π_j)` correctly verifies updated proofs for all j ∈ [1,B].
7. **AC-7**: Updated proof A_j = A_{j-1} + Σ_{i∈I_j\I_{j-1}} a_i·u_i(x) matches a fresh full proof computation (bitwise equality in group elements).
8. **AC-8**: Prover update cost is O(n) where n = |I_j \ I_{j-1}| (number of new multiplication gates), not O(jn).
9. **AC-9**: All tests pass: unit tests for VC, integration tests for UVC with ≥3 sequential compositions.
10. **AC-10**: Project builds cleanly with CMake on macOS (Apple Silicon).

---

## Implementation Steps

### Phase 0: Project Setup and libsnark Fork

**Step 0.1: Fork libsnark into Dev/**
- Clone `scipr-lab/libsnark` (with `--recursive` for submodules libff, libfqfft) into `Dev/libsnark/`
- Verify the vanilla build works on macOS with BN128: `mkdir build && cd build && cmake .. && make`
- Run existing Groth16 tests to establish baseline: `./libsnark/zk_proof_systems/ppzksnark/r1cs_gg_ppzksnark/tests/test_r1cs_gg_ppzksnark`
- **Files**: `Dev/libsnark/` (entire cloned repo)

**Step 0.2: Create UVC module directory structure**
- Create new module alongside existing Groth16:
  ```
  libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/      # VC (non-ZK Groth16)
  libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/      # Updatable VC
  ```
- Create corresponding test and example directories
- Update `CMakeLists.txt` to include new modules
- **Files to create**:
  - `libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/r1cs_vc_ppzksnark.hpp`
  - `libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/r1cs_vc_ppzksnark.tcc`
  - `libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/r1cs_vc_ppzksnark_params.hpp`
  - `libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.hpp`
  - `libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.tcc`
  - `libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark_params.hpp`

---

### Phase 1: VC Scheme (Paper Section 4.1 — Non-ZK Groth16)

**Step 1.1: Define VC data structures**
- File: `r1cs_vc_ppzksnark.hpp`
- Define `r1cs_vc_ppzksnark_proving_key<ppT>`:
  - `alpha_g1`, `beta_g1`, `beta_g2`, `delta_g1`, `delta_g2` (same as Groth16)
  - `A_query` (G1 elements for u_i(x)), `B_query` (G1 and G2 elements for v_i(x))
  - `H_query` (G1 elements for x^i·P(x)/δ)
  - `L_query` (G1 elements for (β·u_i(x) + α·v_i(x) + w_i(x))/δ for witness indices)
- Define `r1cs_vc_ppzksnark_verification_key<ppT>`:
  - `alpha_g1_beta_g2` (precomputed pairing e(α,β))
  - `gamma_g2`, `delta_g2`
  - `gamma_ABC_g1` (accumulation vector for statement: (β·u_i(x) + α·v_i(x) + w_i(x))/γ for i ∈ I^io)
- Define `r1cs_vc_ppzksnark_proof<ppT>`:
  - `g_A` (G1), `g_B` (G2), `g_C` (G1) — three group elements, no randomization

**Step 1.2: Implement VC.Setup (Key Generation)**
- File: `r1cs_vc_ppzksnark.tcc`
- Function: `r1cs_vc_ppzksnark_generator<ppT>(const r1cs_constraint_system<FieldT>& cs)`
- Algorithm (Paper p.14):
  1. Sample random α, β, γ, δ, x ← F*
  2. Perform QAP reduction: call `r1cs_to_qap_instance_map_with_evaluation` to get {u_i(x), v_i(x), w_i(x), h(x)} evaluated at x
  3. Compute σ₁ (proving key):
     - Powers: {[x^i]₁}_{i=0}^{n-1}, {[x^i·P(x)/δ]₁}_{i=0}^{n-2}
     - Statement terms: {[(β·u_i(x) + α·v_i(x) + w_i(x))/γ]₁}_{i∈I^io}
     - Witness terms: {[(β·u_i(x) + α·v_i(x) + w_i(x))/δ]₁}_{i∈I^wt}
     - α, β, δ in appropriate groups
  4. Compute σ₂ (verification key): β, γ, δ in G2, precomputed e(α,β)
- **Key difference from Groth16**: No [δ]₁ terms for randomization; the CRS is deterministic given the random trapdoor

**Step 1.3: Implement VC.Prove**
- File: `r1cs_vc_ppzksnark.tcc`
- Function: `r1cs_vc_ppzksnark_prover<ppT>(const proving_key& pk, const primary_input& primary, const auxiliary_input& auxiliary)`
- Algorithm (Paper p.14):
  1. Compute QAP witness via `r1cs_to_qap_witness_map` (modified: pass d1=d2=d3=0 to remove randomization)
  2. Compute proof elements using multi-exponentiation:
     - A = [α]₁ + Σᵢ aᵢ·[u_i(x)]₁  (multi_exp over A_query)
     - B = [β]₂ + Σᵢ aᵢ·[v_i(x)]₂  (multi_exp over B_query in G2)
     - C = Σ_{i∈I^wt} aᵢ·[(α·v_i(x) + β·u_i(x) + w_i(x))/δ]₁ + [h(x)·P(x)/δ]₁  (multi_exp over L_query + H_query)
  3. Return π = (A, B, C)
- **Key difference from Groth16**: No random r,s; no terms r·[δ]₁ in A, s·[δ]₂ in B, (r·s)·[δ]₁ in C

**Step 1.4: Modify QAP witness map for non-ZK**
- File: `libsnark/reductions/r1cs_to_qap/r1cs_to_qap.tcc`
- Create a new function `r1cs_to_qap_witness_map_no_zk<FieldT>` that:
  - Computes the same witness polynomial coefficients as `r1cs_to_qap_witness_map`
  - Omits the d1, d2, d3 randomization (sets them to 0)
  - Returns the h(x) polynomial coefficients without blinding terms
- Alternatively: call existing `r1cs_to_qap_witness_map` with d1=d2=d3=FieldT::zero()

**Step 1.5: Implement VC.Verify**
- File: `r1cs_vc_ppzksnark.tcc`
- Function: `r1cs_vc_ppzksnark_verifier<ppT>(const vk, const primary_input, const proof)`
- Algorithm (Paper p.14):
  1. Parse π = ([A]₁, [B]₂, [C]₁)
  2. Compute accumulation: acc = Σ_{i∈I^io} aᵢ · [(β·u_i(x) + α·v_i(x) + w_i(x))/γ]₁
  3. Check pairing equation: e([A]₁, [B]₂) = e([α]₁, [β]₂) · e(acc, [γ]₂) · e([C]₁, [δ]₂)
  4. In practice, check: e(A, B) · e(-acc, γ_g2) · e(-C, δ_g2) = e(α, β)  (using product of pairings for efficiency)
- **Note**: Same verification equation as Groth16, just the proof was generated without randomization

**Step 1.6: Unit tests for VC scheme**
- File: `tests/test_r1cs_vc_ppzksnark.cpp`
- Test cases:
  - Simple circuit: x² + x + 5 = y (with known solution x=3, y=17)
  - Medium circuit: ~100 constraints
  - Completeness: valid witness → verify accepts
  - Soundness: invalid witness → verify rejects (multiple random attempts)
  - Edge case: single-constraint circuit
  - Edge case: circuit with only public inputs (empty witness)

---

### Phase 2: Linearly Updatable QAP Family (Paper Section 3.3)

**Step 2.1: Define data structures for circuit compositions**
- File: `r1cs_uvc_ppzksnark.hpp`
- Define `circuit_composition<FieldT>`:
  - Base circuit C (as `r1cs_constraint_system<FieldT>`)
  - Number of compositions B (max transitions)
  - Wire count n per circuit
  - Index sets I_j for each composition step j
  - I^io_j (input/output indices) and I^wt_j (witness indices) per step
- Define `updatable_qap_family<FieldT>`:
  - Shared modulus P_B(X) = Π_{q=1}^{Bn} (X - r_q)
  - Per-step polynomial evaluations: {û_i(x), v̂_i(x), ŵ_i(x)}_{i∈I_j} for each j
  - Common evaluation point x (from CRS)
  - Index difference sets: I_j \ I_{j-1} for each j > 1

**Step 2.2: Implement circuit composition logic**
- File: `r1cs_uvc_ppzksnark.tcc` (or a new file `circuit_composition.tcc`)
- Given a base circuit C with n wires and kn constraints (k multiplicative gates per circuit):
  1. For j ∈ [1,B], construct C_j by concatenating j copies of C:
     - Wire indices for step j: I_j = {1, ..., (j+1)n-1} (accounting for shared input/output gates)
     - New wires at step j: I_j \ I_{j-1} = {jn, ..., (j+1)n-1} (approximately)
  2. Assign unique evaluation points r_q for q ∈ [1, Bn] shared across all QAPs
  3. Compute P_j(X) = Π_{q=1}^{jn}(X - r_q) and P_B(X) = Π_{q=1}^{Bn}(X - r_q)

**Step 2.3: Implement QAP extension with shared modulus**
- File: Modify `libsnark/reductions/r1cs_to_qap/r1cs_to_qap.tcc`
- New function: `r1cs_to_updatable_qap_family<FieldT>`
  - Input: base circuit C, number of compositions B, evaluation domain roots r_1,...,r_{Bn}
  - For each j ∈ [1,B]:
    1. Construct constraint system for C_j (j compositions of C)
    2. Compute polynomials û_i(X), v̂_i(X), ŵ_i(X) of degree Bn-1 using Lagrange interpolation over ALL Bn roots
    3. Verify extension properties: û_i(X) ≡ u_i(X) mod P_j(X) for i ∈ I_j
    4. Verify P̂(X) ≡ 0 mod P(X) and P̂(X) ≠ 0
  - Output: `updatable_qap_family<FieldT>` with all per-step data

**Step 2.4: Evaluate QAP family at trapdoor point**
- Evaluate all polynomials {û_i(x), v̂_i(x), ŵ_i(x)} at the secret evaluation point x
- This is done during Setup and encoded into the CRS
- Reuse libfqfft's evaluation domain for efficient polynomial operations

---

### Phase 3: Updatable VC Scheme (Paper Section 4.2)

**Step 3.1: Implement UVC.Setup**
- File: `r1cs_uvc_ppzksnark.tcc`
- Function: `r1cs_uvc_ppzksnark_generator<ppT>(const circuit_composition& cc, size_t B)`
- Algorithm (Paper p.17):
  1. Sample random α, β, γ, δ, x ← F*
  2. Compute updatable QAP family (Step 2.3) evaluated at x
  3. Compute σ₁ (proving key) — larger than VC because it covers all B steps:
     - Powers: {[x^i·P(x)/δ]₁}_{i=0}^{Bn-2} (degree up to Bn-2 for quotient polynomial)
     - For EACH j ∈ [1,B]:
       - {u_i(x), v_i(x), w_i(x)}_{i∈I_j} as field elements (for incremental computation)
       - Statement terms: {[(β·u_i(x) + α·v_i(x) + w_i(x))/γ]₁}_{i∈I^io_j}
       - Witness terms: {[(β·u_i(x) + α·v_i(x) + w_i(x))/δ]₁}_{i∈I^wt_j}
  4. Compute σ₂ (verification key): same structure as VC, plus per-step {u_i(x), v_i(x), w_i(x)} in G2
  5. CRS size: Bn + 2 elements in G1, (3+n)B elements in G2

**Step 3.2: Define UVC proving key and verification key**
- File: `r1cs_uvc_ppzksnark.hpp`
- `r1cs_uvc_ppzksnark_proving_key<ppT>`:
  - All fields from VC proving key
  - Per-step data: `std::vector<step_proving_data<ppT>>` indexed by j
  - Each `step_proving_data` contains:
    - `A_query_delta`: G1 elements [u_i(x)]₁ for i ∈ I_j \ I_{j-1}
    - `B_query_delta`: G2 elements [v_i(x)]₂ for i ∈ I_j \ I_{j-1}
    - `L_query_delta`: G1 elements [(αv_i(x)+βu_i(x)+w_i(x))/δ]₁ for new witness wires
    - `H_query`: for computing h_j(x) quotient polynomial contribution
    - Index sets I_j, I^io_j, I^wt_j
- `r1cs_uvc_ppzksnark_proof<ppT>`:
  - Same as VC proof: (g_A, g_B, g_C) — three group elements
  - Plus: stored h_{j-1}(x) polynomial (field element vector) for computing h_j(x) - h_{j-1}(x) difference

**Step 3.3: Implement UVC.Prove**
- File: `r1cs_uvc_ppzksnark.tcc`
- Function: `r1cs_uvc_ppzksnark_prover<ppT>(const uvc_proving_key& pk, const transition_input& input, size_t j, const proof* prev_proof)`
- Algorithm (Paper p.18):
  - **Case j = 1**: Call `r1cs_vc_ppzksnark_prover` with the base circuit C₁ = C
  - **Case j > 1**: Parse π_{j-1} = ([A_{j-1}]₁, [B_{j-1}]₂, [C_{j-1}]₁) and compute:
    1. **A_j** = A_{j-1} + Σ_{i∈I_j\I_{j-1}} a_i · [u_i(x)]₁
       - Only |I_j \ I_{j-1}| = O(n) scalar multiplications in G1
    2. **B_j** = B_{j-1} + Σ_{i∈I_j\I_{j-1}} a_i · [v_i(x)]₂
       - Only O(n) scalar multiplications in G2
    3. **C_j** = C_{j-1} + Σ_{i∈(I_j\I_{j-1})^wt} a_i · [(αv_i(x)+βu_i(x)+w_i(x))/δ]₁ + [(h_j(x)-h_{j-1}(x))·t(x)/δ]₁
       - Compute h_j(x) satisfying Σ_{i∈I_j} a_i·u_i(X)·Σ a_i·v_i(X) = Σ a_i·w_i(X) + h_j(X)·P(X)
       - The quotient difference h_j(x) - h_{j-1}(x) can be computed from new constraints only
    4. Store h_j(x) for next update
  - Return π_j = ([A_j]₁, [B_j]₂, [C_j]₁)

**Step 3.4: Implement UVC.Verify**
- File: `r1cs_uvc_ppzksnark.tcc`
- Function: `r1cs_uvc_ppzksnark_verifier<ppT>(const uvc_vk& vk, const transition_statement& stmt, const proof& pi)`
- Algorithm (Paper p.18): Simply call `VC.Verify(σ, s_j, t₁, π_j)` — the verification equation is identical
- The statement includes (t₁, j, s_j) where t₁ is the initial transition, j is the step count, s_j is the current state

**Step 3.5: Implement incremental quotient polynomial computation**
- This is the most technically subtle part
- File: `r1cs_uvc_ppzksnark.tcc` (helper function)
- Given h_{j-1}(x) and new constraints from step j:
  1. Compute full A_j(X) = Σ_{i∈I_j} a_i·u_i(X) and B_j(X) = Σ_{i∈I_j} a_i·v_i(X)
  2. Compute W_j(X) = Σ_{i∈I_j} a_i·w_i(X)
  3. h_j(X) = (A_j(X)·B_j(X) - W_j(X)) / P_j(X)
  4. But we can compute the *difference* (h_j - h_{j-1}) more efficiently using the structure of the updatable QAP
  5. The key insight: polynomial division by P_B(X) dominates at O(Bn), but the incremental terms only add O(n) new constraints
- Use libfqfft for polynomial multiplication and division

---

### Phase 4: Testing

**Step 4.1: Simple arithmetic circuit tests for VC**
- File: `tests/test_r1cs_vc_ppzksnark.cpp`
- Test circuit 1: x² = y (one multiplication gate)
- Test circuit 2: x³ + x + 5 = y (chained multiplications)
- Verify: Setup → Prove → Verify roundtrip
- Verify: Invalid witness → rejection

**Step 4.2: State transition circuit tests for UVC**
- File: `tests/test_r1cs_uvc_ppzksnark.cpp`
- Test circuit: Simple counter — state s_j = s_{j-1} + t_j (addition with carry)
- Test with B = 5 sequential compositions:
  - Generate σ ← UVC.Setup(R, B=5)
  - π₁ ← UVC.Prove(σ, (t₁, 1, s₁), ⊥) — base proof
  - For j = 2..5: π_j ← UVC.Prove(σ, (t₁, j, s_j), π_{j-1}) — incremental updates
  - Verify each π_j independently
- Verify: Updated proof matches fresh proof (AC-7)
- Verify: Update cost is O(n) not O(jn) — measure wall-clock time for j=1 vs j=5

**Step 4.3: Correctness cross-validation**
- For a given circuit and inputs, compute proof both ways:
  1. Full VC.Prove on the composed circuit C_j (from scratch)
  2. Incremental UVC.Prove updating from π_{j-1}
- Both must produce identical group elements (A_j, B_j, C_j)

**Step 4.4: Performance benchmarks**
- Compare prover time: VC.Prove(C_j) vs UVC.Prove(update from π_{j-1})
- Measure for j = 1, 2, 5, 10 with a circuit of n = 1000 wires
- Verify that update cost is sub-linear in j (proving AC-8)
- Report: wall-clock time, number of multi-exponentiation operations, memory usage

---

### Phase 5: Build System and Integration

**Step 5.1: CMake configuration**
- Update `libsnark/CMakeLists.txt` to add new modules:
  - `r1cs_vc_ppzksnark` library target
  - `r1cs_uvc_ppzksnark` library target
  - Test executables
- Ensure BN128 (alt_bn128) is selected as the default curve
- Add macOS/Apple Silicon compatibility flags if needed

**Step 5.2: Top-level Dev/ build script**
- Create `Dev/CMakeLists.txt` or `Dev/Makefile` as a convenience wrapper
- Build steps:
  ```
  cd Dev/libsnark && mkdir -p build && cd build
  cmake .. -DCURVE=ALT_BN128 -DWITH_PROCPS=OFF
  make -j$(nproc)
  ```

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| libsnark build fails on macOS (Apple Silicon) | Blocks all progress | Use Homebrew for dependencies (gmp, openssl); patch CMake for ARM64; fall back to x86 via Rosetta if needed |
| Polynomial division for h_j(x) is numerically unstable in finite fields | Incorrect proofs | Finite field arithmetic is exact — no floating point issues. Validate h_j(x) by checking A_j·B_j = W_j + h_j·P_j over the field |
| CRS size blows up for large B | Memory issues | Start with small B (≤10); the CRS is O((3+n)B) group elements — for n=1000, B=10 this is ~40K elements, manageable |
| Incremental h_j computation is more complex than expected | Delays Phase 3 | Can fall back to full recomputation of h_j(x) from scratch (losing O(Bn) → O(n) improvement for this component, but A_j and B_j updates remain O(n)) |
| libsnark's QAP reduction uses a different root selection than the paper | Incorrect extension properties | The paper requires shared r_q across all P_j. We control root selection in `r1cs_to_qap_instance_map` — ensure roots for step j are a subset of roots for step j+1 |

---

## Verification Steps

1. **Build verification**: `cmake --build . --target all` succeeds with zero warnings for new code
2. **Unit test verification**: All tests in `test_r1cs_vc_ppzksnark` pass
3. **Integration test verification**: All tests in `test_r1cs_uvc_ppzksnark` pass, including multi-step proof updates
4. **Cross-validation**: Incremental proof == full proof for the same inputs (Step 4.3)
5. **Performance verification**: Benchmark shows update cost is O(n) not O(jn) (Step 4.4)
6. **Pairing equation verification**: Manual check that e(A,B) = e(α,β)·e(acc,γ)·e(C,δ) holds for generated proofs

---

## Dependency Order

```
Phase 0 (Setup)
    ↓
Phase 1 (VC Scheme)  ──→  Step 4.1 (VC Tests)
    ↓
Phase 2 (Updatable QAP Family)
    ↓
Phase 3 (UVC Scheme)  ──→  Step 4.2-4.4 (UVC Tests + Benchmarks)
    ↓
Phase 5 (Build Integration)
```

Phases 1 and 2 have a partial overlap: the QAP reduction in Step 1.4 can be developed in parallel with Step 2.1-2.2, but Step 2.3 (shared modulus QAP) depends on having the VC scheme working first for validation.
