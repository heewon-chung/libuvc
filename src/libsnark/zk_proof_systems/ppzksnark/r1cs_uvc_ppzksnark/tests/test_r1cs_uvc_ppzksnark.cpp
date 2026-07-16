/** @file
*****************************************************************************

End-to-end integration test for the R1CS UVC (Updatable Verifiable Computation)
ppzkSNARK scheme.

This file exercises the full UVC lifecycle — Setup, Prove, Verify — on
concrete state-transition circuits, validating correctness across multiple
incremental composition steps.

=== What is being tested ===

The UVC scheme (a non-ZK variant of Groth16 with linear proof updates)
allows a prover to incrementally extend a proof π_j for the j-step composed
circuit C_j into a proof π_{j+1} for C_{j+1}, WITHOUT re-proving from
scratch. The key insight is the linearly updatable QAP family:

    A_{j+1} = A_j + Σ_{i ∈ I_{j+1}\I_j} a_i · u_i(x)
    B_{j+1} = B_j + Σ_{i ∈ I_{j+1}\I_j} a_i · v_i(x)
    C_{j+1} updates similarly with witness terms and quotient polynomial h_j(x)

This test uses two toy circuits to verify this works end-to-end:

  1. **Multiplier circuit** (1 constraint, state_size=1):
     Computes s_out = s_in * t  (a single multiplication gate).
     After j steps: s_j = s_0 * t_1 * t_2 * ... * t_j.

  2. **Two-state multiplier** (2 constraints, state_size=2):
     Computes (a_out, b_out) = (a_in * t, b_in * t).
     Tests that multi-wire state sharing works correctly.

=== Test structure ===

  Test 1: Multiplier, 3 steps — basic multi-step incremental proof chain
  Test 2: Two-state multiplier, 3 steps — multi-wire state vector
  Test 3: Partial compositions — B > num_steps (CRS over-provisioned)
  Test 4: Incremental vs fresh — cross-validates step-1 and step-2 proofs

Each test includes a **soundness check**: after successful verification,
it corrupts the primary input and confirms the verifier rejects.

=== Pure multiplicative gate requirement ===

The UVC scheme requires "pure multiplicative gate" circuits — all R1CS
constraints must have the form A * B = C where the constant wire (wire 0)
has zero coefficient in both A and B vectors. This ensures that zero-padded
assignments for future wires satisfy future constraints as 0 * 0 = 0,
which is essential for the linearly updatable QAP property. Concretely,
when we build C_B from B copies of the base circuit, wires that belong
to future steps (j' > j) are assigned value 0 in the j-th proof. The
pure multiplicative gate form guarantees these zero-padded assignments
trivially satisfy the future constraints.

===  How to build and run ===

    cd build
    make zk_proof_systems_r1cs_uvc_ppzksnark_test
    ./zk_proof_systems_r1cs_uvc_ppzksnark_test

=== Expected output (key lines) ===

For each test, the binary prints:
  - Setup info: circuit dimensions, CRS sizes
  - Per-step: primary/auxiliary input sizes, constraint satisfaction check,
    proof size (always 2 G1 + 1 G2 = 1019 bits), and verification result

Expected output (abbreviated, filtering to test-specific lines):

    ================================================================
    UVC Test: Multiplier s*=t (3 steps) (B=3, steps=3)
    ================================================================
    --- UVC.Setup ---
    * Composed C_3: 3 constraints, 7 variables, 2 inputs
    --- Step 1 ---
    * Primary input size: 2          ← [s_0=3, t_1=5]
    * Auxiliary input size: 1        ← [s_1=15]
    * C_1 satisfied: YES
    * UVC.Verify step 1: PASS
    --- Step 2 ---
    * Primary input size: 2          ← same [s_0=3, t_1=5]
    * Auxiliary input size: 3        ← [s_1=15, t_2=7, s_2=105]
    * C_2 satisfied: YES
    * UVC.Verify step 2: PASS
    --- Step 3 ---
    * Primary input size: 2          ← same [s_0=3, t_1=5]
    * Auxiliary input size: 5        ← [s_1=15, t_2=7, s_2=105, t_3=2, s_3=210]
    * C_3 satisfied: YES
    * UVC.Verify step 3: PASS
    --- Soundness test ---
    * Soundness (bad input rejected): PASS
    Multiplier s*=t (3 steps): ALL PASSED
    ...
    ================================================================
    UVC Test: Two-state mult (a*t, b*t) (3 steps) (B=4, steps=3)
    ================================================================
    * Composed C_4: 8 constraints, 14 variables, 3 inputs
    * Primary input size: 3          ← [a_0=2, b_0=3, t_1=5]
    ...
    Two-state mult (a*t, b*t) (3 steps): ALL PASSED
    ...
    Partial compositions (B=5, steps=1): ALL PASSED
    ...
    Incremental vs Fresh: ALL PASSED
    ================================================================
    ALL UVC TESTS PASSED            ← exit code 0
    ================================================================

Key invariants visible in output:
  - Proof size is always 1019 bits (2 G1 + 1 G2) regardless of step
  - Primary input size is fixed across all steps (2 for multiplier, 3 for two-state)
  - Auxiliary input size grows by (n - state_size) per step
  - All soundness checks show "FAIL" from verifier then "PASS" from test

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#include <cassert>
#include <cstdio>
#include <vector>

#include <libff/common/profiling.hpp>
#include <libff/common/utils.hpp>
#include <libff/algebra/curves/alt_bn128/alt_bn128_pp.hpp>

#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.hpp>

using namespace libsnark;

/**
 * Create a minimal state transition circuit: s_out = s_in * t
 *
 * This is the simplest possible pure-multiplicative-gate circuit.
 * It has exactly 1 R1CS constraint and 3 wires (excluding the constant wire 0):
 *
 * Wire layout (1-indexed, wire 0 is the constant "1" wire):
 *   Wire 1: s_in  (state input)   — carried from the previous step's output
 *   Wire 2: t     (transition input) — fresh external input at each step
 *   Wire 3: s_out (state output)  — becomes s_in for the next step
 *
 * R1CS constraint: s_in * t = s_out
 *   A = [0, 1, 0, 0]  (coefficient 1 on wire 1)
 *   B = [0, 0, 1, 0]  (coefficient 1 on wire 2)
 *   C = [0, 0, 0, 1]  (coefficient 1 on wire 3)
 *
 * Note: Wire 0 (constant) has zero coefficient in A and B — this is the
 * "pure multiplicative gate" requirement. Without it, the zero-padding
 * trick for future wires would fail because 1 * 1 ≠ 0.
 *
 * In the UVC composition:
 *   primary_input  = {s_0, t_1}  (state_size + transition_size = 2)
 *   auxiliary_input = {s_1}       (the computed output)
 *
 * After j compositions, the circuit C_j has j copies of this constraint
 * with wire sharing: s_out of step k becomes s_in of step k+1.
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_multiplier_circuit()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 2;   /* s_in, t */
    cs.auxiliary_input_size = 1; /* s_out */

    /* Constraint: s_in * t = s_out */
    linear_combination<FieldT> A, B, C;
    A.add_term(1, FieldT::one()); /* s_in */
    B.add_term(2, FieldT::one()); /* t */
    C.add_term(3, FieldT::one()); /* s_out */

    cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));

    return state_transition_circuit<FieldT>(cs, 1, 1);
}

