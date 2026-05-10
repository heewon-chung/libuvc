/** @file
*****************************************************************************

Unit tests for build_composed_assignment().

This function takes an execution trace (states, transitions, witnesses)
and packs it into the (primary_input, auxiliary_input) pair expected by
the composed constraint system C_j.

=== Assignment layout for C_j ===

The composed assignment maps the execution trace into the wire assignment
vector for C_j. The key rules are:

  primary_input (public, visible to verifier):
    = [s_0, t_1]  — the initial state and first transition.
    This is FIXED across all steps j. The verifier only sees s_0 and t_1,
    regardless of how many compositions have been applied. This is what
    makes verification cost constant (O(|s_0| + |t_1|) pairings).

  auxiliary_input (private witness):
    All remaining wires, in order:
      [s_1, t_2, s_2, t_3, s_3, ..., t_j, s_j]
    plus any internal witness wires at each step.

  Example for multiplier (n=3, ss=1, ts=1) at j=2:
    Wire 0: constant "1" (implicit, not in assignment)
    Wire 1: s_0 = 3       (primary[0])
    Wire 2: t_1 = 5       (primary[1])
    Wire 3: s_1 = 15      (auxiliary[0])  — output of step 1
    Wire 4: t_2 = 7       (auxiliary[1])  — transition for step 2
    Wire 5: s_2 = 105     (auxiliary[2])  — output of step 2

=== What these tests verify ===

  Test 1: primary/auxiliary sizes match expected dimensions for each j
  Test 2: primary content = [s_0, t_1] regardless of step j
  Test 3: primary content for two-state circuit = [a_0, b_0, t_1]
  Test 4: auxiliary wire values at correct positions (single-state)
  Test 5: auxiliary wire values at correct positions (two-state)
  Test 6: assignment satisfies the composed constraint system for j=1..3

=== How to build and run ===

    cd build
    make test_build_composed_assignment
    ./test_build_composed_assignment

=== Expected output ===

    ================================================================
    Unit tests: build_composed_assignment
    ================================================================

    --- test_assignment_sizes_multiplier ---
      j=1: primary=2 (exp 2), aux=1 (exp 1)     ← total 3 wires
      j=2: primary=2 (exp 2), aux=3 (exp 3)     ← total 5 wires
      j=3: primary=2 (exp 2), aux=5 (exp 5)     ← total 7 wires
      Result: PASS
      ← primary always 2 (s_0, t_1); aux grows by 2 per step

    --- test_primary_content ---
      j=1: primary=[7, 5] (exp [7, 5])
      j=2: primary=[7, 5] (exp [7, 5])    ← same regardless of step
      Result: PASS

    --- test_primary_content_two_state ---
      j=1: primary size=3, values correct: YES    ← [a_0=2, b_0=3, t_1=5]
      j=2: primary size=3, values correct: YES
      Result: PASS

    --- test_auxiliary_wire_values ---
      aux[0]=s_1=15 (exp 15)      ← wire 3: output of step 1
      aux[1]=t_2=7 (exp 7)        ← wire 4: transition input for step 2
      aux[2]=s_2=105 (exp 105)    ← wire 5: output of step 2
      Result: PASS

    --- test_auxiliary_wire_values_two_state ---
      aux size=5 (exp 5)
      a_1=10, b_1=15, t_2=3, a_2=30, b_2=45
      Result: PASS
      ← Wires 4-8: [a_1, b_1, t_2, a_2, b_2]

    --- test_assignment_satisfies_cs ---
      C_1 satisfied: YES    C_2 satisfied: YES    C_3 satisfied: YES
      Result: PASS
      ← Cross-check: assignment from build_composed_assignment satisfies
        constraint system from build_composed_constraint_system

    ================================================================
    build_composed_assignment: ALL PASSED  ← exit code 0
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


/* ======================================================================== */
/* Test 1: Primary/auxiliary size for multiplier                             */
/* ======================================================================== */

