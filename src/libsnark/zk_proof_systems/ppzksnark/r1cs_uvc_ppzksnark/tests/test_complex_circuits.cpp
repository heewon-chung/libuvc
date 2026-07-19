/** @file
*****************************************************************************

Unit tests for complex state transition circuits in the UVC scheme.

Existing tests use only two trivially simple circuits (multiplier: 1 constraint,
3 wires, no witness; two-state mult: 2 constraints, 5 wires, no witness).
This file adds 4 circuits exercising features not previously covered:

  - Internal witness wires (intermediate computation values)
  - Transition size > 1 (multiple external inputs per step)
  - State size > 2 (3-element state vectors)
  - 3+ constraints per step
  - Many composition steps (up to 10)

=== Circuits ===

  1. poly_eval     (ss=1, ts=2, n=5, 2 constraints, 1 witness)
  2. three_state   (ss=3, ts=1, n=7, 3 constraints, 0 witnesses)
  3. chained_cube  (ss=1, ts=1, n=5, 3 constraints, 2 witnesses)
  4. cross_multiply(ss=2, ts=2, n=8, 4 constraints, 2 witnesses)

=== Test categories (22 tests) ===

  A. Dimension tests (4):         vars, constraints, primary sizes for C_j
  B. Constraint satisfaction (4): correct traces satisfy C_1..C_3
  C. Witness sensitivity (3):     wrong witness -> CS not satisfied
  D. End-to-end UVC (8):          Setup -> Prove -> Verify + soundness
  E. Many-step stress (1):        10-step poly_eval chain
  F. Soundness (2):               corrupted proofs and cross-trace rejection

=== How to build and run ===

    cd build
    make test_complex_circuits
    ./test_complex_circuits

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
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/sensor_fusion_circuit.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/hadamard_circuit.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/mimc_circuit.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/scalable_circuit.hpp>

using namespace libsnark;

/* ======================================================================== */
/* Circuit factories                                                        */
/* ======================================================================== */