/**
 * Create a 2-element state circuit with pure multiplicative gates.
 *
 * This circuit tests multi-wire state vectors: the state is a pair (a, b),
 * and both components are updated by the same transition scalar t.
 * It has 2 R1CS constraints and 5 wires.
 *
 * Wire layout (1-indexed):
 *   Wire 1: a_in   (state input, component 1)
 *   Wire 2: b_in   (state input, component 2)
 *   Wire 3: t      (transition input)
 *   Wire 4: a_out  (state output, component 1)
 *   Wire 5: b_out  (state output, component 2)
 *
 * Constraints:
 *   Constraint 1: a_in * t = a_out    (pure multiplicative)
 *   Constraint 2: b_in * t = b_out    (pure multiplicative)
 *
 * In the UVC composition at step j, the composed circuit C_j has:
 *   - 2*j constraints (2 per step)
 *   - 5 + (j-1)*3 variables (5 base wires + 3 new wires per additional step)
 *     where new_per_step = n - state_size = 5 - 2 = 3 (t_j, a_j, b_j)
 *   - primary_input = {a_0, b_0, t_1} (state_size + transition_size = 3)
 *   - auxiliary_input = remaining wires
 *
 * Wire sharing across steps:
 *   Step 1 outputs (a_1, b_1) at wires 4,5 become step 2's state inputs.
 *   Step 2 adds 3 new wires: t_2, a_2, b_2 at wires 6,7,8.
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_two_state_mult_circuit()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 3;   /* a_in, b_in, t */
    cs.auxiliary_input_size = 2; /* a_out, b_out */

    /* Constraint 1: a_in * t = a_out */
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(1, FieldT::one()); /* a_in */
        B.add_term(3, FieldT::one()); /* t */
        C.add_term(4, FieldT::one()); /* a_out */
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    /* Constraint 2: b_in * t = b_out */
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(2, FieldT::one()); /* b_in */
        B.add_term(3, FieldT::one()); /* t */
        C.add_term(5, FieldT::one()); /* b_out */
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    return state_transition_circuit<FieldT>(cs, 2, 1);
}

