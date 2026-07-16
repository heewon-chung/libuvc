/** @file
*****************************************************************************

Unit tests for r1cs_uvc_ppzksnark_prover() and r1cs_uvc_ppzksnark_verifier().

These tests isolate the Prover and Verifier algorithms (as opposed to the
end-to-end tests in test_r1cs_uvc_ppzksnark.cpp) to verify individual
correctness properties.

=== Proof structure recap ===

A UVC proof π_j = (g_A, g_B, g_C, step, cached_h_coefficients) where:
  - g_A ∈ G1: encodes Σ a_i · u_i(τ) (the "A" polynomial evaluated at τ)
  - g_B ∈ G2: encodes Σ a_i · v_i(τ) (the "B" polynomial evaluated at τ)
  - g_C ∈ G1: encodes the witness contribution + quotient polynomial h(x)
  - step: which composition step j this proof covers
  - cached_h_coefficients: the h(x) coefficients, cached for incremental updates

The verifier checks the Groth16-style pairing equation:
  e(g_A, g_B) = e(α_g1, β_g2) · e(vk_x, γ_g2) · e(g_C, δ_g2)
where vk_x = Σ x_i · gamma_ABC_i accumulates public input contributions.

=== Prover tests ===

  Test 1 (well-formed):  g_A, g_B, g_C are valid curve points (on curve,
    in correct subgroup). Malformed points would cause pairing failures.

  Test 2 (step index):   Proof stores the correct step number, which the
    prover needs to select the right delta CRS elements for updates.

  Test 3 (cached h):     The quotient polynomial h(x) = (A·B - C) / Z has
    degree (domain_size - 2). All proofs should have the same h coefficient
    vector length (since the domain is fixed by C_B), and h should be nonzero.

  Test 4 (determinism):  The prover is deterministic: same (pk, step,
    primary, auxiliary) always produces the same proof. This is because the
    UVC scheme is non-ZK (no randomization on A, B, C).

=== Verifier tests (completeness) ===

  Test 5 (completeness):         All honest proofs verify for multiplier.
  Test 6 (completeness 2-state): All honest proofs verify for 2-state circuit.

=== Verifier tests (soundness) ===

  Test 7  (bad primary):    Corrupting any element of the primary input
    changes vk_x, breaking the pairing equation.

  Test 8  (bad g_A):        Random g_A breaks e(g_A, g_B) on the LHS.
  Test 9  (bad g_B):        Random g_B breaks e(g_A, g_B) on the LHS.
  Test 10 (bad g_C):        Random g_C breaks e(g_C, δ_g2) on the RHS.

  Test 11 (cross-trace):    A proof generated for one execution trace
    (s_0, t_1) is rejected when verified against a different (s_0', t_1').

=== Incremental proof tests ===

  Test 12 (incremental):    Verifies that incremental proof updates (step 2+)
    produce valid proofs across a 3-step chain.

  Test 13 (partial B):      A CRS with B=10 still produces valid proofs for
    just step 1, testing the zero-padding mechanism.

=== How to build and run ===

    cd build
    make test_uvc_prover_verifier
    ./test_uvc_prover_verifier

=== Expected output (key lines) ===

The binary generates proofs for a 3-step multiplier trace (s_0=3, t_1=5→15,
t_2=7→105, t_3=2→210) and tests proof properties and verifier behavior.

    ================================================================
    Unit tests: r1cs_uvc_ppzksnark_prover & verifier
    ================================================================

    --- test_proof_well_formed ---
      Step 1: well-formed=YES
      Step 2: well-formed=YES
      Step 3: well-formed=YES
      Result: PASS

    --- test_proof_step_index ---
      Proof 0: step=1 (exp 1)
      Proof 1: step=2 (exp 2)
      Proof 2: step=3 (exp 3)
      Result: PASS

    --- test_cached_h_size ---
      h coefficients sizes: step1=5 step2=5 step3=5  ← all same (domain-1)
      All same size: YES
      Step 1 h has nonzero coefficients: YES
      Result: PASS

    --- test_proof_determinism ---
      Step 1 determinism: A=same, B=same, C=same, h=same  ← non-ZK, no randomness
      Result: PASS

    --- test_completeness ---
      Step 1: verify=PASS    Step 2: verify=PASS    Step 3: verify=PASS
      Result: PASS

    --- test_completeness_two_state ---
      Step 1: verify=PASS    Step 2: verify=PASS
      Result: PASS

    --- test_soundness_bad_primary ---
      Step 1, corrupt primary[0]: rejected (PASS)    ← corrupted s_0
      Step 1, corrupt primary[1]: rejected (PASS)    ← corrupted t_1
      Step 2, corrupt primary[0]: rejected (PASS)
      Step 2, corrupt primary[1]: rejected (PASS)
      Step 3, corrupt primary[0]: rejected (PASS)
      Step 3, corrupt primary[1]: rejected (PASS)
      Result: PASS

    --- test_soundness_bad_g_A ---
      Step 1: corrupted g_A rejected (PASS)    ← random G1 point for g_A
      Step 2: corrupted g_A rejected (PASS)
      Step 3: corrupted g_A rejected (PASS)
      Result: PASS

    --- test_soundness_bad_g_B ---
      (same pattern: all rejected, PASS)

    --- test_soundness_bad_g_C ---
      (same pattern: all rejected, PASS)

    --- test_soundness_cross_trace ---
      Trace A self-verify: PASS                ← proof_A + primary_A
      Trace B self-verify: PASS                ← proof_B + primary_B
      Proof A with primary B: rejected (PASS)  ← proof_A + primary_B → FAIL
      Proof B with primary A: rejected (PASS)  ← proof_B + primary_A → FAIL
      Result: PASS

    --- test_incremental_valid ---
      Step 1 (base): verify=PASS
      Step 2 (incremental): verify=PASS
      Step 3 (incremental): verify=PASS
      Result: PASS

    --- test_partial_B ---
      B=10, step=1: verify=PASS     ← CRS supports 10 steps, only 1 used
      Result: PASS

    ================================================================
    r1cs_uvc_ppzksnark_prover & verifier: ALL PASSED  ← exit code 0
    ================================================================

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

template<typename FieldT>
state_transition_circuit<FieldT> make_multiplier()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 2;
    cs.auxiliary_input_size = 1;
    linear_combination<FieldT> A, B, C;
    A.add_term(1, FieldT::one());
    B.add_term(2, FieldT::one());
    C.add_term(3, FieldT::one());
    cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    return state_transition_circuit<FieldT>(cs, 1, 1);
}

template<typename FieldT>
state_transition_circuit<FieldT> make_two_state_mult()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 3;
    cs.auxiliary_input_size = 2;
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(1, FieldT::one());
        B.add_term(3, FieldT::one());
        C.add_term(4, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(2, FieldT::one());
        B.add_term(3, FieldT::one());
        C.add_term(5, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    return state_transition_circuit<FieldT>(cs, 2, 1);
}

/**
 * Test fixture: generates a complete 3-step multiplier execution and proofs.
 *
 * Creates the multiplier circuit (s_out = s_in * t), runs Setup with B=3,
 * and pre-computes all 3 proofs (step 1 = base, steps 2-3 = incremental).
 *
 * Execution trace: s_0=3 → s_1=15 → s_2=105 → s_3=210
 * (t_1=5, t_2=7, t_3=2)
 *
 * This fixture is reused by multiple tests to avoid redundant setup costs.
 * Each test then examines specific properties of the generated proofs.
 */