/**
 * poly_eval: s_out = s_in * t1 * t2 via intermediate w = s_in * t1
 *
 * n=5, ss=1, ts=2, 2 constraints, 1 internal witness wire
 *
 * Wire layout (convention: [s_in, t, s_out, witnesses]):
 *   [1]=s_in, [2]=t1, [3]=t2, [4]=s_out, [5]=w
 *
 * Constraints:
 *   s_in * t1 = w        (wire 1 * wire 2 = wire 5)
 *   w * t2 = s_out       (wire 5 * wire 3 = wire 4)
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_poly_eval()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 3;   /* s_in, t1, t2 */
    cs.auxiliary_input_size = 2; /* s_out, w */

    {
        linear_combination<FieldT> A, B, C;
        A.add_term(1, FieldT::one());
        B.add_term(2, FieldT::one());
        C.add_term(5, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(5, FieldT::one());
        B.add_term(3, FieldT::one());
        C.add_term(4, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    return state_transition_circuit<FieldT>(cs, 1, 2);
}

/**
 * three_state_accum: (a,b,c)_out = (a,b,c)_in * t
 *
 * n=7, ss=3, ts=1, 3 constraints, 0 internal witness wires
 *
 * Wire layout:
 *   [1]=a_in, [2]=b_in, [3]=c_in, [4]=t, [5]=a_out, [6]=b_out, [7]=c_out
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_three_state_accum()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 4;   /* a_in, b_in, c_in, t */
    cs.auxiliary_input_size = 3; /* a_out, b_out, c_out */

    {
        linear_combination<FieldT> A, B, C;
        A.add_term(1, FieldT::one());
        B.add_term(4, FieldT::one());
        C.add_term(5, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(2, FieldT::one());
        B.add_term(4, FieldT::one());
        C.add_term(6, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(3, FieldT::one());
        B.add_term(4, FieldT::one());
        C.add_term(7, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    return state_transition_circuit<FieldT>(cs, 3, 1);
}

/**
 * chained_cube: s_out = s_in * t^3 via w1 = s_in*t, w2 = w1*t, s_out = w2*t
 *
 * n=5, ss=1, ts=1, 3 constraints, 2 internal witness wires
 *
 * Wire layout:
 *   [1]=s_in, [2]=t, [3]=s_out, [4]=w1, [5]=w2
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_chained_cube()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 2;   /* s_in, t */
    cs.auxiliary_input_size = 3; /* s_out, w1, w2 */

    {
        linear_combination<FieldT> A, B, C;
        A.add_term(1, FieldT::one());
        B.add_term(2, FieldT::one());
        C.add_term(4, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(4, FieldT::one());
        B.add_term(2, FieldT::one());
        C.add_term(5, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(5, FieldT::one());
        B.add_term(2, FieldT::one());
        C.add_term(3, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    return state_transition_circuit<FieldT>(cs, 1, 1);
}

/**
 * cross_multiply: cross-coupled two-state multiplication
 *   w1 = a_in * t1, a_out = w1 * t2
 *   w2 = b_in * t2, b_out = w2 * t1
 *
 * n=8, ss=2, ts=2, 4 constraints, 2 internal witness wires
 *
 * Wire layout:
 *   [1]=a_in, [2]=b_in, [3]=t1, [4]=t2,
 *   [5]=a_out, [6]=b_out, [7]=w1, [8]=w2
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_cross_multiply()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 4;   /* a_in, b_in, t1, t2 */
    cs.auxiliary_input_size = 4; /* a_out, b_out, w1, w2 */

    {
        linear_combination<FieldT> A, B, C;
        A.add_term(1, FieldT::one());
        B.add_term(3, FieldT::one());
        C.add_term(7, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(7, FieldT::one());
        B.add_term(4, FieldT::one());
        C.add_term(5, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(2, FieldT::one());
        B.add_term(4, FieldT::one());
        C.add_term(8, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(8, FieldT::one());
        B.add_term(3, FieldT::one());
        C.add_term(6, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    return state_transition_circuit<FieldT>(cs, 2, 2);
}


/* ======================================================================== */
/* E2E helper                                                               */
/* ======================================================================== */

/**
 * Generic end-to-end UVC test: Setup -> Prove (base + incremental) -> Verify.
 * Includes soundness check (corrupt primary[0] -> rejected).
 */
template<typename ppT>
bool run_e2e_uvc(
    const state_transition_circuit<libff::Fr<ppT> > &st,
    size_t B,
    size_t num_steps,
    const std::vector<std::vector<libff::Fr<ppT> > > &states,
    const std::vector<std::vector<libff::Fr<ppT> > > &transitions,
    const std::vector<std::vector<libff::Fr<ppT> > > &witnesses,
    const char *name)
{
    typedef libff::Fr<ppT> FieldT;
    printf("  UVC chain: %s\n", name);
    const auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);
    libff::G1<ppT> D_prev = libff::G1<ppT>::zero();
    libff::G1<ppT> final_D_prev = D_prev;
    r1cs_uvc_ppzksnark_proof<ppT> prev;
    bool pass = true;
    for (size_t step = 1; step <= num_steps; ++step) {
        const auto assignment = build_composed_assignment(st, step, states, transitions, witnesses);
        const auto cs_j = build_composed_constraint_system(st, step);
        if (!cs_j.is_satisfied(assignment.first, assignment.second)) return false;
        const auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, step, assignment.first, assignment.second, step == 1 ? nullptr : &prev);
        const bool accepted = r1cs_uvc_ppzksnark_verifier<ppT>(
            kp.vk, step, assignment.first, states[step], proof, D_prev);
        pass &= accepted;
        if (step == num_steps) final_D_prev = D_prev;
        if (accepted) D_prev = proof.g_D;
        prev = proof;
    }
    if (pass && num_steps >= 1) {
        const auto assignment = build_composed_assignment(st, num_steps, states, transitions, witnesses);
        std::vector<FieldT> bad_primary = assignment.first;
        bad_primary[0] += FieldT::one();
        pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(
            kp.vk, num_steps, bad_primary, states[num_steps], prev, final_D_prev);
        std::vector<FieldT> bad_state = states[num_steps];
        bad_state[0] += FieldT::one();
        pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(
            kp.vk, num_steps, assignment.first, bad_state, prev, final_D_prev);
    }
    return pass;
}

/* ======================================================================== */
/* Category A: Dimension tests (4)                                          */
/* ======================================================================== */

template<typename ppT>
bool test_dimensions_poly_eval()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_dimensions_poly_eval ---\n");
    printf("  poly_eval: n=5, ss=1, ts=2, 2 constraints/step\n");

    auto st = make_poly_eval<FieldT>();
    const size_t n = 5, ss = 1, ts = 2, cps = 2;
    const size_t new_per_step = n - ss;
    bool pass = true;

    for (size_t j = 1; j <= 5; ++j)
    {
        auto cs_j = build_composed_constraint_system(st, j);
        size_t exp_vars = n + (j - 1) * new_per_step;
        size_t exp_cons = j * cps;
        size_t exp_primary = ss + ts;

        bool ok = (cs_j.num_variables() == exp_vars) &&
                  (cs_j.num_constraints() == exp_cons) &&
                  (cs_j.num_inputs() == exp_primary);
        printf("  C_%zu: vars=%zu (exp %zu), constraints=%zu (exp %zu), primary=%zu (exp %zu) %s\n",
            j, cs_j.num_variables(), exp_vars,
            cs_j.num_constraints(), exp_cons,
            cs_j.num_inputs(), exp_primary, ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_dimensions_three_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_dimensions_three_state ---\n");
    printf("  three_state: n=7, ss=3, ts=1, 3 constraints/step\n");

    auto st = make_three_state_accum<FieldT>();
    const size_t n = 7, ss = 3, ts = 1, cps = 3;
    const size_t new_per_step = n - ss;
    bool pass = true;

    for (size_t j = 1; j <= 4; ++j)
    {
        auto cs_j = build_composed_constraint_system(st, j);
        size_t exp_vars = n + (j - 1) * new_per_step;
        size_t exp_cons = j * cps;
        size_t exp_primary = ss + ts;

        bool ok = (cs_j.num_variables() == exp_vars) &&
                  (cs_j.num_constraints() == exp_cons) &&
                  (cs_j.num_inputs() == exp_primary);
        printf("  C_%zu: vars=%zu (exp %zu), constraints=%zu (exp %zu), primary=%zu (exp %zu) %s\n",
            j, cs_j.num_variables(), exp_vars,
            cs_j.num_constraints(), exp_cons,
            cs_j.num_inputs(), exp_primary, ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_dimensions_chained_cube()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_dimensions_chained_cube ---\n");
    printf("  chained_cube: n=5, ss=1, ts=1, 3 constraints/step\n");

    auto st = make_chained_cube<FieldT>();
    const size_t n = 5, ss = 1, ts = 1, cps = 3;
    const size_t new_per_step = n - ss;
    bool pass = true;

    for (size_t j = 1; j <= 5; ++j)
    {
        auto cs_j = build_composed_constraint_system(st, j);
        size_t exp_vars = n + (j - 1) * new_per_step;
        size_t exp_cons = j * cps;
        size_t exp_primary = ss + ts;

        bool ok = (cs_j.num_variables() == exp_vars) &&
                  (cs_j.num_constraints() == exp_cons) &&
                  (cs_j.num_inputs() == exp_primary);
        printf("  C_%zu: vars=%zu (exp %zu), constraints=%zu (exp %zu), primary=%zu (exp %zu) %s\n",
            j, cs_j.num_variables(), exp_vars,
            cs_j.num_constraints(), exp_cons,
            cs_j.num_inputs(), exp_primary, ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_dimensions_cross_multiply()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_dimensions_cross_multiply ---\n");
    printf("  cross_multiply: n=8, ss=2, ts=2, 4 constraints/step\n");

    auto st = make_cross_multiply<FieldT>();
    const size_t n = 8, ss = 2, ts = 2, cps = 4;
    const size_t new_per_step = n - ss;
    bool pass = true;

    for (size_t j = 1; j <= 4; ++j)
    {
        auto cs_j = build_composed_constraint_system(st, j);
        size_t exp_vars = n + (j - 1) * new_per_step;
        size_t exp_cons = j * cps;
        size_t exp_primary = ss + ts;

        bool ok = (cs_j.num_variables() == exp_vars) &&
                  (cs_j.num_constraints() == exp_cons) &&
                  (cs_j.num_inputs() == exp_primary);
        printf("  C_%zu: vars=%zu (exp %zu), constraints=%zu (exp %zu), primary=%zu (exp %zu) %s\n",
            j, cs_j.num_variables(), exp_vars,
            cs_j.num_constraints(), exp_cons,
            cs_j.num_inputs(), exp_primary, ok ? "" : "FAIL");
        if (!ok) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Category B: Constraint satisfaction (4)                                  */
/* ======================================================================== */

template<typename ppT>
bool test_satisfaction_poly_eval()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_satisfaction_poly_eval ---\n");
    printf("  s_0=2, (t1,t2)=(3,5)->w=6,s=30, (2,4)->w=60,s=240, (3,2)->w=720,s=1440\n");

    auto st = make_poly_eval<FieldT>();

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)}, {FieldT(30)}, {FieldT(240)}, {FieldT(1440)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(3), FieldT(5)}, {FieldT(2), FieldT(4)}, {FieldT(3), FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = {
        {FieldT(6)}, {FieldT(60)}, {FieldT(720)}
    };

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

template<typename ppT>
bool test_satisfaction_three_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_satisfaction_three_state ---\n");
    printf("  (a,b,c)=(2,3,5), t=7->(14,21,35), t=3->(42,63,105), t=2->(84,126,210)\n");

    auto st = make_three_state_accum<FieldT>();

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3), FieldT(5)},
        {FieldT(14), FieldT(21), FieldT(35)},
        {FieldT(42), FieldT(63), FieldT(105)},
        {FieldT(84), FieldT(126), FieldT(210)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(7)}, {FieldT(3)}, {FieldT(2)}
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

template<typename ppT>
bool test_satisfaction_chained_cube()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_satisfaction_chained_cube ---\n");
    printf("  s_0=2, t=3->w1=6,w2=18,s=54, t=2->w1=108,w2=216,s=432\n");

    auto st = make_chained_cube<FieldT>();

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)}, {FieldT(54)}, {FieldT(432)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(3)}, {FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = {
        {FieldT(6), FieldT(18)}, {FieldT(108), FieldT(216)}
    };

    bool pass = true;
    for (size_t j = 1; j <= 2; ++j)
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

template<typename ppT>
bool test_satisfaction_cross_multiply()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_satisfaction_cross_multiply ---\n");
    printf("  (a,b)=(2,3), (t1,t2)=(5,7)->w1=10,a=70,w2=21,b=105, (3,2)->...\n");

    auto st = make_cross_multiply<FieldT>();

    /* (a,b)=(2,3), (5,7)->w1=10,a=70,w2=21,b=105, (3,2)->w1=210,a=420,w2=210,b=630 */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)},
        {FieldT(70), FieldT(105)},
        {FieldT(420), FieldT(630)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5), FieldT(7)}, {FieldT(3), FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = {
        {FieldT(10), FieldT(21)}, {FieldT(210), FieldT(210)}
    };

    bool pass = true;
    for (size_t j = 1; j <= 2; ++j)
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
/* Category C: Witness sensitivity (3)                                      */
/*                                                                          */
/* These are the FIRST tests in the suite that verify the constraint system */
/* checks internal witness wires. Existing tests never had internal wires.  */
/* ======================================================================== */

template<typename ppT>
bool test_bad_witness_poly_eval()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_bad_witness_poly_eval ---\n");
    printf("  Correct w=6 (s_in=2, t1=3), replaced with w=99 -> CS not satisfied\n");

    auto st = make_poly_eval<FieldT>();

    /* Correct: s_0=2, (t1,t2)=(3,5), w=6, s_1=30 */
    /* Bad:     w=99 instead of 6 -> constraint s_in*t1=w fails (2*3=6 != 99) */
    std::vector<std::vector<FieldT> > states = { {FieldT(2)}, {FieldT(30)} };
    std::vector<std::vector<FieldT> > transitions = { {FieldT(3), FieldT(5)} };
    std::vector<std::vector<FieldT> > bad_witnesses = { {FieldT(99)} };

    auto cs_1 = build_composed_constraint_system(st, (size_t)1);
    auto assign = build_composed_assignment(st, (size_t)1, states, transitions, bad_witnesses);
    bool sat = cs_1.is_satisfied(assign.first, assign.second);

    printf("  C_1 with w=99: satisfied=%s (expected NO)\n", sat ? "YES" : "NO");
    bool pass = !sat;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_bad_witness_chained_cube()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_bad_witness_chained_cube ---\n");
    printf("  Correct w1=6 (s_in=2, t=3), replaced with w1=99 -> CS not satisfied\n");

    auto st = make_chained_cube<FieldT>();

    /* Correct: s_0=2, t=3, w1=6, w2=18, s_1=54 */
    /* Bad:     w1=99 instead of 6 -> constraint s_in*t=w1 fails (2*3=6 != 99) */
    std::vector<std::vector<FieldT> > states = { {FieldT(2)}, {FieldT(54)} };
    std::vector<std::vector<FieldT> > transitions = { {FieldT(3)} };
    std::vector<std::vector<FieldT> > bad_witnesses = { {FieldT(99), FieldT(18)} };

    auto cs_1 = build_composed_constraint_system(st, (size_t)1);
    auto assign = build_composed_assignment(st, (size_t)1, states, transitions, bad_witnesses);
    bool sat = cs_1.is_satisfied(assign.first, assign.second);

    printf("  C_1 with w1=99: satisfied=%s (expected NO)\n", sat ? "YES" : "NO");
    bool pass = !sat;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_bad_witness_cross_multiply()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_bad_witness_cross_multiply ---\n");
    printf("  Correct w2=21 (b_in=3, t2=7), replaced with w2=99 -> CS not satisfied\n");

    auto st = make_cross_multiply<FieldT>();

    /* Correct: (a,b)=(2,3), (t1,t2)=(5,7), w1=10, w2=21, a_out=70, b_out=105 */
    /* Bad:     w2=99 instead of 21 -> constraint b_in*t2=w2 fails (3*7=21 != 99) */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)}, {FieldT(70), FieldT(105)}
    };
    std::vector<std::vector<FieldT> > transitions = { {FieldT(5), FieldT(7)} };
    std::vector<std::vector<FieldT> > bad_witnesses = { {FieldT(10), FieldT(99)} };

    auto cs_1 = build_composed_constraint_system(st, (size_t)1);
    auto assign = build_composed_assignment(st, (size_t)1, states, transitions, bad_witnesses);
    bool sat = cs_1.is_satisfied(assign.first, assign.second);

    printf("  C_1 with w2=99: satisfied=%s (expected NO)\n", sat ? "YES" : "NO");
    bool pass = !sat;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


/* ======================================================================== */
/* Category D: End-to-end UVC (4)                                           */
/* ======================================================================== */

template<typename ppT>
bool test_e2e_poly_eval()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_e2e_poly_eval ---\n");

    auto st = make_poly_eval<FieldT>();

    /* s_0=2, (3,5)->w=6,s=30, (2,4)->w=60,s=240, (3,2)->w=720,s=1440 */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)}, {FieldT(30)}, {FieldT(240)}, {FieldT(1440)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(3), FieldT(5)}, {FieldT(2), FieldT(4)}, {FieldT(3), FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = {
        {FieldT(6)}, {FieldT(60)}, {FieldT(720)}
    };

    return run_e2e_uvc<ppT>(st, 3, 3, states, transitions, witnesses, "poly_eval");
}

template<typename ppT>
bool test_e2e_three_state()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_e2e_three_state ---\n");

    auto st = make_three_state_accum<FieldT>();

    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3), FieldT(5)},
        {FieldT(14), FieldT(21), FieldT(35)},
        {FieldT(42), FieldT(63), FieldT(105)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(7)}, {FieldT(3)}
    };
    std::vector<std::vector<FieldT> > witnesses = { {}, {} };

    return run_e2e_uvc<ppT>(st, 3, 2, states, transitions, witnesses, "three_state");
}

template<typename ppT>
bool test_e2e_chained_cube()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_e2e_chained_cube ---\n");

    auto st = make_chained_cube<FieldT>();

    /* s_0=2, t=3->w1=6,w2=18,s=54, t=2->w1=108,w2=216,s=432 */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)}, {FieldT(54)}, {FieldT(432)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(3)}, {FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = {
        {FieldT(6), FieldT(18)}, {FieldT(108), FieldT(216)}
    };

    return run_e2e_uvc<ppT>(st, 3, 2, states, transitions, witnesses, "chained_cube");
}