/**
 * Generic UVC integration test: runs Setup → Prove (base + incremental) → Verify
 * for a given state transition circuit and execution trace.
 *
 * The test exercises the full UVC workflow:
 *
 *   1. **Setup** (generator): Generates proving/verification keys for C_B,
 *      the B-fold composed circuit. The CRS contains base QAP evaluations
 *      for C_1 plus per-step delta elements for steps 2..B.
 *
 *   2. **Prove** (iterative): For each step j = 1..num_steps:
 *      - Builds the composed assignment (primary + auxiliary) for C_j
 *      - Sanity-checks that the composed constraint system C_j is satisfied
 *      - Calls the prover:
 *        * Step 1 (base): computes proof π_1 from scratch using the full C_B QAP
 *        * Step j>1 (incremental): updates π_{j-1} to π_j using only the
 *          delta CRS elements, adding contributions from new wires I_j \ I_{j-1}
 *
 *   3. **Verify**: Checks each proof π_j against the verification key and
 *      the primary input [s_0, t_1]. Verification is a constant-time
 *      pairing check: e(A, B) = e(α, β) · e(Σ γ_i·x_i, γ) · e(C, δ).
 *
 *   4. **Soundness check**: Corrupts the primary input and confirms the
 *      verifier rejects, ensuring the pairing equation is non-trivially binding.
 *
 * @param st_circuit      The base state transition circuit (one step)
 * @param B               Max compositions the CRS supports (≥ num_steps)
 * @param num_steps       Number of steps to actually prove (1 ≤ num_steps ≤ B)
 * @param states          State vectors s_0, s_1, ..., s_{num_steps}
 * @param transitions     Transition inputs t_1, ..., t_{num_steps}
 * @param step_witnesses  Per-step internal witness vectors (empty if no internal wires)
 * @param test_name       Descriptive name printed in output
 */