template<typename ppT>
struct multiplier_fixture {
    typedef libff::Fr<ppT> FieldT;

    state_transition_circuit<FieldT> st;
    r1cs_uvc_ppzksnark_keypair<ppT> kp;

    std::vector<std::vector<FieldT> > states;
    std::vector<std::vector<FieldT> > transitions;
    std::vector<std::vector<FieldT> > witnesses;

    /* Proofs and assignments per step */
    std::vector<r1cs_uvc_ppzksnark_proof<ppT> > proofs;
    std::vector<r1cs_uvc_ppzksnark_primary_input<ppT> > primaries;

    multiplier_fixture(bool bind_state = false)
    {
        st = make_multiplier<FieldT>();

        /* s_0=3, t_1=5->15, t_2=7->105, t_3=2->210 */
        states = { {FieldT(3)}, {FieldT(15)}, {FieldT(105)}, {FieldT(210)} };
        transitions = { {FieldT(5)}, {FieldT(7)}, {FieldT(2)} };
        witnesses = { {}, {}, {} };

        kp = r1cs_uvc_ppzksnark_generator<ppT>(st, 3, bind_state);

        for (size_t step = 1; step <= 3; ++step)
        {
            auto assign = build_composed_assignment(st, step, states, transitions, witnesses);
            primaries.push_back(assign.first);

            r1cs_uvc_ppzksnark_proof<ppT> proof = r1cs_uvc_ppzksnark_prover<ppT>(
                kp.pk, step, assign.first, assign.second,
                step > 1 ? &proofs[step - 2] : nullptr);
            proofs.push_back(proof);
        }
    }
};


/* ======================================================================== */
/* Test 1: Proof well-formedness (group elements on curve)                  */
/* ======================================================================== */