template<typename ppT>
bool test_assignment_sizes_multiplier()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_assignment_sizes_multiplier ---\n");
    printf("  Checks primary/auxiliary sizes match dimension formulas for each j.\n");
    printf("  Input:    multiplier trace (n=3, ss=1, ts=1), j=1..3\n");
    printf("  Expected: primary=2 (fixed), aux = 2j-1\n");

    auto st = make_multiplier<FieldT>();
    const size_t ss = 1, ts = 1, n = 3;
    const size_t new_per_step = n - ss;

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
        auto assign = build_composed_assignment(st, j, states, transitions, witnesses);
        size_t expected_primary = ss + ts;
        size_t total_vars = n + (j - 1) * new_per_step;
        size_t expected_aux = total_vars - expected_primary;

        bool ok = (assign.first.size() == expected_primary) &&
                  (assign.second.size() == expected_aux);
        printf("  j=%zu: primary=%zu (exp %zu), aux=%zu (exp %zu) %s\n",
            j, assign.first.size(), expected_primary,
            assign.second.size(), expected_aux, ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 2: Primary input content = [s_0, t_1] for all steps                */
/* ======================================================================== */

template<typename ppT>
bool test_primary_content()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_primary_content ---\n");
    printf("  Verifies primary input = [s_0, t_1] regardless of step j.\n");
    printf("  Input:    s_0=7, t_1=5 (multiplier), j=1..2\n");
    printf("  Expected: primary=[7, 5] at every step\n");

    auto st = make_multiplier<FieldT>();

    std::vector<std::vector<FieldT> > states = {
        {FieldT(7)}, {FieldT(35)}, {FieldT(70)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {} };

    bool pass = true;

    for (size_t j = 1; j <= 2; ++j)
    {
        auto assign = build_composed_assignment(st, j, states, transitions, witnesses);
        /* primary = [s_0, t_1] = [7, 5] regardless of step */
        bool ok = (assign.first[0] == FieldT(7)) && (assign.first[1] == FieldT(5));
        printf("  j=%zu: primary=[%s, %s] (exp [7, 5]) %s\n",
            j,
            (assign.first[0] == FieldT(7)) ? "7" : "?",
            (assign.first[1] == FieldT(5)) ? "5" : "?",
            ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 3: Primary content for two-state = [a_0, b_0, t_1]                 */
/* ======================================================================== */

template<typename ppT>
bool test_primary_content_two_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_primary_content_two_state ---\n");
    printf("  Verifies primary = [a_0, b_0, t_1] for two-state circuit.\n");
    printf("  Input:    (a_0,b_0)=(2,3), t_1=5, j=1..2\n");
    printf("  Expected: primary=[2, 3, 5] at every step\n");

    auto st = make_two_state_mult<FieldT>();

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)},
        {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(45)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(3)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {} };

    bool pass = true;

    for (size_t j = 1; j <= 2; ++j)
    {
        auto assign = build_composed_assignment(st, j, states, transitions, witnesses);
        /* primary = [a_0, b_0, t_1] = [2, 3, 5] */
        bool ok = (assign.first.size() == 3) &&
                  (assign.first[0] == FieldT(2)) &&
                  (assign.first[1] == FieldT(3)) &&
                  (assign.first[2] == FieldT(5));
        printf("  j=%zu: primary size=%zu, values correct: %s\n",
            j, assign.first.size(), ok ? "YES" : "NO");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 4: Auxiliary wire values at expected positions                       */
/* ======================================================================== */

template<typename ppT>
bool test_auxiliary_wire_values()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_auxiliary_wire_values ---\n");
    printf("  Checks auxiliary wire values at correct positions for j=2.\n");
    printf("  Input:    s_0=3, t=[5,7], s=[15,105]\n");
    printf("  Expected: aux=[s_1=15, t_2=7, s_2=105]\n");

    auto st = make_multiplier<FieldT>();

    /* s_0=3, t_1=5, s_1=15, t_2=7, s_2=105 */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(3)}, {FieldT(15)}, {FieldT(105)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(7)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {} };

    /* For j=2, the full assignment (excluding constant wire 0) is:
       Wire 1: s_0=3       (primary[0])
       Wire 2: t_1=5       (primary[1])
       Wire 3: s_1=15      (auxiliary[0])
       Wire 4: t_2=7       (auxiliary[1])
       Wire 5: s_2=105     (auxiliary[2])
    */
    auto assign = build_composed_assignment(st, (size_t)2, states, transitions, witnesses);

    bool pass = true;

    /* Check auxiliary values */
    pass &= (assign.second[0] == FieldT(15));   /* s_1 */
    pass &= (assign.second[1] == FieldT(7));    /* t_2 */
    pass &= (assign.second[2] == FieldT(105));  /* s_2 */

    printf("  aux[0]=s_1=%s (exp 15)\n", assign.second[0] == FieldT(15) ? "15" : "WRONG");
    printf("  aux[1]=t_2=%s (exp 7)\n", assign.second[1] == FieldT(7) ? "7" : "WRONG");
    printf("  aux[2]=s_2=%s (exp 105)\n", assign.second[2] == FieldT(105) ? "105" : "WRONG");

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 5: Auxiliary wire values for two-state                              */
/* ======================================================================== */

template<typename ppT>
bool test_auxiliary_wire_values_two_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_auxiliary_wire_values_two_state ---\n");
    printf("  Checks auxiliary wire values for two-state circuit at j=2.\n");
    printf("  Input:    (a_0,b_0)=(2,3), t=[5,3]\n");
    printf("  Expected: aux=[a_1=10, b_1=15, t_2=3, a_2=30, b_2=45]\n");

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

    /* For j=2, full assignment:
       Wire 1: a_0=2       (primary[0])
       Wire 2: b_0=3       (primary[1])
       Wire 3: t_1=5       (primary[2])
       Wire 4: a_1=10      (auxiliary[0])
       Wire 5: b_1=15      (auxiliary[1])
       Wire 6: t_2=3       (auxiliary[2])
       Wire 7: a_2=30      (auxiliary[3])
       Wire 8: b_2=45      (auxiliary[4])
    */
    auto assign = build_composed_assignment(st, (size_t)2, states, transitions, witnesses);

    bool pass = true;

    pass &= (assign.second.size() == 5);
    pass &= (assign.second[0] == FieldT(10));  /* a_1 */
    pass &= (assign.second[1] == FieldT(15));  /* b_1 */
    pass &= (assign.second[2] == FieldT(3));   /* t_2 */
    pass &= (assign.second[3] == FieldT(30));  /* a_2 */
    pass &= (assign.second[4] == FieldT(45));  /* b_2 */

    printf("  aux size=%zu (exp 5)\n", assign.second.size());
    printf("  a_1=%s, b_1=%s, t_2=%s, a_2=%s, b_2=%s\n",
        assign.second[0] == FieldT(10) ? "10" : "?",
        assign.second[1] == FieldT(15) ? "15" : "?",
        assign.second[2] == FieldT(3)  ? "3"  : "?",
        assign.second[3] == FieldT(30) ? "30" : "?",
        assign.second[4] == FieldT(45) ? "45" : "?");

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Test 6: Assignment satisfies composed CS for multiple steps              */
/* ======================================================================== */

template<typename ppT>
bool test_assignment_satisfies_cs()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_assignment_satisfies_cs ---\n");
    printf("  Cross-check: assignment satisfies composed CS for j=1..3.\n");
    printf("  Input:    two-state trace (a_0,b_0)=(2,3), t=[5,3,2]\n");
    printf("  Expected: C_1, C_2, C_3 all satisfied (YES)\n");

    auto st = make_two_state_mult<FieldT>();

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)},
        {FieldT(10), FieldT(15)},
        {FieldT(30), FieldT(45)},
        {FieldT(60), FieldT(90)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5)}, {FieldT(3)}, {FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {}, {} };

    bool pass = true;

    for (size_t j = 1; j <= 3; ++j)
    {
        auto cs_j = build_composed_constraint_system(st, j);
        auto assign = build_composed_assignment(st, j, states, transitions, witnesses);
        bool sat = cs_j.is_satisfied(assign.first, assign.second);
        printf("  C_%zu satisfied: %s\n", j, sat ? "YES" : "NO");
        if (!sat) pass = false;
    }

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
    printf("Unit tests: build_composed_assignment\n");
    printf("================================================================\n");

    bool all_pass = true;

    all_pass &= test_assignment_sizes_multiplier<libff::alt_bn128_pp>();
    all_pass &= test_primary_content<libff::alt_bn128_pp>();
    all_pass &= test_primary_content_two_state<libff::alt_bn128_pp>();
    all_pass &= test_auxiliary_wire_values<libff::alt_bn128_pp>();
    all_pass &= test_auxiliary_wire_values_two_state<libff::alt_bn128_pp>();
    all_pass &= test_assignment_satisfies_cs<libff::alt_bn128_pp>();

    printf("\n================================================================\n");
    printf("build_composed_assignment: %s\n", all_pass ? "ALL PASSED" : "SOME FAILED");
    printf("================================================================\n");
    return all_pass ? 0 : 1;
}