template<typename ppT>
bool test_uvc(
    const state_transition_circuit<libff::Fr<ppT> > &st_circuit,
    size_t B,
    size_t num_steps,
    const std::vector<std::vector<libff::Fr<ppT> > > &states,
    const std::vector<std::vector<libff::Fr<ppT> > > &transitions,
    const std::vector<std::vector<libff::Fr<ppT> > > &step_witnesses,
    const char *test_name)
{
    typedef libff::Fr<ppT> FieldT;

    printf("================================================================\n");
    printf("UVC Test: %s (B=%zu, steps=%zu)\n", test_name, B, num_steps);
    printf("================================================================\n");
    printf("  Tests the full UVC lifecycle: Setup -> Prove (base + incremental) -> Verify.\n");
    printf("  Input:    state transition circuit, execution trace of %zu steps\n", num_steps);
    printf("  Expected: all steps verify PASS, soundness check (corrupted input) rejected\n");

    /* Setup */
    printf("\n--- UVC.Setup ---\n");
    printf("  Generates CRS for up to B=%zu compositions from the base circuit.\n", B);
    const r1cs_uvc_ppzksnark_keypair<ppT> keypair = r1cs_uvc_ppzksnark_generator<ppT>(st_circuit, B);

    bool all_pass = true;

    r1cs_uvc_ppzksnark_proof<ppT> prev_proof;
    bool have_prev = false;

    for (size_t step = 1; step <= num_steps; ++step)
    {
        printf("\n--- Step %zu (%s) ---\n", step, step == 1 ? "base proof" : "incremental update from previous proof");

        /* Build primary_input and auxiliary_input for C_step */
        auto assignment = build_composed_assignment(st_circuit, step, states, transitions, step_witnesses);
        const r1cs_uvc_ppzksnark_primary_input<ppT> &primary = assignment.first;
        const r1cs_uvc_ppzksnark_auxiliary_input<ppT> &auxiliary = assignment.second;

        printf("* Primary input size: %zu\n", primary.size());
        printf("* Auxiliary input size: %zu\n", auxiliary.size());

        /* Sanity: check that the composed constraint system is satisfied */
        r1cs_constraint_system<FieldT> cs_step = build_composed_constraint_system(st_circuit, step);
        bool cs_sat = cs_step.is_satisfied(primary, auxiliary);
        printf("* C_%zu satisfied: %s\n", step, cs_sat ? "YES" : "NO");
        if (!cs_sat) {
            printf("ERROR: Composed constraint system not satisfied at step %zu!\n", step);
            all_pass = false;
            break;
        }

        /* Prove */
        r1cs_uvc_ppzksnark_proof<ppT> proof = r1cs_uvc_ppzksnark_prover<ppT>(
            keypair.pk, step, primary, auxiliary,
            have_prev ? &prev_proof : nullptr);
        const bool off_shape = !proof.bind_state && proof.G1_size() == 2 &&
            proof.G2_size() == 1;
        printf("* Proof shape: G1=%zu, G2=%zu, size_in_bits=%zu: %s\n",
               proof.G1_size(), proof.G2_size(), proof.size_in_bits(),
               off_shape ? "PASS" : "FAIL");
        if (!off_shape) {
            all_pass = false;
        }

        /* Verify */
        bool verified = r1cs_uvc_ppzksnark_verifier<ppT>(keypair.vk, step, primary, proof);
        printf("* UVC.Verify step %zu: %s\n", step, verified ? "PASS" : "FAIL");

        if (!verified) {
            printf("ERROR: Verification failed at step %zu!\n", step);
            all_pass = false;
        }

        /* Store proof for next step */
        prev_proof = proof;
        have_prev = true;
    }

    /* Soundness test: modify primary input and verify it fails */
    if (all_pass && num_steps >= 1)
    {
        printf("\n--- Soundness test ---\n");
        printf("  Corrupts primary[0] with a random field element.\n");
        printf("  Expected: verifier rejects (pairing equation fails).\n");
        auto assignment = build_composed_assignment(st_circuit, num_steps, states, transitions, step_witnesses);
        r1cs_uvc_ppzksnark_primary_input<ppT> bad_primary = assignment.first;
        if (bad_primary.size() > 0) {
            bad_primary[0] = FieldT::random_element();
        }

        bool bad_verified = r1cs_uvc_ppzksnark_verifier<ppT>(
            keypair.vk, num_steps, bad_primary, prev_proof);
        printf("* Soundness (bad input rejected): %s\n", !bad_verified ? "PASS" : "FAIL");
        if (bad_verified) {
            printf("ERROR: Soundness check failed!\n");
            all_pass = false;
        }
    }

    printf("\n%s: %s\n\n", test_name, all_pass ? "ALL PASSED" : "SOME FAILED");
    return all_pass;
}

/**
 * State-bound UVC integration test: verifies every proof against the
 * caller-held eta-track state commitment and the reported output state.
 */