template<typename ppT>
bool test_e2e_cross_multiply()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_e2e_cross_multiply ---\n");

    auto st = make_cross_multiply<FieldT>();

    /* (a,b)=(2,3), (5,7)->w1=10,a=70,w2=21,b=105, (3,2)->w1=210,a=420,w2=210,b=630 */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(2), FieldT(3)},
        {FieldT(70), FieldT(105)},
        {FieldT(420), FieldT(630)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(5), FieldT(7)}, {FieldT(3), FieldT(2)}
    };
    std::vector<std::vector<FieldT> > witnesses = {
        {FieldT(10), FieldT(21)}, {FieldT(210), FieldT(210)}
    };

    return run_e2e_uvc<ppT>(st, 3, 2, states, transitions, witnesses, "cross_multiply");
}


/* ======================================================================== */
/* Category E: Many-step stress test (1)                                    */
/*                                                                          */
/* Pushes composition count to 10 (existing tests max at 3).                */
/* Uses poly_eval so every step has a witness wire.                         */
/* ======================================================================== */

template<typename ppT>
bool test_many_steps_poly_eval()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_many_steps_poly_eval ---\n");
    printf("  B=10, 10-step poly_eval chain with witness wires at every step\n");

    auto st = make_poly_eval<FieldT>();
    const size_t B = 10;

    /* Compute trace dynamically: s_out = s_in * t1 * t2 */
    std::vector<std::vector<FieldT> > states;
    std::vector<std::vector<FieldT> > transitions;
    std::vector<std::vector<FieldT> > witnesses;

    FieldT s = FieldT(2);
    states.push_back({s});

    /* Alternating transition pairs */
    FieldT t1_vals[10] = {
        FieldT(2), FieldT(3), FieldT(2), FieldT(3), FieldT(2),
        FieldT(3), FieldT(2), FieldT(3), FieldT(2), FieldT(3)
    };
    FieldT t2_vals[10] = {
        FieldT(3), FieldT(2), FieldT(3), FieldT(2), FieldT(3),
        FieldT(2), FieldT(3), FieldT(2), FieldT(3), FieldT(2)
    };

    for (size_t i = 0; i < B; ++i)
    {
        FieldT t1 = t1_vals[i];
        FieldT t2 = t2_vals[i];
        FieldT w = s * t1;
        s = w * t2;
        transitions.push_back({t1, t2});
        witnesses.push_back({w});
        states.push_back({s});
    }

    return run_e2e_uvc<ppT>(
        st, B, B, states, transitions, witnesses, "poly_eval 10-step stress");
}


