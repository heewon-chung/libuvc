/** @file
*****************************************************************************

Unit tests for build_composed_constraint_system().

This function builds the j-step composed circuit C_j from a base state
transition circuit C. C_j represents j sequential applications of the
state update function: s_0 → s_1 → ... → s_j.

=== How circuit composition works ===

Given a base circuit C with n wires (state_size=ss, transition_size=ts):

  Step 1 (C_1): Uses the original n wires.
    Wire layout: [s_0 (ss wires), t_1 (ts wires), s_1 + witness]
    primary_input = s_0 ∪ t_1 (ss + ts wires)
    auxiliary_input = remaining n - (ss+ts) wires

  Step j>1 (C_j): Adds (n - ss) new wires per step.
    The key insight is wire sharing: the state OUTPUT wires of step j-1
    (s_{j-1}) become the state INPUT wires of step j. Only the transition
    input t_j, state output s_j, and any internal witness are new.

    C_j total wires = n + (j-1) * (n - ss)
    C_j total constraints = j * (constraints per step)
    C_j primary_input = s_0 ∪ t_1 (FIXED across all j — this is why
      verification cost is constant regardless of step count)

=== What these tests verify ===

  Test 1: Dimension formulas for single-state multiplier (n=3, ss=1)
  Test 2: Dimension formulas for two-state circuit (n=5, ss=2)
  Test 3: Correct assignments satisfy the composed constraint system
  Test 4: Incorrect assignments (wrong s_1) are rejected
  Test 5: Wire sharing — state continuity between steps is enforced
  Test 6: Wire sharing for two-state circuits (both a and b must match)

=== How to build and run ===

    cd build
    make test_build_composed_cs
    ./test_build_composed_cs

=== Expected output ===

    ================================================================
    Unit tests: build_composed_constraint_system
    ================================================================

    --- test_dimensions_multiplier ---
      C_1: vars=3 (exp 3), constraints=1 (exp 1), primary=2 (exp 2), auxiliary=1 (exp 1)
      C_2: vars=5 (exp 5), constraints=2 (exp 2), primary=2 (exp 2), auxiliary=3 (exp 3)
      C_3: vars=7 (exp 7), constraints=3 (exp 3), primary=2 (exp 2), auxiliary=5 (exp 5)
      C_4: vars=9 (exp 9), constraints=4 (exp 4), primary=2 (exp 2), auxiliary=7 (exp 7)
      C_5: vars=11 (exp 11), constraints=5 (exp 5), primary=2 (exp 2), auxiliary=9 (exp 9)
      Result: PASS
      ← vars = 3 + (j-1)*2; constraints = j; primary = 2 (fixed); aux = vars - 2

    --- test_dimensions_two_state ---
      C_1: vars=5 (exp 5), constraints=2 (exp 2)
      C_2: vars=8 (exp 8), constraints=4 (exp 4)
      C_3: vars=11 (exp 11), constraints=6 (exp 6)
      C_4: vars=14 (exp 14), constraints=8 (exp 8)
      Result: PASS
      ← vars = 5 + (j-1)*3; constraints = 2*j

    --- test_cs_satisfaction ---
      C_1 satisfied: YES    C_2 satisfied: YES    C_3 satisfied: YES
      Result: PASS
      ← Correct trace: s_0=3, t_1=5→15, t_2=7→105, t_3=2→210

    --- test_cs_non_satisfaction ---
      C_1 with wrong s_1: satisfied=NO (expected NO)
      Result: PASS
      ← Wrong value: s_1=99 instead of 15; constraint 3*5=99 fails

    --- test_wire_sharing ---
      C_3 with state continuity: satisfied
      C_3 with broken state at step 2: NOT satisfied (expected NO)
      Result: PASS
      ← s_2=99 instead of 18; wire shared between steps detects mismatch

    --- test_wire_sharing_two_state ---
      C_2 (two-state) with state continuity: satisfied
      C_2 (two-state) with broken b_2: NOT satisfied (expected NO)
      Result: PASS
      ← b_2=999 instead of 45; second state component mismatch detected

    ================================================================
    build_composed_constraint_system: ALL PASSED  ← exit code 0
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

/* ======================================================================== */
/* Circuit factories                                                        */
/* ======================================================================== */