template<typename ppT>
bool test_proof_well_formed()
{
    printf("\n--- test_proof_well_formed ---\n");
    printf("  Checks that proof elements (g_A, g_B, g_C) are valid curve points.\n");
    printf("  Input:    3-step multiplier: s_0=3, t=[5,7,2] -> s=[15,105,210]\n");
    printf("  Expected: all 3 proofs well-formed = YES\n");

    multiplier_fixture<ppT> fix;
    bool pass = true;

    for (size_t i = 0; i < fix.proofs.size(); ++i)
    {
        bool wf = fix.proofs[i].is_well_formed();
        printf("  Step %zu: well-formed=%s\n", i + 1, wf ? "YES" : "NO");
        if (!wf) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 2: Step index stored correctly                                      */
/* ======================================================================== */

template<typename ppT>
bool test_proof_step_index()
{
    printf("\n--- test_proof_step_index ---\n");
    printf("  Checks that each proof stores the correct step number.\n");
    printf("  Input:    3 proofs from fixture\n");
    printf("  Expected: proof[0].step=1, proof[1].step=2, proof[2].step=3\n");

    multiplier_fixture<ppT> fix;
    bool pass = true;

    for (size_t i = 0; i < fix.proofs.size(); ++i)
    {
        bool ok = (fix.proofs[i].step == i + 1);
        printf("  Proof %zu: step=%zu (exp %zu) %s\n",
            i, fix.proofs[i].step, i + 1, ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 3: Cached h coefficients size                                       */
/* ======================================================================== */

template<typename ppT>
bool test_cached_h_size()
{
    printf("\n--- test_cached_h_size ---\n");
    printf("  Checks h(x) coefficient vector length and non-triviality.\n");
    printf("  Input:    3 proofs from fixture (B=3, QAP domain size = 6)\n");
    printf("  Expected: all h sizes = 5 (domain-1), step 1 has nonzero coefficients\n");

    multiplier_fixture<ppT> fix;
    bool pass = true;

    /* All h coefficients should have the same size (degree of C_B's domain - 1) */
    size_t first_size = fix.proofs[0].cached_h_coefficients.size();
    printf("  h coefficients sizes:");
    for (size_t i = 0; i < fix.proofs.size(); ++i)
    {
        printf(" step%zu=%zu", i + 1, fix.proofs[i].cached_h_coefficients.size());
        if (fix.proofs[i].cached_h_coefficients.size() != first_size) {
            pass = false;
        }
    }
    printf("\n  All same size: %s\n", pass ? "YES" : "NO");

    /* h coefficients should not be all zero */
    bool any_nonzero = false;
    for (const auto &c : fix.proofs[0].cached_h_coefficients) {
        if (!c.is_zero()) { any_nonzero = true; break; }
    }
    printf("  Step 1 h has nonzero coefficients: %s\n", any_nonzero ? "YES" : "NO");
    if (!any_nonzero) pass = false;

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 4: Proof determinism                                                */
/* ======================================================================== */

template<typename ppT>
bool test_proof_determinism()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_proof_determinism ---\n");
    printf("  Proves the same statement twice with the same key; proofs must be identical.\n");
    printf("  Input:    s_0=3, t_1=5 -> s_1=15 (proved twice with same pk)\n");
    printf("  Expected: g_A, g_B, g_C, h coefficients all identical (non-ZK = no randomness)\n");

    /* Note: The generator uses random elements, so we need to use
       the same keypair for both proof generations.
       But r1cs_to_qap_witness_map is deterministic given the same CS and inputs. */
    auto st = make_multiplier<FieldT>();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)2);

    std::vector<std::vector<FieldT> > states = { {FieldT(3)}, {FieldT(15)}, {FieldT(105)} };
    std::vector<std::vector<FieldT> > transitions = { {FieldT(5)}, {FieldT(7)} };
    std::vector<std::vector<FieldT> > witnesses = { {}, {} };

    auto assign1 = build_composed_assignment(st, (size_t)1, states, transitions, witnesses);
    auto proof1a = r1cs_uvc_ppzksnark_prover<ppT>(kp.pk, 1, assign1.first, assign1.second, nullptr);
    auto proof1b = r1cs_uvc_ppzksnark_prover<ppT>(kp.pk, 1, assign1.first, assign1.second, nullptr);

    bool same_A = (proof1a.g_A == proof1b.g_A);
    bool same_B = (proof1a.g_B == proof1b.g_B);
    bool same_C = (proof1a.g_C == proof1b.g_C);
    bool same_h = (proof1a.cached_h_coefficients == proof1b.cached_h_coefficients);

    printf("  Step 1 determinism: A=%s, B=%s, C=%s, h=%s\n",
        same_A ? "same" : "DIFF", same_B ? "same" : "DIFF",
        same_C ? "same" : "DIFF", same_h ? "same" : "DIFF");

    bool pass = same_A && same_B && same_C && same_h;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 5: Completeness — all valid proofs verify                           */
/* ======================================================================== */

template<typename ppT>
bool test_completeness()
{
    printf("\n--- test_completeness ---\n");
    printf("  Verifies all honest proofs pass the pairing check.\n");
    printf("  Input:    3-step multiplier proofs from fixture\n");
    printf("  Expected: all 3 steps verify = PASS\n");

    multiplier_fixture<ppT> fix;
    bool pass = true;

    for (size_t i = 0; i < fix.proofs.size(); ++i)
    {
        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, i + 1, fix.primaries[i], fix.proofs[i]);
        printf("  Step %zu: verify=%s\n", i + 1, v ? "PASS" : "FAIL");
        if (!v) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 6: Completeness for two-state circuit                               */
/* ======================================================================== */

template<typename ppT>
bool test_completeness_two_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_completeness_two_state ---\n");
    printf("  Completeness with 2-element state vector.\n");
    printf("  Input:    (a_0,b_0)=(2,3), t=[5,3] -> (10,15) -> (30,45)\n");
    printf("  Expected: both steps verify = PASS\n");

    auto st = make_two_state_mult<FieldT>();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)3);

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)},
        {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(45)},
    };
    std::vector<std::vector<FieldT> > transitions = { {FieldT(5)}, {FieldT(3)} };
    std::vector<std::vector<FieldT> > witnesses = { {}, {} };

    bool pass = true;
    r1cs_uvc_ppzksnark_proof<ppT> prev_proof;
    bool have_prev = false;

    for (size_t step = 1; step <= 2; ++step)
    {
        auto assign = build_composed_assignment(st, step, states, transitions, witnesses);
        auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, step, assign.first, assign.second,
            have_prev ? &prev_proof : nullptr);

        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, step, assign.first, proof);
        printf("  Step %zu: verify=%s\n", step, v ? "PASS" : "FAIL");
        if (!v) pass = false;

        prev_proof = proof;
        have_prev = true;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 7: Soundness — bad primary input rejected                           */