/* ======================================================================== */
/* Category F: Soundness tests (2)                                          */
/* ======================================================================== */

/**
 * Corrupt g_A, g_B, g_C on a step-2 incremental proof.
 * First test in the suite to corrupt proof elements of a witness-bearing circuit.
 */
template<typename ppT>
bool test_soundness_bad_proof_elements()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_soundness_bad_proof_elements ---\n");
    printf("  Corrupt g_A/g_B/g_C of poly_eval step 2 proof -> all rejected\n");

    auto st = make_poly_eval<FieldT>();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)3);

    /* 2-step trace: s_0=2, (3,5)->w=6,s=30, (2,4)->w=60,s=240 */
    std::vector<std::vector<FieldT> > states = {
        {FieldT(2)}, {FieldT(30)}, {FieldT(240)}
    };
    std::vector<std::vector<FieldT> > transitions = {
        {FieldT(3), FieldT(5)}, {FieldT(2), FieldT(4)}
    };
    std::vector<std::vector<FieldT> > witnesses = {
        {FieldT(6)}, {FieldT(60)}
    };

    /* Generate valid proofs for step 1 and step 2 */
    auto assign1 = build_composed_assignment(st, (size_t)1, states, transitions, witnesses);
    auto proof1 = r1cs_uvc_ppzksnark_prover<ppT>(
        kp.pk, 1, assign1.first, assign1.second, nullptr);

    auto assign2 = build_composed_assignment(st, (size_t)2, states, transitions, witnesses);
    auto proof2 = r1cs_uvc_ppzksnark_prover<ppT>(
        kp.pk, 2, assign2.first, assign2.second, &proof1);

    /* Self-verify should pass */
    bool v_ok = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 2, assign2.first, states[2], proof2, proof1.g_D);
    printf("  Step 2 self-verify: %s\n", v_ok ? "PASS" : "FAIL");

    bool pass = v_ok;

    /* Corrupt g_A */
    {
        auto bad = proof2;
        bad.g_A = libff::G1<ppT>::random_element();
        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 2, assign2.first, states[2], bad, proof1.g_D);
        printf("  Corrupt g_A: %s\n", !v ? "rejected (PASS)" : "accepted (FAIL)");
        if (v) pass = false;
    }

    /* Corrupt g_B */
    {
        auto bad = proof2;
        bad.g_B = libff::G2<ppT>::random_element();
        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 2, assign2.first, states[2], bad, proof1.g_D);
        printf("  Corrupt g_B: %s\n", !v ? "rejected (PASS)" : "accepted (FAIL)");
        if (v) pass = false;
    }

    /* Corrupt g_C */
    {
        auto bad = proof2;
        bad.g_C = libff::G1<ppT>::random_element();
        bool v = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, 2, assign2.first, states[2], bad, proof1.g_D);
        printf("  Corrupt g_C: %s\n", !v ? "rejected (PASS)" : "accepted (FAIL)");
        if (v) pass = false;
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