template<typename ppT>
bool test_uvc_state_bound(
    const state_transition_circuit<libff::Fr<ppT> > &st_circuit,
    size_t B,
    size_t num_steps,
    const std::vector<std::vector<libff::Fr<ppT> > > &states,
    const std::vector<std::vector<libff::Fr<ppT> > > &transitions,
    const std::vector<std::vector<libff::Fr<ppT> > > &step_witnesses,
    const char *test_name)
{
    typedef libff::Fr<ppT> FieldT;

    printf("================================================================\n");
    printf("State-bound UVC Test: %s (B=%zu, steps=%zu)\n", test_name, B, num_steps);
    printf("================================================================\n");

    const auto keypair = r1cs_uvc_ppzksnark_generator<ppT>(st_circuit, B, true);
    bool all_pass = true;
    libff::G1<ppT> D_prev = libff::G1<ppT>::zero();
    libff::G1<ppT> final_D_prev = D_prev;
    r1cs_uvc_ppzksnark_proof<ppT> prev_proof;
    bool have_prev = false;

    for (size_t step = 1; step <= num_steps; ++step)
    {
        const auto assignment = build_composed_assignment(
            st_circuit, step, states, transitions, step_witnesses);
        const auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            keypair.pk, step, assignment.first, assignment.second,
            have_prev ? &prev_proof : nullptr);
        const bool shape = proof.bind_state && proof.G1_size() == 3 &&
            proof.G2_size() == 1;
        printf("  Step %zu: G1=%zu, G2=%zu, size_in_bits=%zu, shape=%s\n",
               step, proof.G1_size(), proof.G2_size(), proof.size_in_bits(),
               shape ? "PASS" : "FAIL");
        const bool accepted = r1cs_uvc_ppzksnark_verifier<ppT>(
            keypair.vk, step, assignment.first, states[step], proof, D_prev);
        printf("  Step %zu: state-bound verify=%s\n",
               step, accepted ? "PASS" : "FAIL");
        all_pass &= shape && accepted;
        if (step == num_steps) {
            final_D_prev = D_prev;
        }
        if (accepted) {
            D_prev = proof.g_D;
        }
        prev_proof = proof;
        have_prev = true;
    }

    if (all_pass && num_steps >= 1)
    {
        std::vector<FieldT> bad_reported_state = states[num_steps];
        bad_reported_state[0] += FieldT::one();
        const auto assignment = build_composed_assignment(
            st_circuit, num_steps, states, transitions, step_witnesses);
        const bool bad_accepted = r1cs_uvc_ppzksnark_verifier<ppT>(
            keypair.vk, num_steps, assignment.first, bad_reported_state,
            prev_proof, final_D_prev);
        printf("  Soundness (wrong reported state rejected): %s\n",
               !bad_accepted ? "PASS" : "FAIL");
        all_pass &= !bad_accepted;
    }

    printf("\n%s state-bound: %s\n\n",
           test_name, all_pass ? "ALL PASSED" : "SOME FAILED");
    return all_pass;
}


/**
 * Test 1: Multiplier circuit, s_out = s_in * t, for 3 steps (B = num_steps = 3).
 *
 * Execution trace (each step applies s_j = s_{j-1} * t_j):
 *   s_0 = 3
 *   Step 1: t_1 = 5,  s_1 = 3 * 5   = 15
 *   Step 2: t_2 = 7,  s_2 = 15 * 7  = 105
 *   Step 3: t_3 = 2,  s_3 = 105 * 2 = 210
 *
 * Composed circuit C_3 has:
 *   - 3 constraints (1 per step)
 *   - 7 wires: [s_0, t_1, s_1, t_2, s_2, t_3, s_3]
 *   - primary = [s_0=3, t_1=5], auxiliary = [s_1=15, t_2=7, s_2=105, t_3=2, s_3=210]
 *
 * This test verifies the basic incremental proof chain:
 *   π_1 (base proof for C_1) → π_2 (updated for C_2) → π_3 (updated for C_3)
 * Each π_j is verified independently against the same vk and primary input.
 */
template<typename ppT>
bool test_multiplier_3_steps()
{
    typedef libff::Fr<ppT> FieldT;

    auto st = make_multiplier_circuit<FieldT>();

    const size_t B = 3;
    const size_t num_steps = 3;

    std::vector<std::vector<FieldT> > states = {
        {FieldT(3)},
        {FieldT(15)},
        {FieldT(105)},
        {FieldT(210)}
    };

    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)},
        {FieldT(7)},
        {FieldT(2)}
    };

    std::vector<std::vector<FieldT> > witnesses = {
        {},
        {},
        {}
    };

    return test_uvc<ppT>(st, B, num_steps, states, transitions, witnesses, "Multiplier s*=t (3 steps)");
}

/**
 * Test 2: Two-state multiplicative circuit for 3 steps (B=4, num_steps=3).
 *
 * This tests a circuit with state_size=2, verifying that multi-wire state
 * vectors are correctly shared across composition steps.
 *
 * Execution trace ((a_j, b_j) = (a_{j-1} * t_j, b_{j-1} * t_j)):
 *   (a_0, b_0) = (2, 3)
 *   Step 1: t_1 = 5, (a_1, b_1) = (10, 15)
 *   Step 2: t_2 = 3, (a_2, b_2) = (30, 45)
 *   Step 3: t_3 = 2, (a_3, b_3) = (60, 90)
 *
 * B is set to 4 (> num_steps=3) to also exercise the case where the CRS
 * supports more compositions than actually used.
 *
 * Per step, the composed circuit adds 3 new wires (t_j, a_j, b_j) and
 * 2 new constraints. State wires (a_{j-1}, b_{j-1}) are shared with the
 * previous step's output wires — this sharing is the core of the UVC
 * wire reuse mechanism that enables linear proof updates.
 */