/** s_out = s_in * t — 1 constraint, 3 variables, state=1, transition=1 */
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

/** (a_out, b_out) = (a_in * t, b_in * t) — 2 constraints, 5 variables, state=2, transition=1 */
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
/* Test 1: Dimension checks for single-state multiplier                     */
/* ======================================================================== */

template<typename ppT>
bool test_dimensions_multiplier()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_dimensions_multiplier ---\n");
    printf("  Checks vars, constraints, primary/auxiliary sizes for C_j (j=1..5).\n");
    printf("  Input:    multiplier circuit (n=3, ss=1, ts=1)\n");
    printf("  Expected: vars = 2j+1, constraints = j, primary = 2, aux = 2j-1\n");

    auto st = make_multiplier<FieldT>();
    const size_t n = 3;  /* base: 3 variables */
    const size_t ss = 1, ts = 1;
    const size_t new_per_step = n - ss; /* = 2 */

    bool pass = true;

    for (size_t j = 1; j <= 5; ++j)
    {
        auto cs_j = build_composed_constraint_system(st, j);

        size_t expected_vars = n + (j - 1) * new_per_step;
        size_t expected_constraints = j * 1;
        size_t expected_primary = ss + ts;
        size_t expected_auxiliary = expected_vars - expected_primary;

        printf("  C_%zu: vars=%zu (exp %zu), constraints=%zu (exp %zu), "
               "primary=%zu (exp %zu), auxiliary=%zu (exp %zu)\n",
            j, cs_j.num_variables(), expected_vars,
            cs_j.num_constraints(), expected_constraints,
            cs_j.num_inputs(), expected_primary,
            cs_j.auxiliary_input_size, expected_auxiliary);

        bool ok = (cs_j.num_variables() == expected_vars) &&
                  (cs_j.num_constraints() == expected_constraints) &&
                  (cs_j.num_inputs() == expected_primary) &&
                  (cs_j.auxiliary_input_size == expected_auxiliary);
        if (!ok) {
            printf("  FAIL at j=%zu\n", j);
            pass = false;
        }
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 2: Dimension checks for two-state circuit                           */
/* ======================================================================== */

template<typename ppT>
bool test_dimensions_two_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_dimensions_two_state ---\n");
    printf("  Same dimension checks for two-state circuit (j=1..4).\n");
    printf("  Input:    two-state circuit (n=5, ss=2, ts=1)\n");
    printf("  Expected: vars = 3j+2, constraints = 2j\n");

    auto st = make_two_state_mult<FieldT>();
    const size_t n = 5;
    const size_t ss = 2, ts = 1;
    const size_t new_per_step = n - ss; /* = 3 */

    bool pass = true;

    for (size_t j = 1; j <= 4; ++j)
    {
        auto cs_j = build_composed_constraint_system(st, j);

        size_t expected_vars = n + (j - 1) * new_per_step;
        size_t expected_constraints = j * 2;
        size_t expected_primary = ss + ts;
        size_t expected_auxiliary = expected_vars - expected_primary;

        printf("  C_%zu: vars=%zu (exp %zu), constraints=%zu (exp %zu)\n",
            j, cs_j.num_variables(), expected_vars,
            cs_j.num_constraints(), expected_constraints);

        bool ok = (cs_j.num_variables() == expected_vars) &&
                  (cs_j.num_constraints() == expected_constraints) &&
                  (cs_j.num_inputs() == expected_primary) &&
                  (cs_j.auxiliary_input_size == expected_auxiliary);
        if (!ok) {
            printf("  FAIL at j=%zu\n", j);
            pass = false;
        }
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 3: Constraint satisfaction with correct assignment                   */
/* ======================================================================== */

template<typename ppT>
bool test_cs_satisfaction()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_cs_satisfaction ---\n");
    printf("  Verifies correct execution traces satisfy the composed CS.\n");
    printf("  Input:    multiplier trace s_0=3, t=[5,7,2] -> s=[15,105,210]\n");
    printf("  Expected: C_1, C_2, C_3 all satisfied (YES)\n");

    auto st = make_multiplier<FieldT>();

    /* s_0=3, t_1=5->s_1=15, t_2=7->s_2=105, t_3=2->s_3=210 */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(3)}, {FieldT(15)}, {FieldT(105)}, {FieldT(210)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(7)}, {FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {}, {} };

    bool pass = true;

    for (size_t j = 1; j <= 3; ++j)
    {
        auto cs_j = build_composed_constraint_system(st, j);
        auto assignment = build_composed_assignment(st, j, states, transitions, witnesses);

        bool sat = cs_j.is_satisfied(assignment.first, assignment.second);
        printf("  C_%zu satisfied: %s\n", j, sat ? "YES" : "NO");
        if (!sat) { pass = false; }
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 4: Constraint NON-satisfaction with wrong assignment                 */
/* ======================================================================== */

template<typename ppT>
bool test_cs_non_satisfaction()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_cs_non_satisfaction ---\n");
    printf("  Verifies wrong assignment is rejected by the composed CS.\n");
    printf("  Input:    s_0=3, t_1=5, s_1=99 (wrong; correct would be 15)\n");
    printf("  Expected: C_1 NOT satisfied (3*5 != 99)\n");

    auto st = make_multiplier<FieldT>();

    /* Correct: s_0=3, t_1=5, s_1=15 */
    /* Wrong:   s_1=99 instead of 15 */
    std::vector<std::vector<FieldT> > states_bad = {
        {FieldT(3)}, {FieldT(99)}
    };
    std::vector<std::vector<FieldT> > transitions = { {FieldT(5)} };
    std::vector<std::vector<FieldT> > witnesses = { {} };

    auto cs_1 = build_composed_constraint_system(st, (size_t)1);
    auto assignment = build_composed_assignment(st, (size_t)1, states_bad, transitions, witnesses);

    bool sat = cs_1.is_satisfied(assignment.first, assignment.second);
    printf("  C_1 with wrong s_1: satisfied=%s (expected NO)\n", sat ? "YES" : "NO");

    bool pass = !sat;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 5: Wire sharing — state output of step k-1 = state input of step k */
/*                                                                          */
/* This is the critical structural property of UVC circuit composition.     */
/* When C_j is built from j copies of the base circuit, the state OUTPUT   */
/* wires of step k-1 are literally the SAME wire indices as the state      */
/* INPUT wires of step k. This means:                                       */
/*                                                                          */
/*   - If step 1 outputs s_1 at wire index 3, then step 2's constraint    */
/*     references wire 3 as its state input.                                */
/*   - The assignment for wire 3 must be consistent: the value computed    */
/*     as output at step 1 IS the input to step 2 (no separate copy).     */
/*                                                                          */
/* This test verifies wire sharing by:                                      */
/*   (a) Confirming that a correct, state-continuous trace satisfies C_3    */
/*   (b) Breaking state continuity at step 2 (s_2=99 instead of 18) and   */
/*       confirming the constraint system rejects it                        */
/* ======================================================================== */

template<typename ppT>
bool test_wire_sharing()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_wire_sharing ---\n");
    printf("  Tests that state output wires of step k are reused as input of step k+1.\n");
    printf("  Input:    s_0=2, t=[3,3,2] -> s=[6,18,36] (correct), then s_2=99 (broken)\n");
    printf("  Expected: correct trace satisfies C_3; broken s_2 is rejected\n");

    auto st = make_multiplier<FieldT>();
    const size_t ss = 1, ts = 1, n = 3;
    const size_t new_per_step = n - ss; /* = 2 */

    /* Build C_3 and check that wire indices for state output at step k
       match wire indices for state input at step k+1.
       Wire layout:
         Step 1: [1]=s_0, [2]=t_1, [3]=s_1
         Step 2: s_1 reused from [3], [4]=t_2, [5]=s_2
         Step 3: s_2 reused from [5], [6]=t_3, [7]=s_3

       State output at step 1 is at wire ss+ts+1 = 3
       State input at step 2 reuses wire 3 (the s_1 output)
       State output at step 2 is at wire (2-1)*new_per_step + ss + ts + 1 = 2+1+1+1 = 5
       State input at step 3 reuses wire 5 */

    /* We verify this by checking that assignments with state continuity satisfy C_j. */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)}, {FieldT(6)}, {FieldT(18)}, {FieldT(36)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(3)}, {FieldT(3)}, {FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {}, {} };

    auto cs_3 = build_composed_constraint_system(st, (size_t)3);
    auto assign_3 = build_composed_assignment(st, (size_t)3, states, transitions, witnesses);

    bool sat = cs_3.is_satisfied(assign_3.first, assign_3.second);
    printf("  C_3 with state continuity: %s\n", sat ? "satisfied" : "NOT satisfied");

    /* Now break state continuity: s_1 output should be 6, but at step 2
       we use s_1 input = 6 (from sharing), and t_2 = 3, so s_2 = 18.
       If we set s_2 = 99, it should fail. */
    std::vector<std::vector<FieldT> > states_broken = {
        {FieldT(2)}, {FieldT(6)}, {FieldT(99)}, {FieldT(36)}
    };
    auto assign_broken = build_composed_assignment(st, (size_t)3, states_broken, transitions, witnesses);
    bool sat_broken = cs_3.is_satisfied(assign_broken.first, assign_broken.second);
    printf("  C_3 with broken state at step 2: %s (expected NO)\n",
        sat_broken ? "satisfied" : "NOT satisfied");

    bool pass = sat && !sat_broken;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 6: Two-state wire sharing                                           */