/* ======================================================================== */

template<typename ppT>
bool test_soundness_bad_primary()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_soundness_bad_primary ---\n");
    printf("  Corrupts each element of primary input with a random value.\n");
    printf("  Input:    valid proofs, then replace primary[k] with random for each k\n");
    printf("  Expected: all corrupted inputs rejected by verifier\n");

    multiplier_fixture<ppT> fix;
    bool pass = true;

    for (size_t i = 0; i < fix.proofs.size(); ++i)
    {
        /* Corrupt each primary input element */
        for (size_t k = 0; k < fix.primaries[i].size(); ++k)
        {
            auto bad_primary = fix.primaries[i];
            bad_primary[k] = FieldT::random_element();

            bool v = r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, i + 1, bad_primary, fix.proofs[i]);
            printf("  Step %zu, corrupt primary[%zu]: %s\n",
                i + 1, k, !v ? "rejected (PASS)" : "accepted (FAIL)");
            if (v) pass = false;
        }
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 8: Soundness — corrupted g_A rejected                               */
/* ======================================================================== */

template<typename ppT>
bool test_soundness_bad_g_A()
{
    printf("\n--- test_soundness_bad_g_A ---\n");
    printf("  Replaces g_A with a random G1 point, breaking e(g_A, g_B) on LHS.\n");
    printf("  Input:    valid proof with g_A replaced by G1::random_element()\n");
    printf("  Expected: all corrupted proofs rejected\n");

    multiplier_fixture<ppT> fix;
    bool pass = true;

    for (size_t i = 0; i < fix.proofs.size(); ++i)
    {
        auto bad_proof = fix.proofs[i];
        bad_proof.g_A = libff::G1<ppT>::random_element();

        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, i + 1, fix.primaries[i], bad_proof);
        printf("  Step %zu: corrupted g_A %s\n",
            i + 1, !v ? "rejected (PASS)" : "accepted (FAIL)");
        if (v) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 9: Soundness — corrupted g_B rejected                               */
/* ======================================================================== */

template<typename ppT>
bool test_soundness_bad_g_B()
{
    printf("\n--- test_soundness_bad_g_B ---\n");
    printf("  Replaces g_B with a random G2 point, breaking e(g_A, g_B) on LHS.\n");
    printf("  Input:    valid proof with g_B replaced by G2::random_element()\n");
    printf("  Expected: all corrupted proofs rejected\n");

    multiplier_fixture<ppT> fix;
    bool pass = true;

    for (size_t i = 0; i < fix.proofs.size(); ++i)
    {
        auto bad_proof = fix.proofs[i];
        bad_proof.g_B = libff::G2<ppT>::random_element();

        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, i + 1, fix.primaries[i], bad_proof);
        printf("  Step %zu: corrupted g_B %s\n",
            i + 1, !v ? "rejected (PASS)" : "accepted (FAIL)");
        if (v) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 10: Soundness — corrupted g_C rejected                              */
/* ======================================================================== */

template<typename ppT>
bool test_soundness_bad_g_C()
{
    printf("\n--- test_soundness_bad_g_C ---\n");
    printf("  Replaces g_C with a random G1 point, breaking e(g_C, delta) on RHS.\n");
    printf("  Input:    valid proof with g_C replaced by G1::random_element()\n");
    printf("  Expected: all corrupted proofs rejected\n");

    multiplier_fixture<ppT> fix;
    bool pass = true;

    for (size_t i = 0; i < fix.proofs.size(); ++i)
    {
        auto bad_proof = fix.proofs[i];
        bad_proof.g_C = libff::G1<ppT>::random_element();

        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, i + 1, fix.primaries[i], bad_proof);
        printf("  Step %zu: corrupted g_C %s\n",
            i + 1, !v ? "rejected (PASS)" : "accepted (FAIL)");
        if (v) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 11: Soundness — proof from different execution trace rejected        */
/*                                                                          */
/* In UVC, the primary input [s_0, t_1] is fixed across all steps within   */
/* one trace, so swapping proofs across steps of the SAME trace would use   */
/* the same primary and trivially pass (if the pairing equations happen to  */
/* coincide). Instead, this test creates TWO independent execution traces   */
/* with DIFFERENT (s_0, t_1) values:                                        */
/*                                                                          */
/*   Trace A: s_0=3, t_1=5, s_1=15                                         */
/*   Trace B: s_0=7, t_1=2, s_1=14                                         */
/*                                                                          */
/* It then verifies:                                                        */
/*   1. Each proof verifies against its own primary (self-verify: pass)     */
/*   2. proof_A rejects when given primary_B (cross-verify: fail)           */
/*   3. proof_B rejects when given primary_A (cross-verify: fail)           */
/*                                                                          */
/* This tests that the verifier's input accumulator vk_x properly binds    */
/* the proof to the specific primary input values.                          */
/* ======================================================================== */

template<typename ppT>
bool test_soundness_cross_trace()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_soundness_cross_trace ---\n");
    printf("  Creates two independent traces with different (s_0, t_1) values.\n");
    printf("  Input:    Trace A: s_0=3, t_1=5, s_1=15; Trace B: s_0=7, t_1=2, s_1=14\n");
    printf("  Expected: self-verify PASS, cross-verify (proof_A + primary_B) rejected\n");

    auto st = make_multiplier<FieldT>();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)2);

    /* Trace A: s_0=3, t_1=5, s_1=15 */
    std::vector<std::vector<FieldT> > states_A = { {FieldT(3)}, {FieldT(15)} };
    std::vector<std::vector<FieldT> > trans_A = { {FieldT(5)} };
    std::vector<std::vector<FieldT> > wit_A = { {} };

    auto assign_A = build_composed_assignment(st, (size_t)1, states_A, trans_A, wit_A);
    auto proof_A = r1cs_uvc_ppzksnark_prover<ppT>(
        kp.pk, 1, assign_A.first, assign_A.second, nullptr);

    /* Trace B: s_0=7, t_1=2, s_1=14 (different initial state and transition) */
    std::vector<std::vector<FieldT> > states_B = { {FieldT(7)}, {FieldT(14)} };
    std::vector<std::vector<FieldT> > trans_B = { {FieldT(2)} };
    std::vector<std::vector<FieldT> > wit_B = { {} };

    auto assign_B = build_composed_assignment(st, (size_t)1, states_B, trans_B, wit_B);
    auto proof_B = r1cs_uvc_ppzksnark_prover<ppT>(
        kp.pk, 1, assign_B.first, assign_B.second, nullptr);

    bool pass = true;

    /* Self-verify should work */
    bool v_AA = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 1, assign_A.first, proof_A);
    bool v_BB = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 1, assign_B.first, proof_B);
    printf("  Trace A self-verify: %s\n", v_AA ? "PASS" : "FAIL");
    printf("  Trace B self-verify: %s\n", v_BB ? "PASS" : "FAIL");
    if (!v_AA || !v_BB) pass = false;

    /* Cross-verify should fail: proof_A with primary_B, and vice versa */
    bool v_AB = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 1, assign_B.first, proof_A);
    bool v_BA = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 1, assign_A.first, proof_B);
    printf("  Proof A with primary B: %s\n", !v_AB ? "rejected (PASS)" : "accepted (FAIL)");
    printf("  Proof B with primary A: %s\n", !v_BA ? "rejected (PASS)" : "accepted (FAIL)");
    if (v_AB || v_BA) pass = false;

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 12: Incremental vs base: step 2 incremental produces valid proof    */
/* ======================================================================== */