/**
 * Two different poly_eval traces with different primary inputs.
 * Self-verify passes, cross-verify fails.
 */
template<typename ppT>
bool test_soundness_cross_trace()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n--- test_soundness_cross_trace ---\n");
    printf("  Trace A: s_0=2,(3,5)->30; Trace B: s_0=7,(2,3)->42. Cross-verify fails.\n");

    auto st = make_poly_eval<FieldT>();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, (size_t)2);

    /* Trace A: s_0=2, (t1,t2)=(3,5), w=6, s_1=30 */
    std::vector<std::vector<FieldT> > states_A = { {FieldT(2)}, {FieldT(30)} };
    std::vector<std::vector<FieldT> > trans_A = { {FieldT(3), FieldT(5)} };
    std::vector<std::vector<FieldT> > wit_A = { {FieldT(6)} };

    auto assign_A = build_composed_assignment(st, (size_t)1, states_A, trans_A, wit_A);
    auto proof_A = r1cs_uvc_ppzksnark_prover<ppT>(
        kp.pk, 1, assign_A.first, assign_A.second, nullptr);

    /* Trace B: s_0=7, (t1,t2)=(2,3), w=14, s_1=42 */
    std::vector<std::vector<FieldT> > states_B = { {FieldT(7)}, {FieldT(42)} };
    std::vector<std::vector<FieldT> > trans_B = { {FieldT(2), FieldT(3)} };
    std::vector<std::vector<FieldT> > wit_B = { {FieldT(14)} };

    auto assign_B = build_composed_assignment(st, (size_t)1, states_B, trans_B, wit_B);
    auto proof_B = r1cs_uvc_ppzksnark_prover<ppT>(
        kp.pk, 1, assign_B.first, assign_B.second, nullptr);

    bool pass = true;

    /* Self-verify */
    bool v_AA = r1cs_uvc_ppzksnark_verifier<ppT>(
        kp.vk, 1, assign_A.first, states_A[1], proof_A, libff::G1<ppT>::zero());
    bool v_BB = r1cs_uvc_ppzksnark_verifier<ppT>(
        kp.vk, 1, assign_B.first, states_B[1], proof_B, libff::G1<ppT>::zero());
    printf("  Trace A self-verify: %s\n", v_AA ? "PASS" : "FAIL");
    printf("  Trace B self-verify: %s\n", v_BB ? "PASS" : "FAIL");
    if (!v_AA || !v_BB) pass = false;

    /* Cross-verify: proof_A with primary_B, and vice versa */
    bool v_AB = r1cs_uvc_ppzksnark_verifier<ppT>(
        kp.vk, 1, assign_B.first, states_B[1], proof_A, libff::G1<ppT>::zero());
    bool v_BA = r1cs_uvc_ppzksnark_verifier<ppT>(
        kp.vk, 1, assign_A.first, states_A[1], proof_B, libff::G1<ppT>::zero());
    printf("  Proof A + primary B: %s\n", !v_AB ? "rejected (PASS)" : "accepted (FAIL)");
    printf("  Proof B + primary A: %s\n", !v_BA ? "rejected (PASS)" : "accepted (FAIL)");
    if (v_AB || v_BA) pass = false;

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


