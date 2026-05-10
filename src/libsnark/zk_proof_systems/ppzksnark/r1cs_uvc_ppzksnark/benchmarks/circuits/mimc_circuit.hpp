/** @file
*****************************************************************************

MiMC-like hash chain circuit for UVC benchmarks.

Per step: s_out = MiMC(s_in, t) using rounds of x^3 S-box.
Each round uses 3 pure multiplicative constraints:
  w_mix  = prev * t       (mixing with transition input)
  w_sq   = w_mix * w_mix  (square)
  w_cube = w_sq  * w_mix  (cube)

All constraints are pure multiplicative (no constant wire in A or B),
which is required by the UVC scheme for zero-padded future wires.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef BENCH_MIMC_CIRCUIT_HPP_
#define BENCH_MIMC_CIRCUIT_HPP_

#include <vector>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark_params.hpp>

namespace libsnark {

/**
 * Create a MiMC-like state transition circuit with pure multiplicative gates.
 *
 * Wire layout (1-indexed, wire 0 is the constant wire):
 *   Wire 1:       s_in  (state input)
 *   Wire 2:       t     (transition input)
 *   Wire 3:       s_out (state output)
 *   Wires 4+:     internal witness (round intermediates)
 *
 * For round r (1-indexed, 1 <= r <= rounds):
 *   Input: s_in (r=1) or w_cube_{r-1} (r>1)
 *   Constraint 3r-2: input * t     = w_mix_r
 *   Constraint 3r-1: w_mix_r * w_mix_r = w_sq_r
 *   Constraint 3r:   w_sq_r * w_mix_r  = w_cube_r  (or s_out for r=rounds)
 *
 * Constraints/step: 3 * rounds
 * Wires/step:       rounds * 3 + 3 - 1 = 3*rounds + 2
 *   (s_in, t, s_out, plus 3 intermediates per round minus 1 because last cube = s_out)
 *
 * @param rounds  Number of x^3 rounds (default 90, giving 270 constraints)
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_mimc_circuit(size_t rounds = 90)
{
    /* Wire counts */
    const size_t state_size = 1;
    const size_t transition_size = 1;
    /* Internal wires: 3 per round, except last round's cube output is s_out */
    const size_t num_internal = 3 * rounds - 1;
    const size_t num_variables = state_size + transition_size + state_size + num_internal;
    /* = 1 + 1 + 1 + (3*rounds - 1) = 3*rounds + 2 */

    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = state_size + transition_size; /* [s_in, t] */
    cs.auxiliary_input_size = num_variables - cs.primary_input_size;

    const size_t wire_s_in  = 1;
    const size_t wire_t     = 2;
    const size_t wire_s_out = 3;

    for (size_t r = 1; r <= rounds; ++r)
    {
        /* Input wire for this round */
        size_t input_wire;
        if (r == 1) {
            input_wire = wire_s_in;
        } else {
            /* w_cube_{r-1}: wire 4 + 3*(r-2) + 2 = 3*r */
            input_wire = 3 * r;
        }

        /* w_mix_r wire: 4 + 3*(r-1) = 3*r + 1 */
        size_t wire_mix = 3 * r + 1;

        /* w_sq_r wire: 4 + 3*(r-1) + 1 = 3*r + 2 */
        size_t wire_sq = 3 * r + 2;

        /* w_cube_r wire: s_out for last round, else 4 + 3*(r-1) + 2 = 3*r + 3 */
        size_t wire_cube;
        if (r == rounds) {
            wire_cube = wire_s_out;
        } else {
            wire_cube = 3 * r + 3;
        }

        /* Constraint 1: input * t = w_mix */
        {
            linear_combination<FieldT> A, B, C;
            A.add_term(input_wire, FieldT::one());
            B.add_term(wire_t, FieldT::one());
            C.add_term(wire_mix, FieldT::one());
            cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
        }

        /* Constraint 2: w_mix * w_mix = w_sq */
        {
            linear_combination<FieldT> A, B, C;
            A.add_term(wire_mix, FieldT::one());
            B.add_term(wire_mix, FieldT::one());
            C.add_term(wire_sq, FieldT::one());
            cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
        }

        /* Constraint 3: w_sq * w_mix = w_cube (x^3) */
        {
            linear_combination<FieldT> A, B, C;
            A.add_term(wire_sq, FieldT::one());
            B.add_term(wire_mix, FieldT::one());
            C.add_term(wire_cube, FieldT::one());
            cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
        }
    }

    return state_transition_circuit<FieldT>(cs, state_size, transition_size);
}

/**
 * Generate an execution trace for the MiMC circuit.
 *
 * Computes: s_j = MiMC(s_{j-1}, t_j) for j = 1..num_steps
 * where MiMC applies `rounds` iterations of x -> (x * t)^3.
 *
 * @param num_steps    Number of sequential steps
 * @param rounds       Number of x^3 rounds per step (must match circuit)
 * @param states       Output: states[0..num_steps], each a 1-element vector
 * @param transitions  Output: transitions[0..num_steps-1]
 * @param witnesses    Output: per-step internal witness vectors
 */
template<typename FieldT>
void make_mimc_trace(
    size_t num_steps,
    size_t rounds,
    std::vector<std::vector<FieldT> > &states,
    std::vector<std::vector<FieldT> > &transitions,
    std::vector<std::vector<FieldT> > &witnesses)
{
    states.clear(); transitions.clear(); witnesses.clear();

    FieldT s = FieldT(2); /* s_0 */
    states.push_back({s});

    for (size_t k = 1; k <= num_steps; ++k)
    {
        FieldT t = FieldT(3 + k); /* different transition each step */
        transitions.push_back({t});

        /* Compute MiMC rounds and collect internal witness */
        std::vector<FieldT> internal_witness;
        FieldT prev = s;

        for (size_t r = 1; r <= rounds; ++r)
        {
            FieldT w_mix = prev * t;
            FieldT w_sq  = w_mix * w_mix;
            FieldT w_cube = w_sq * w_mix;

            internal_witness.push_back(w_mix);
            internal_witness.push_back(w_sq);

            if (r < rounds) {
                /* w_cube goes to internal wire */
                internal_witness.push_back(w_cube);
            }
            /* else: w_cube is s_out, stored in states */

            prev = w_cube;
        }

        s = prev; /* s_out = final w_cube */
        states.push_back({s});
        witnesses.push_back(internal_witness);
    }
}

} // libsnark

#endif // BENCH_MIMC_CIRCUIT_HPP_