template<typename ppT>
bool test_incremental_valid()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_incremental_valid ---\n");
    printf("  Runs a 3-step chain: base proof at step 1, incremental at steps 2-3.\n");
    printf("  Input:    s_0=2, t=[5,3,2] -> s=[10,30,60]\n");
    printf("  Expected: step 1 (base) PASS, step 2 (incremental) PASS, step 3 (incremental) PASS\n");

    auto st = make_multiplier<FieldT>();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)3);

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)}, {FieldT(10)}, {FieldT(30)}, {FieldT(60)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(3)}, {FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {}, {} };

    bool pass = true;
    r1cs_uvc_ppzksnark_proof<ppT> prev;
    bool have_prev = false;

    for (size_t step = 1; step <= 3; ++step)
    {
        auto assign = build_composed_assignment(st, step, states, transitions, witnesses);
        auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, step, assign.first, assign.second,
            have_prev ? &prev : nullptr);

        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, step, assign.first, proof);
        printf("  Step %zu (%s): verify=%s\n",
            step, step == 1 ? "base" : "incremental", v ? "PASS" : "FAIL");
        if (!v) pass = false;

        prev = proof;
        have_prev = true;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 13: Proof with B > steps (partial use of allocated compositions)    */
/* ======================================================================== */

template<typename ppT>
bool test_partial_B()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_partial_B ---\n");
    printf("  CRS supports B=10 compositions but only step 1 is proved.\n");
    printf("  Input:    s_0=2, t_1=3 -> s_1=6 (B=10, step=1)\n");
    printf("  Expected: verify PASS (zero-padded wires for steps 2..10)\n");

    auto st = make_multiplier<FieldT>();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)10);

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)}, {FieldT(6)}
    };
    std::vector<std::vector<FieldT> > transitions = { {FieldT(3)} };
    std::vector<std::vector<FieldT> > witnesses = { {} };

    auto assign = build_composed_assignment(st, (size_t)1, states, transitions, witnesses);
    auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
        kp.pk, 1, assign.first, assign.second, nullptr);

    bool v = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 1, assign.first, proof);
    printf("  B=10, step=1: verify=%s\n", v ? "PASS" : "FAIL");

    printf("  Result: %s\n", v ? "PASS" : "FAIL");
    return v;
}
/* ======================================================================== */
/* Test 14: Running eta-track state commitment                               */
/* ======================================================================== */

