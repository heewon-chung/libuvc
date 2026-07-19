/** @file
*****************************************************************************

Unit tests for r1cs_uvc_ppzksnark_generator() — the UVC.Setup algorithm.

The generator (Setup) takes a state transition circuit C and a bound B,
then constructs the B-fold composed circuit C_B and runs the Groth16-style
key generation over C_B's QAP. The output consists of:

  - **Proving key (pk)**: Contains base QAP evaluations [u_i(τ)]₁, [v_i(τ)]₂,
    [w_i(τ)]₁ for all wires in C_B, plus per-step delta data that stores the
    CRS elements for wires introduced at each step j = 2..B. These deltas
    enable the prover to incrementally update proof components.

  - **Verification key (vk)**: Contains [α]₁, [β]₂, [γ]₂, [δ]₂, and
    gamma_ABC = {[β·u_i(τ) + α·v_i(τ) + w_i(τ)]₁ / γ} for public input
    wires only. The verifier uses these for the pairing check.

=== What these tests verify ===

  Test 1 (Key dimensions): For varying B, checks that A_query, B_query,
    L_query have sizes matching the total number of wires in C_B.
    Formula: total_vars = n + (B-1) * (n - state_size)
    where n = wires per step, and (n - state_size) new wires per additional step.

  Test 2 (max_compositions): Confirms that the B parameter is stored
    correctly in both pk and vk, so the prover/verifier know the CRS bounds.

  Test 3 (Per-step data dimensions): Verifies step_data[j-2] for steps 2..B
    contains correctly-sized delta CRS elements:
    - new_wire_start: index of first new wire at step j
    - new_wire_count: how many new wires step j introduces
    - A/B/L_query_delta: CRS elements for exactly the new wires

  Test 4 (Two-state step data): Same as Test 3 but with state_size=2,
    confirming new_per_step = n - 2 = 3 wires per additional step.

  Test 5 (VK gamma_ABC size): Checks that gamma_ABC has exactly
    (state_size + transition_size) entries — one per public input wire.
    These are the wires visible to the verifier: s_0 and t_1.

  Test 6 (B=1 edge case): With B=1, there is only the base circuit C_1
    and no incremental steps, so step_data should be empty.

  Test 7 (Stored constraint system): Verifies that the constraint system
    stored in the proving key is C_B (the full composed circuit), with
    the correct number of variables and constraints.

=== How to build and run ===

    cd build
    make test_uvc_generator
    ./test_uvc_generator

=== Expected output (key lines) ===

The binary runs Setup for various B values and circuit types, then checks
that the generated key dimensions match expected formulas.

    ================================================================
    Unit tests: r1cs_uvc_ppzksnark_generator (UVC.Setup)
    ================================================================

    --- test_key_dimensions_multiplier ---
      B=1: A_query=4 (exp 4) , B_query=4 (exp 4) , L_query=1 (exp 1)
      B=2: A_query=6 (exp 6) , B_query=6 (exp 6) , L_query=3 (exp 3)
      B=3: A_query=8 (exp 8) , B_query=8 (exp 8) , L_query=5 (exp 5)
      B=4: A_query=10 (exp 10) , B_query=10 (exp 10) , L_query=7 (exp 7)
      Result: PASS
      ← A/B_query = total_vars+1 = (2B+1)+1; L_query = total_vars - 2

    --- test_max_compositions ---
      B=1: pk.max=1, vk.max=1
      ...
      B=5: pk.max=5, vk.max=5
      Result: PASS

    --- test_step_data_dimensions ---
      step_data count=3 (exp 3)       ← B-1 entries for steps 2..B
      Step 2: start=4 (exp 4), count=2 (exp 2), A_delta=2, B_delta=2, L_delta=2
      Step 3: start=6 (exp 6), count=2 (exp 2), A_delta=2, B_delta=2, L_delta=2
      Step 4: start=8 (exp 8), count=2 (exp 2), A_delta=2, B_delta=2, L_delta=2
      Result: PASS
      ← Each step adds 2 new wires (new_per_step = n - ss = 3 - 1 = 2)

    --- test_step_data_two_state ---
      step_data count=2 (exp 2)
      Step 2: count=3 (exp 3), A=3, L=3
      Step 3: count=3 (exp 3), A=3, L=3
      Result: PASS
      ← Two-state: new_per_step = 5 - 2 = 3

    --- test_vk_gamma_abc_size ---
      Multiplier: gamma_ABC size=2 (exp 2)    ← ss + ts = 1 + 1
      Two-state: gamma_ABC size=3 (exp 3)     ← ss + ts = 2 + 1
      Result: PASS

    --- test_b_equals_1 ---
      step_data size=0 (exp 0), max=1 (exp 1)  ← no deltas for B=1
      Result: PASS

    --- test_stored_constraint_system ---
      Stored CS: vars=7 (exp 7), constraints=3 (exp 3), inputs=2 (exp 2)
      Result: PASS
      ← C_3: vars = 3 + 2*2 = 7, constraints = 3*1 = 3

    ================================================================
    r1cs_uvc_ppzksnark_generator: ALL PASSED  ← exit code 0
    ================================================================

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
#include <stdexcept>

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


/* ======================================================================== */
/* Test 1: Key dimensions for multiplier with various B                     */
/*                                                                          */
/* The multiplier circuit has n=3 wires, state_size=1, so each additional   */
/* step adds new_per_step = 3 - 1 = 2 wires. For B compositions:           */
/*   total_vars_B = 3 + (B-1)*2 = 2B + 1                                   */
/*   num_constraints_B = B (one constraint per step)                        */
/*   A_query: total_vars_B + 1 elements (indices 0..total_vars_B)           */
/*   B_query: same size                                                     */
/*   L_query: total_vars_B - num_inputs (witness wires only)                */
/* ======================================================================== */