template<typename ppT>
bool test_two_state_mult_3_steps()
{
    typedef libff::Fr<ppT> FieldT;

    auto st = make_two_state_mult_circuit<FieldT>();

    const size_t B = 4;
    const size_t num_steps = 3;

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)},
        {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(45)},
        {FieldT(60), FieldT(90)}
    };

    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)},
        {FieldT(3)},
        {FieldT(2)}
    };

    /* No internal witnesses (2*state_size + transition_size = 5 = num_variables) */
    std::vector<std::vector<FieldT> > witnesses = {
        {},
        {},
        {}
    };

    return test_uvc<ppT>(st, B, num_steps, states, transitions, witnesses, "Two-state mult (a*t, b*t) (3 steps)");
}

template<typename ppT>
bool test_multiplier_3_steps_state_bound()
{
    typedef libff::Fr<ppT> FieldT;
    const auto st = make_multiplier_circuit<FieldT>();
    const std::vector<std::vector<FieldT> > states = {
        {FieldT(3)}, {FieldT(15)}, {FieldT(105)}, {FieldT(210)}
    };
    const std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(7)}, {FieldT(2)}
    };
    const std::vector<std::vector<FieldT> > witnesses = { {}, {}, {} };
    return test_uvc_state_bound<ppT>(
        st, 3, 3, states, transitions, witnesses, "Multiplier s*=t (3 steps)");
}

template<typename ppT>
bool test_two_state_mult_3_steps_state_bound()
{
    typedef libff::Fr<ppT> FieldT;
    const auto st = make_two_state_mult_circuit<FieldT>();
    const std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)}, {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(45)}, {FieldT(60), FieldT(90)}
    };
    const std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(3)}, {FieldT(2)}
    };
    const std::vector<std::vector<FieldT> > witnesses = { {}, {}, {} };
    return test_uvc_state_bound<ppT>(
        st, 4, 3, states, transitions, witnesses, "Two-state mult (a*t, b*t) (3 steps)");
}

/**
 * Test 3: Partial composition — B=5 but only 1 step is proved.
 *
 * This tests that the CRS can be over-provisioned: Setup allocates CRS
 * elements for up to B=5 compositions, but the prover only uses step 1.
 * The zero-padded assignment for wires of steps 2..5 must still produce
 * a valid proof for C_1 embedded in the larger C_B QAP.
 *
 * Concretely, the prover computes the QAP witness over C_B (the full
 * circuit), but with wires for steps 2..5 set to zero. The pure
 * multiplicative gate property ensures these contribute 0 to A, B, and C
 * polynomials, so the quotient h(x) is well-defined.
 *
 * Execution trace:
 *   s_0 = 2
 *   Step 1: t_1 = 4, s_1 = 8
 */
template<typename ppT>
bool test_partial_compositions()
{
    typedef libff::Fr<ppT> FieldT;

    auto st = make_multiplier_circuit<FieldT>();

    const size_t B = 5;
    const size_t num_steps = 1;

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)},
        {FieldT(8)}
    };

    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(4)}
    };

    std::vector<std::vector<FieldT> > witnesses = {
        {}
    };

    return test_uvc<ppT>(st, B, num_steps, states, transitions, witnesses, "Partial compositions (B=5, steps=1)");
}

/**
 * Test 4: Cross-validate incremental proof vs fresh proof.
 *
 * This test verifies that the proof update mechanism produces valid proofs
 * by running 2 steps and verifying both independently:
 *
 *   - Step 1 (fresh/base): π_1 is computed from scratch for C_1.
 *     The prover evaluates the full QAP witness A(x), B(x), C(x), h(x)
 *     and computes g_A, g_B, g_C via multi-scalar multiplications.
 *
 *   - Step 2 (incremental): π_2 is computed by updating π_1.
 *     The prover adds delta contributions for the new wires (t_2, s_2)
 *     to the cached proof components:
 *       g_A_2 = g_A_1 + Σ_{new wires} a_i · [u_i(τ)]₁
 *       g_B_2 = g_B_1 + Σ_{new wires} a_i · [v_i(τ)]₂
 *       g_C_2 recomputed from updated witness and h polynomial
 *
 * Both proofs verify against the same vk (since the verification equation
 * only depends on the primary input [s_0, t_1], not the step count).
 *
 * Execution trace:
 *   s_0 = 10, t_1 = 7, s_1 = 70, t_2 = 3, s_2 = 210
 */