template<typename ppT>
bool test_state_commitment_chain_for_circuit(
    const char *name,
    const state_transition_circuit<libff::Fr<ppT> > &st,
    const std::vector<std::vector<libff::Fr<ppT> > > &states,
    const std::vector<std::vector<libff::Fr<ppT> > > &transitions)
{
    typedef libff::Fr<ppT> FieldT;
    const size_t B = transitions.size();
    const size_t ss = st.state_size;
    const std::vector<std::vector<FieldT> > witnesses(B);

    printf("  %s (state_size=%zu):\n", name, ss);
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B, true);
    bool pass = true;
    r1cs_uvc_ppzksnark_proof<ppT> prev;
    bool have_prev = false;

    for (size_t step = 1; step <= B; ++step)
    {
        const auto assign = build_composed_assignment(st, step, states, transitions, witnesses);
        const auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, step, assign.first, assign.second, have_prev ? &prev : nullptr);

        libff::G1<ppT> expected_delta = libff::G1<ppT>::zero();
        for (size_t i = 0; i < ss; ++i) {
            expected_delta = expected_delta +
                states[step][i] * kp.vk.st_ABC_g1[(step - 1) * ss + i];
        }

        const bool bound = proof.bind_state;
        const bool identity = step == 1
            ? proof.g_D == expected_delta
            : proof.g_D - prev.g_D == expected_delta;
        const bool changed = !have_prev || proof.g_D != prev.g_D;
        printf("    Step %zu: bind_state=%s, %s identity=%s, g_D changes=%s\n",
               step, bound ? "PASS" : "FAIL", step == 1 ? "base" : "increment",
               identity ? "PASS" : "FAIL", changed ? "PASS" : "FAIL");
        pass &= bound && identity && changed;

        prev = proof;
        have_prev = true;
    }

    return pass;
}