template<typename ppT>
bool test_key_dimensions_multiplier()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_key_dimensions_multiplier ---\n");
    printf("  Checks A/B/L query sizes in the proving key for B=1..4.\n");
    printf("  Input:    multiplier circuit (n=3, ss=1, ts=1)\n");
    printf("  Expected: A_query = 2B+2, B_query = 2B+2, L_query = 2B-1\n");

    auto st = make_multiplier<FieldT>();
    const size_t n = 3, ss = 1, ts = 1;
    const size_t new_per_step = n - ss;
    bool pass = true;

    for (size_t B = 1; B <= 4; ++B)
    {
        auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

        size_t total_vars_B = n + (B - 1) * new_per_step;
        size_t num_inputs_B = ss + ts;
        size_t num_constraints_B = B * 1;

        /* A query: total_vars_B + 1 elements (indices 0..total_vars_B) */
        size_t expected_A = total_vars_B + 1;
        /* B query: same count */
        size_t expected_B = total_vars_B + 1;
        /* L query: witness wires = total_vars_B - num_inputs_B */
        size_t expected_L = total_vars_B - num_inputs_B;
        /* H query: degree - 2, where degree = domain_size >= num_constraints_B + num_inputs_B + 1
           H_query size = degree - 2 after Ht.resize(Ht.size()-2) then batch_exp.
           Actual degree is smallest power of 2 >= num_constraints_B + num_inputs_B + 1.
           Ht originally has degree+1 elements, after resize it has degree-1 elements. */

        bool ok_A = (kp.pk.base_pk.A_query.size() == expected_A);
        bool ok_B = (kp.pk.base_pk.B_query.domain_size_ == expected_B);
        bool ok_L = (kp.pk.base_pk.L_query.size() == expected_L);

        printf("  B=%zu: A_query=%zu (exp %zu) %s, B_query=%zu (exp %zu) %s, L_query=%zu (exp %zu) %s\n",
            B,
            kp.pk.base_pk.A_query.size(), expected_A, ok_A ? "" : "FAIL",
            kp.pk.base_pk.B_query.domain_size_, expected_B, ok_B ? "" : "FAIL",
            kp.pk.base_pk.L_query.size(), expected_L, ok_L ? "" : "FAIL");

        if (!ok_A || !ok_B || !ok_L) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 2: max_compositions stored correctly                                */
/* ======================================================================== */

template<typename ppT>
bool test_max_compositions()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_max_compositions ---\n");
    printf("  Checks that B is stored correctly in both pk and vk.\n");
    printf("  Input:    multiplier circuit, B=1..5\n");
    printf("  Expected: pk.max_compositions = vk.max_compositions = B\n");

    auto st = make_multiplier<FieldT>();
    bool pass = true;

    for (size_t B = 1; B <= 5; ++B)
    {
        auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);
        bool ok = (kp.pk.max_compositions == B) && (kp.vk.max_compositions == B);
        printf("  B=%zu: pk.max=%zu, vk.max=%zu %s\n",
            B, kp.pk.max_compositions, kp.vk.max_compositions, ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 3: Per-step data dimensions                                         */
/*                                                                          */
/* step_data is the key UVC-specific structure in the proving key. It       */
/* stores, for each step j = 2..B, the CRS elements needed to update       */
/* the proof from step j-1 to step j:                                       */
/*   - new_wire_start: first 1-indexed wire introduced at step j            */
/*   - new_wire_count: number of new wires (= n - state_size)               */
/*   - A_query_delta: [u_i(τ)]₁ for new wire indices only                  */
/*   - B_query_delta: [v_i(τ)]₂ for new wire indices only                  */
/*   - L_query_delta: [(β·u_i + α·v_i + w_i)(τ)/δ]₁ for new witnesses     */
/*                                                                          */
/* For the multiplier (n=3, ss=1), new_per_step=2, and:                     */
/*   Step 2: new_wire_start = 4 (wires 4,5 = t_2, s_2)                     */
/*   Step 3: new_wire_start = 6 (wires 6,7 = t_3, s_3)                     */
/* ======================================================================== */

template<typename ppT>
bool test_step_data_dimensions()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_step_data_dimensions ---\n");
    printf("  Checks per-step delta CRS: new_wire_start, new_wire_count, delta query sizes.\n");
    printf("  Input:    multiplier circuit, B=4 -> step_data for steps 2,3,4\n");
    printf("  Expected: each step adds 2 new wires starting at 4,6,8; delta sizes all = 2\n");

    auto st = make_multiplier<FieldT>();
    const size_t n = 3, ss = 1;
    const size_t new_per_step = n - ss;
    const size_t B = 4;

    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

    bool pass = true;

    /* step_data has B-1 entries (for steps 2..B) */
    bool ok_count = (kp.pk.step_data.size() == B - 1);
    printf("  step_data count=%zu (exp %zu) %s\n",
        kp.pk.step_data.size(), B - 1, ok_count ? "" : "FAIL");
    if (!ok_count) { pass = false; }

    for (size_t i = 0; i < kp.pk.step_data.size(); ++i)
    {
        size_t step = i + 2;
        const auto &sd = kp.pk.step_data[i];

        size_t expected_start = n + i * new_per_step + 1;
        size_t expected_count = new_per_step;

        bool ok = (sd.new_wire_start == expected_start) &&
                  (sd.new_wire_count == expected_count) &&
                  (sd.new_io_count == 0) &&
                  (sd.new_st_count == ss) &&
                  (sd.new_wt_count == expected_count - ss) &&
                  (sd.A_query_delta.size() == expected_count) &&
                  (sd.B_query_delta.domain_size_ == expected_count) &&
                  (sd.L_query_delta.size() == expected_count) &&
                  (sd.st_query_delta.size() == ss);

        printf("  Step %zu: start=%zu (exp %zu), count=%zu (exp %zu), "
               "A_delta=%zu, B_delta=%zu, L_delta=%zu %s\n",
            step, sd.new_wire_start, expected_start,
            sd.new_wire_count, expected_count,
            sd.A_query_delta.size(), sd.B_query_delta.domain_size_,
            sd.L_query_delta.size(),
            ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 4: Per-step data for two-state circuit                              */
/* ======================================================================== */

template<typename ppT>
bool test_step_data_two_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_step_data_two_state ---\n");
    printf("  Same as above but with two-state circuit (n=5, ss=2).\n");
    printf("  Input:    two-state circuit, B=3 -> step_data for steps 2,3\n");
    printf("  Expected: each step adds 3 new wires (new_per_step = 5 - 2 = 3)\n");

    auto st = make_two_state_mult<FieldT>();
    const size_t n = 5, ss = 2;
    const size_t new_per_step = n - ss; /* = 3 */
    const size_t B = 3;

    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

    bool pass = true;

    bool ok_count = (kp.pk.step_data.size() == B - 1);
    printf("  step_data count=%zu (exp %zu) %s\n",
        kp.pk.step_data.size(), B - 1, ok_count ? "" : "FAIL");
    if (!ok_count) pass = false;

    for (size_t i = 0; i < kp.pk.step_data.size(); ++i)
    {
        const auto &sd = kp.pk.step_data[i];
        bool ok = (sd.new_wire_count == new_per_step) &&
                  (sd.new_io_count == 0) &&
                  (sd.new_st_count == ss) &&
                  (sd.new_wt_count == new_per_step - ss) &&
                  (sd.A_query_delta.size() == new_per_step) &&
                  (sd.L_query_delta.size() == new_per_step) &&
                  (sd.st_query_delta.size() == ss);
        printf("  Step %zu: count=%zu (exp %zu), A=%zu, L=%zu %s\n",
            i + 2, sd.new_wire_count, new_per_step,
            sd.A_query_delta.size(), sd.L_query_delta.size(),
            ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 5: Verification key gamma_ABC size                                  */
/*                                                                          */
/* gamma_ABC stores the G1 elements used in the verifier's input            */
/* accumulator: vk_x = Σ_{i=0}^{l} x_i · gamma_ABC_i, where l is the      */
/* number of public inputs. In UVC, the public inputs are fixed as          */
/* [s_0, t_1] across all steps, so gamma_ABC.size() = state_size +          */
/* transition_size. This is independent of B.                               */
/* ======================================================================== */

template<typename ppT>
bool test_vk_gamma_abc_size()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_vk_gamma_abc_size ---\n");
    printf("  Checks gamma_ABC size in vk = number of public inputs (ss + ts).\n");
    printf("  Input:    multiplier (ss+ts=2), two-state (ss+ts=3), both B=3\n");
    printf("  Expected: multiplier gamma_ABC=2, two-state gamma_ABC=3\n");

    bool pass = true;

    /* Multiplier: num_inputs = ss + ts = 2 */
    {
        auto st = make_multiplier<FieldT>();
        auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)3);
        bool ok = (kp.vk.gamma_ABC_g1.size() == 2);
        printf("  Multiplier: gamma_ABC size=%zu (exp 2) %s\n",
            kp.vk.gamma_ABC_g1.size(), ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    /* Two-state: num_inputs = ss + ts = 3 */
    {
        auto st = make_two_state_mult<FieldT>();
        auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)3);
        bool ok = (kp.vk.gamma_ABC_g1.size() == 3);
        printf("  Two-state: gamma_ABC size=%zu (exp 3) %s\n",
            kp.vk.gamma_ABC_g1.size(), ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 6: B=1 produces no step_data (only base case)                       */
/*                                                                          */
/* Edge case: with B=1, the UVC scheme degenerates to a standard VC         */
/* (non-updatable). The CRS covers only C_1, and no incremental updates     */
/* are possible. step_data should be empty (no deltas needed).              */
/* ======================================================================== */

template<typename ppT>
bool test_b_equals_1()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_b_equals_1 ---\n");
    printf("  Edge case: B=1 means no incremental steps, only base circuit.\n");
    printf("  Input:    multiplier circuit, B=1\n");
    printf("  Expected: step_data empty (size=0), max_compositions=1\n");

    auto st = make_multiplier<FieldT>();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)1);

    bool pass = (kp.pk.step_data.size() == 0) &&
                (kp.pk.max_compositions == 1) &&
                (kp.vk.max_compositions == 1);

    printf("  step_data size=%zu (exp 0), max=%zu (exp 1) %s\n",
        kp.pk.step_data.size(), kp.pk.max_compositions,
        pass ? "" : "FAIL");

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 7: Constraint system stored in base_pk is C_B                       */
/*                                                                          */
/* The proving key embeds the full B-fold composed constraint system C_B    */
/* (used by the prover for QAP witness computation). This test confirms     */
/* that the stored CS has the expected dimensions:                          */
/*   - num_variables = n + (B-1)*(n-ss) total wires                         */
/*   - num_constraints = B * (constraints per step)                         */
/*   - num_inputs = ss + ts (fixed public inputs: s_0, t_1)                 */
/* ======================================================================== */

template<typename ppT>
bool test_stored_constraint_system()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_stored_constraint_system ---\n");
    printf("  Checks the full composed CS stored in pk matches C_B dimensions.\n");
    printf("  Input:    multiplier circuit, B=3\n");
    printf("  Expected: vars=7 (3+2*2), constraints=3 (3*1), inputs=2 (ss+ts)\n");

    auto st = make_multiplier<FieldT>();
    const size_t B = 3;
    const size_t n = 3, ss = 1;
    const size_t new_per_step = n - ss;
    const size_t expected_vars = n + (B - 1) * new_per_step;
    const size_t expected_constraints = B;

    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

    const auto &stored_cs = kp.pk.base_pk.constraint_system;
    bool pass = (stored_cs.num_variables() == expected_vars) &&
                (stored_cs.num_constraints() == expected_constraints) &&
                (stored_cs.num_inputs() == ss + (size_t)1);

    printf("  Stored CS: vars=%zu (exp %zu), constraints=%zu (exp %zu), inputs=%zu (exp %zu) %s\n",
        stored_cs.num_variables(), expected_vars,
        stored_cs.num_constraints(), expected_constraints,
        stored_cs.num_inputs(), ss + (size_t)1,
        pass ? "" : "FAIL");

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}
/* ======================================================================== */
/* ======================================================================== */
/* Tests 8-9: Single-mode gamma-track CRS layout                            */
/* ======================================================================== */

template<typename ppT>
bool check_generator_dimensions(
    const char *name,
    const state_transition_circuit<libff::Fr<ppT> > &st)
{
    const size_t B = 3;
    const size_t ss = st.state_size;
    const size_t ts = st.transition_size;
    const size_t new_per_step = st.num_new_wires_per_step();
    const auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

    bool state_placeholders = true;
    const std::vector<size_t> state_wires = uvc_state_output_indices(st, B);
    const std::vector<uvc_wire_class> classes = uvc_wire_classes(st, B);
    for (size_t wire = 1; wire <= classes.size() - 1; ++wire) {
        const uvc_wire_class expected = wire <= ss + ts
            ? uvc_wire_class::io
            : (std::find(state_wires.begin(), state_wires.end(), wire) != state_wires.end()
                ? uvc_wire_class::st : uvc_wire_class::wt);
        state_placeholders &= classes[wire] == expected;
    }
    for (const size_t wire : state_wires) {
        const size_t L_index = wire - (ss + ts) - 1;
        state_placeholders &= kp.pk.base_pk.L_query[L_index].is_zero();
    }

    bool step_deltas = kp.pk.step_data.size() == B - 1;
    for (const auto &step_data : kp.pk.step_data) {
        step_deltas &= step_data.st_query_delta.size() == ss;
        step_deltas &= step_data.new_st_count == ss;
        step_deltas &= step_data.new_wt_count == new_per_step - ss;
    }

    const bool pass =
        kp.pk.st_query.size() == B * ss &&
        kp.vk.st_ABC_g1.size() == B * ss &&
        kp.vk.state_size == ss &&
        kp.vk.transition_size == ts &&
        kp.vk.new_per_step == new_per_step &&
        step_deltas && state_placeholders;
    printf("  %s: dimensions, wire classes, and state L placeholders=%s\n",
           name, pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_generator_dimensions()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_generator_dimensions ---\n");
    const bool pass =
        check_generator_dimensions<ppT>("multiplier", make_multiplier<FieldT>()) &&
        check_generator_dimensions<ppT>("two-state multiplier", make_two_state_mult<FieldT>());
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_track_disjointness_checker_detects_violation()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_track_disjointness_checker_detects_violation ---\n");
    const auto st = make_multiplier<FieldT>();
    const size_t B = 3;
    const size_t total_vars = st.wires_per_step() + (B - 1) * st.num_new_wires_per_step();
    const bool consistent = uvc_check_track_disjointness(st, B, total_vars);

    state_transition_circuit<FieldT> malformed;
    malformed.base_cs.primary_input_size = 3;
    malformed.state_size = 2;
    malformed.transition_size = 0;
    bool rejected = false;
    try {
        (void)r1cs_uvc_ppzksnark_generator<ppT>(malformed, 2);
    } catch (const std::logic_error &) {
        rejected = true;
    }

    const bool pass = consistent && rejected;
    printf("  Consistent layout=%s, malformed layout rejected=%s\n",
           consistent ? "PASS" : "FAIL", rejected ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */

template<typename ppT>
bool test_step1_alias_and_span()
{
    typedef libff::Fr<ppT> FieldT;
    const size_t B = 3;
    const state_transition_circuit<FieldT> st = make_multiplier<FieldT>();
    const auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);
    const size_t ss = st.state_size;
    bool pass = kp.pk.step_data.size() == B - 1 &&
                kp.vk.st_ABC_g1.size() == kp.pk.st_query.size();

    for (size_t i = 0; i < kp.pk.st_query.size(); ++i) {
        pass &= kp.vk.st_ABC_g1[i] == kp.pk.st_query[i];
    }
    for (size_t idx = 0; idx < kp.pk.step_data.size(); ++idx) {
        pass &= kp.pk.step_data[idx].st_query_delta.size() == ss;
        for (size_t i = 0; i < ss; ++i) {
            pass &= kp.pk.step_data[idx].st_query_delta[i] ==
                kp.pk.st_query[(idx + 1) * ss + i];
        }
    }

    printf("  published state CRS aliases gamma slices; step 1 has no delta: %s\n",
           pass ? "PASS" : "FAIL");
    return pass;
}
int main()
{
    libff::alt_bn128_pp::init_public_params();
    libff::inhibit_profiling_info = true;

    printf("================================================================\n");
    printf("Unit tests: r1cs_uvc_ppzksnark_generator (UVC.Setup)\n");
    printf("================================================================\n");

    bool all_pass = true;

    all_pass &= test_key_dimensions_multiplier<libff::alt_bn128_pp>();
    all_pass &= test_max_compositions<libff::alt_bn128_pp>();
    all_pass &= test_step_data_dimensions<libff::alt_bn128_pp>();
    all_pass &= test_step_data_two_state<libff::alt_bn128_pp>();
    all_pass &= test_vk_gamma_abc_size<libff::alt_bn128_pp>();
    all_pass &= test_b_equals_1<libff::alt_bn128_pp>();
    all_pass &= test_stored_constraint_system<libff::alt_bn128_pp>();
    all_pass &= test_generator_dimensions<libff::alt_bn128_pp>();
    all_pass &= test_track_disjointness_checker_detects_violation<libff::alt_bn128_pp>();
    all_pass &= test_step1_alias_and_span<libff::alt_bn128_pp>();

    printf("\n================================================================\n");
    printf("r1cs_uvc_ppzksnark_generator: %s\n", all_pass ? "ALL PASSED" : "SOME FAILED");
    printf("================================================================\n");
    return all_pass ? 0 : 1;
}