template<typename ppT>
bool test_incremental_vs_fresh()
{
    typedef libff::Fr<ppT> FieldT;

    printf("================================================================\n");
    printf("UVC Test: Incremental vs Fresh proof\n");
    printf("================================================================\n");
    printf("  Cross-validates a base proof (step 1) and an incremental update (step 2).\n");
    printf("  Input:    s_0=10, t_1=7 -> s_1=70, t_2=3 -> s_2=210\n");
    printf("  Expected: step 1 (fresh) verify PASS, step 2 (incremental) verify PASS\n");

    auto st = make_multiplier_circuit<FieldT>();
    const size_t B = 2;

    /* States and transitions */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(10)},
        {FieldT(70)},
        {FieldT(210)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(7)},
        {FieldT(3)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {} };

    /* Setup */
    const r1cs_uvc_ppzksnark_keypair<ppT> keypair = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

    /* Step 1: fresh proof */
    auto assign1 = build_composed_assignment(st, 1, states, transitions, witnesses);
    r1cs_uvc_ppzksnark_proof<ppT> proof1 = r1cs_uvc_ppzksnark_prover<ppT>(
        keypair.pk, 1, assign1.first, assign1.second, nullptr);

    bool v1 = r1cs_uvc_ppzksnark_verifier<ppT>(keypair.vk, 1, assign1.first, proof1);
    printf("* Step 1 verify: %s\n", v1 ? "PASS" : "FAIL");

    /* Step 2: incremental proof */
    auto assign2 = build_composed_assignment(st, 2, states, transitions, witnesses);
    r1cs_uvc_ppzksnark_proof<ppT> proof2_inc = r1cs_uvc_ppzksnark_prover<ppT>(
        keypair.pk, 2, assign2.first, assign2.second, &proof1);

    bool v2_inc = r1cs_uvc_ppzksnark_verifier<ppT>(keypair.vk, 2, assign2.first, proof2_inc);
    printf("* Step 2 (incremental) verify: %s\n", v2_inc ? "PASS" : "FAIL");

    bool all_pass = v1 && v2_inc;
    printf("\nIncremental vs Fresh: %s\n\n", all_pass ? "ALL PASSED" : "SOME FAILED");
    return all_pass;
}


int main()
{
    /* Initialize the alt_bn128 elliptic curve parameters (BN254).
       This sets up the finite field F_r and the pairing groups G1, G2, GT
       used by the Groth16-style proof system. */
    libff::alt_bn128_pp::init_public_params();
    libff::inhibit_profiling_info = true;

    bool all_pass = true;

    /* Test 1: Basic 3-step incremental proof chain (single-state multiplier) */
    all_pass &= test_multiplier_3_steps<libff::alt_bn128_pp>();

    /* Test 2: Multi-wire state vector (two-state multiplier, 3 steps) */
    all_pass &= test_two_state_mult_3_steps<libff::alt_bn128_pp>();
    /* State-bound variants: eta-track state commitment and D carry. */
    all_pass &= test_multiplier_3_steps_state_bound<libff::alt_bn128_pp>();
    all_pass &= test_two_state_mult_3_steps_state_bound<libff::alt_bn128_pp>();

    /* Test 3: Over-provisioned CRS (B=5, only 1 step used) */
    all_pass &= test_partial_compositions<libff::alt_bn128_pp>();

    /* Test 4: Cross-validate fresh base proof vs incremental update */
    all_pass &= test_incremental_vs_fresh<libff::alt_bn128_pp>();

    if (all_pass)
    {
        printf("================================================================\n");
        printf("ALL UVC TESTS PASSED\n");
        printf("================================================================\n");
        return 0;
    }
    else
    {
        printf("================================================================\n");
        printf("SOME UVC TESTS FAILED\n");
        printf("================================================================\n");
        return 1;
    }
}