template<typename ppT>
bool test_state_commitment_chain()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_state_commitment_chain ---\n");
    printf("  Checks eta-track D_j base and incremental commitment identities.\n");

    const auto multiplier = make_multiplier<FieldT>();
    const std::vector<std::vector<FieldT> > multiplier_states = {
        {FieldT(3)}, {FieldT(15)}, {FieldT(105)}, {FieldT(210)}
    };
    const std::vector<std::vector<FieldT> > multiplier_transitions = {
        {FieldT(5)}, {FieldT(7)}, {FieldT(2)}
    };

    bool pass = test_state_commitment_chain_for_circuit<ppT>(
        "multiplier", multiplier, multiplier_states, multiplier_transitions);

    const auto two_state = make_two_state_mult<FieldT>();
    const std::vector<std::vector<FieldT> > two_state_states = {
        {FieldT(2), FieldT(3)}, {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(45)}, {FieldT(60), FieldT(90)}
    };
    const std::vector<std::vector<FieldT> > two_state_transitions = {
        {FieldT(5)}, {FieldT(3)}, {FieldT(2)}
    };
    pass &= test_state_commitment_chain_for_circuit<ppT>(
        "two-state multiplier", two_state, two_state_states, two_state_transitions);

    const std::vector<std::vector<FieldT> > witnesses(3);
    auto off_kp = r1cs_uvc_ppzksnark_generator<ppT>(multiplier, 3, false);
    r1cs_uvc_ppzksnark_proof<ppT> prev;
    bool have_prev = false;
    for (size_t step = 1; step <= 3; ++step) {
        const auto assign = build_composed_assignment(
            multiplier, step, multiplier_states, multiplier_transitions, witnesses);
        const auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            off_kp.pk, step, assign.first, assign.second, have_prev ? &prev : nullptr);
        const bool off_shape = !proof.bind_state &&
            proof.g_D == libff::G1<ppT>::zero() && proof.G1_size() == 2;
        printf("  Off-path step %zu: 2-G1 and zero g_D=%s\n",
               step, off_shape ? "PASS" : "FAIL");
        pass &= off_shape;
        prev = proof;
        have_prev = true;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}
/* ======================================================================== */
/* Tests 15-18: State-bound verifier                                        */
/* ======================================================================== */

template<typename ppT>
bool verify_state_bound_chain(
    const state_transition_circuit<libff::Fr<ppT> > &st,
    const std::vector<std::vector<libff::Fr<ppT> > > &states,
    const std::vector<std::vector<libff::Fr<ppT> > > &transitions)
{
    typedef libff::Fr<ppT> FieldT;
    const size_t B = transitions.size();
    const std::vector<std::vector<FieldT> > witnesses(B);
    const auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B, true);
    libff::G1<ppT> D_prev = libff::G1<ppT>::zero();
    r1cs_uvc_ppzksnark_proof<ppT> previous_proof;
    bool pass = true;

    for (size_t step = 1; step <= B; ++step) {
        const auto assign = build_composed_assignment(st, step, states, transitions, witnesses);
        const auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, step, assign.first, assign.second,
            step == 1 ? nullptr : &previous_proof);
        const bool accepted = r1cs_uvc_ppzksnark_verifier<ppT>(
            kp.vk, step, assign.first, states[step], proof, D_prev);
        printf("  Step %zu: state-bound verify=%s\n", step, accepted ? "PASS" : "FAIL");
        pass &= accepted;
        if (accepted) {
            D_prev = proof.g_D;
        }
        previous_proof = proof;
    }

    return pass;
}

template<typename ppT>
bool test_state_bound_verifier_completeness()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_state_bound_verifier_completeness ---\n");

    const std::vector<std::vector<FieldT> > multiplier_states = {
        {FieldT(3)}, {FieldT(15)}, {FieldT(105)}, {FieldT(210)}
    };
    const std::vector<std::vector<FieldT> > multiplier_transitions = {
        {FieldT(5)}, {FieldT(7)}, {FieldT(2)}
    };
    const std::vector<std::vector<FieldT> > two_state_states = {
        {FieldT(2), FieldT(3)}, {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(45)}, {FieldT(60), FieldT(90)}
    };
    const std::vector<std::vector<FieldT> > two_state_transitions = {
        {FieldT(5)}, {FieldT(3)}, {FieldT(2)}
    };

    const bool pass =
        verify_state_bound_chain<ppT>(
            make_multiplier<FieldT>(), multiplier_states, multiplier_transitions) &&
        verify_state_bound_chain<ppT>(
            make_two_state_mult<FieldT>(), two_state_states, two_state_transitions);
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_state_bound_negative_wrong_reported_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_state_bound_negative_wrong_reported_state ---\n");
    multiplier_fixture<ppT> fix(true);

    std::vector<FieldT> reported = fix.states[2];
    reported[0] += FieldT::one();
    const bool rejected = !r1cs_uvc_ppzksnark_verifier<ppT>(
        fix.kp.vk, 2, fix.primaries[1], reported, fix.proofs[1], fix.proofs[0].g_D);
    printf("  Wrong reported state rejected=%s\n", rejected ? "PASS" : "FAIL");
    return rejected;
}