template<typename ppT>
bool test_e2e_sensor_fusion_fixture()
{
    typedef libff::Fr<ppT> FieldT;
    const size_t B = 3;
    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    const state_transition_circuit<FieldT> st = make_sensor_fusion_circuit<FieldT>(4);
    make_sensor_fusion_trace<FieldT>(B, 4, states, transitions, witnesses);
    const bool pass = run_e2e_uvc<ppT>(st, B, B, states, transitions, witnesses, "sensor fusion K=4");
    printf("  production sensor fusion chain: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_e2e_hadamard_fixture()
{
    typedef libff::Fr<ppT> FieldT;
    const size_t B = 3;
    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    const state_transition_circuit<FieldT> st = make_hadamard_circuit<FieldT>(4);
    make_hadamard_trace<FieldT>(B, 4, states, transitions, witnesses);
    const bool pass = run_e2e_uvc<ppT>(st, B, B, states, transitions, witnesses, "hadamard dim=4");
    printf("  production hadamard chain: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_e2e_mimc_fixture()
{
    typedef libff::Fr<ppT> FieldT;
    const size_t B = 3;
    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    const state_transition_circuit<FieldT> st = make_mimc_circuit<FieldT>(4);
    make_mimc_trace<FieldT>(B, 4, states, transitions, witnesses);
    const bool pass = run_e2e_uvc<ppT>(st, B, B, states, transitions, witnesses, "MiMC rounds=4");
    printf("  production MiMC chain: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_e2e_scalable_fixture()
{
    typedef libff::Fr<ppT> FieldT;
    const size_t B = 3;
    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    const state_transition_circuit<FieldT> st = make_scalable_circuit<FieldT>(8);
    make_scalable_trace<FieldT>(B, 8, states, transitions, witnesses);
    const bool pass = run_e2e_uvc<ppT>(st, B, B, states, transitions, witnesses, "scalable constraints=8");
    printf("  production scalable chain: %s\n", pass ? "PASS" : "FAIL");
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
    printf("Unit tests: complex circuits for UVC scheme\n");
    printf("================================================================\n");

    bool all_pass = true;

    /* A. Dimension tests */
    all_pass &= test_dimensions_poly_eval<libff::alt_bn128_pp>();
    all_pass &= test_dimensions_three_state<libff::alt_bn128_pp>();
    all_pass &= test_dimensions_chained_cube<libff::alt_bn128_pp>();
    all_pass &= test_dimensions_cross_multiply<libff::alt_bn128_pp>();

    /* B. Constraint satisfaction */
    all_pass &= test_satisfaction_poly_eval<libff::alt_bn128_pp>();
    all_pass &= test_satisfaction_three_state<libff::alt_bn128_pp>();
    all_pass &= test_satisfaction_chained_cube<libff::alt_bn128_pp>();
    all_pass &= test_satisfaction_cross_multiply<libff::alt_bn128_pp>();

    /* C. Witness sensitivity */
    all_pass &= test_bad_witness_poly_eval<libff::alt_bn128_pp>();
    all_pass &= test_bad_witness_chained_cube<libff::alt_bn128_pp>();
    all_pass &= test_bad_witness_cross_multiply<libff::alt_bn128_pp>();

    /* D. End-to-end UVC */
    all_pass &= test_e2e_poly_eval<libff::alt_bn128_pp>();
    all_pass &= test_e2e_three_state<libff::alt_bn128_pp>();
    all_pass &= test_e2e_chained_cube<libff::alt_bn128_pp>();
    all_pass &= test_e2e_cross_multiply<libff::alt_bn128_pp>();
    all_pass &= test_e2e_sensor_fusion_fixture<libff::alt_bn128_pp>();
    all_pass &= test_e2e_hadamard_fixture<libff::alt_bn128_pp>();
    all_pass &= test_e2e_mimc_fixture<libff::alt_bn128_pp>();
    all_pass &= test_e2e_scalable_fixture<libff::alt_bn128_pp>();

    /* E. Many-step stress */
    all_pass &= test_many_steps_poly_eval<libff::alt_bn128_pp>();

    /* F. Soundness */
    all_pass &= test_soundness_bad_proof_elements<libff::alt_bn128_pp>();
    all_pass &= test_soundness_cross_trace<libff::alt_bn128_pp>();

    printf("\n================================================================\n");
    printf("complex circuits: %s\n", all_pass ? "ALL 22 PASSED" : "SOME FAILED");
    printf("================================================================\n");
    return all_pass ? 0 : 1;
}