/* ======================================================================== */

template<typename ppT>
bool test_wire_sharing_two_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_wire_sharing_two_state ---\n");
    printf("  Wire sharing for two-state circuit (both a and b must be continuous).\n");
    printf("  Input:    (a_0,b_0)=(2,3), t=[5,3] -> (10,15),(30,45); then b_2=999\n");
    printf("  Expected: correct trace satisfies C_2; broken b_2 is rejected\n");

    auto st = make_two_state_mult<FieldT>();

    /* (a_0,b_0)=(2,3), t_1=5 -> (10,15), t_2=3 -> (30,45) */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)},
        {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(45)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(3)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {} };

    auto cs_2 = build_composed_constraint_system(st, (size_t)2);
    auto assign_2 = build_composed_assignment(st, (size_t)2, states, transitions, witnesses);

    bool sat = cs_2.is_satisfied(assign_2.first, assign_2.second);
    printf("  C_2 (two-state) with state continuity: %s\n", sat ? "satisfied" : "NOT satisfied");

    /* Break one state element */
    std::vector<std::vector<FieldT> > states_bad = {
        {FieldT(2), FieldT(3)},
        {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(999)}  /* b_2 should be 45 */
    };
    auto assign_bad = build_composed_assignment(st, (size_t)2, states_bad, transitions, witnesses);
    bool sat_bad = cs_2.is_satisfied(assign_bad.first, assign_bad.second);
    printf("  C_2 (two-state) with broken b_2: %s (expected NO)\n",
        sat_bad ? "satisfied" : "NOT satisfied");

    bool pass = sat && !sat_bad;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */

int main()
{
    libff::alt_bn128_pp::init_public_params();
    libff::inhibit_profiling_info = true;

    printf("================================================================\n");
    printf("Unit tests: build_composed_constraint_system\n");
    printf("================================================================\n");

    bool all_pass = true;

    all_pass &= test_dimensions_multiplier<libff::alt_bn128_pp>();
    all_pass &= test_dimensions_two_state<libff::alt_bn128_pp>();
    all_pass &= test_cs_satisfaction<libff::alt_bn128_pp>();
    all_pass &= test_cs_non_satisfaction<libff::alt_bn128_pp>();
    all_pass &= test_wire_sharing<libff::alt_bn128_pp>();
    all_pass &= test_wire_sharing_two_state<libff::alt_bn128_pp>();

    printf("\n================================================================\n");
    printf("build_composed_constraint_system: %s\n", all_pass ? "ALL PASSED" : "SOME FAILED");
    printf("================================================================\n");
    return all_pass ? 0 : 1;
}