template<typename ppT>
bool test_state_bound_negative_tampered_gD()
{
    printf("\n--- test_state_bound_negative_tampered_gD ---\n");
    multiplier_fixture<ppT> fix(true);

    r1cs_uvc_ppzksnark_proof<ppT> tampered = fix.proofs[1];
    const libff::G1<ppT> delta = libff::G1<ppT>::one();
    tampered.g_D = tampered.g_D + delta;
    const bool tampered_rejected = !r1cs_uvc_ppzksnark_verifier<ppT>(
        fix.kp.vk, 2, fix.primaries[1], fix.states[2], tampered, fix.proofs[0].g_D);

    const libff::G1<ppT> forged_D_prev = fix.proofs[0].g_D + delta;
    const bool wrong_track_rejected = !r1cs_uvc_ppzksnark_verifier<ppT>(
        fix.kp.vk, 2, fix.primaries[1], fix.states[2], tampered, forged_D_prev);
    const bool pass = tampered_rejected && wrong_track_rejected;
    printf("  Tampered g_D rejected=%s, consistent wrong track rejected=%s\n",
           tampered_rejected ? "PASS" : "FAIL", wrong_track_rejected ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_state_bound_mismatched_step()
{
    printf("\n--- test_state_bound_mismatched_step ---\n");
    multiplier_fixture<ppT> fix(true);

    const bool rejected = !r1cs_uvc_ppzksnark_verifier<ppT>(
        fix.kp.vk, 3, fix.primaries[1], fix.states[2], fix.proofs[1], fix.proofs[0].g_D);
    printf("  Proof 2 verified as step 3 rejected=%s\n", rejected ? "PASS" : "FAIL");
    return rejected;
}



/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */

int main()
{
    libff::alt_bn128_pp::init_public_params();
    libff::inhibit_profiling_info = true;

    printf("================================================================\n");
    printf("Unit tests: r1cs_uvc_ppzksnark_prover & verifier\n");
    printf("================================================================\n");

    bool all_pass = true;

    /* Prover tests */
    all_pass &= test_proof_well_formed<libff::alt_bn128_pp>();
    all_pass &= test_proof_step_index<libff::alt_bn128_pp>();
    all_pass &= test_cached_h_size<libff::alt_bn128_pp>();
    all_pass &= test_proof_determinism<libff::alt_bn128_pp>();

    /* Completeness tests */
    all_pass &= test_completeness<libff::alt_bn128_pp>();
    all_pass &= test_completeness_two_state<libff::alt_bn128_pp>();

    /* Soundness tests */
    all_pass &= test_soundness_bad_primary<libff::alt_bn128_pp>();
    all_pass &= test_soundness_bad_g_A<libff::alt_bn128_pp>();
    all_pass &= test_soundness_bad_g_B<libff::alt_bn128_pp>();
    all_pass &= test_soundness_bad_g_C<libff::alt_bn128_pp>();
    all_pass &= test_soundness_cross_trace<libff::alt_bn128_pp>();

    /* Incremental tests */
    all_pass &= test_incremental_valid<libff::alt_bn128_pp>();
    all_pass &= test_partial_B<libff::alt_bn128_pp>();
    all_pass &= test_state_commitment_chain<libff::alt_bn128_pp>();
    all_pass &= test_state_bound_verifier_completeness<libff::alt_bn128_pp>();
    all_pass &= test_state_bound_negative_wrong_reported_state<libff::alt_bn128_pp>();
    all_pass &= test_state_bound_negative_tampered_gD<libff::alt_bn128_pp>();
    all_pass &= test_state_bound_mismatched_step<libff::alt_bn128_pp>();

    printf("\n================================================================\n");
    printf("r1cs_uvc_ppzksnark_prover & verifier: %s\n", all_pass ? "ALL PASSED" : "SOME FAILED");
    printf("================================================================\n");
    return all_pass ? 0 : 1;
}
