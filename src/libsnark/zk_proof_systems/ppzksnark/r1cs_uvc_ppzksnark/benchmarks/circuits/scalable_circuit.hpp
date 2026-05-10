/** @file
*****************************************************************************

Configurable-size circuit using chained squarings for UVC benchmarks.

Generates a circuit with a target number of constraints by chaining
pure multiplicative operations: initial mix, then repeated squarings.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef BENCH_SCALABLE_CIRCUIT_HPP_
#define BENCH_SCALABLE_CIRCUIT_HPP_

#include <vector>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark_params.hpp>

namespace libsnark {

/**
 * Create a scalable circuit with a target number of constraints.
 *
 * Wire layout (1-indexed):
 *   Wire 1:     s_in  (state input)
 *   Wire 2:     t     (transition input)
 *   Wire 3:     s_out (state output)
 *   Wires 4+:   internal witness (chain intermediates)
 *
 * Constraint chain:
 *   Constraint 1:  s_in * t  = w_1
 *   Constraint 2:  w_1 * w_1 = w_2   (square)
 *   Constraint 3:  w_2 * w_2 = w_3   (square)
 *   ...
 *   Constraint target: w_{target-2} * w_{target-2} = s_out
 *
 * Wires/step: target_constraints + 2
 *   (s_in, t, s_out, plus target-1 intermediates)
 *
 * @param target_constraints  Number of constraints (e.g. 1024 or 16384)
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_scalable_circuit(size_t target_constraints)
{
    const size_t state_size = 1;
    const size_t transition_size = 1;
    /* Internal wires: one per constraint except last (which outputs to s_out) */
    const size_t num_internal = target_constraints - 1;
    const size_t num_variables = state_size + transition_size + state_size + num_internal;
    /* = target_constraints + 2 */

    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = state_size + transition_size; /* 2 */
    cs.auxiliary_input_size = num_variables - cs.primary_input_size;

    const size_t wire_s_in  = 1;
    const size_t wire_t     = 2;
    const size_t wire_s_out = 3;

    for (size_t c = 1; c <= target_constraints; ++c)
    {
        size_t lhs_wire, rhs_wire, out_wire;

        if (c == 1) {
            /* First constraint: s_in * t = w_1 */
            lhs_wire = wire_s_in;
            rhs_wire = wire_t;
        } else {
            /* Subsequent: w_{c-1} * w_{c-1} = w_c (squaring chain) */
            /* w_{c-1} is at wire 3 + (c-1) = c + 2, except w_1 is at wire 4 */
            size_t prev_wire;
            if (c == 2) {
                prev_wire = 4; /* w_1 */
            } else {
                prev_wire = c + 2; /* w_{c-1} */
            }
            lhs_wire = prev_wire;
            rhs_wire = prev_wire;
        }

        /* Output wire */
        if (c == target_constraints) {
            out_wire = wire_s_out;
        } else {
            out_wire = c + 3; /* w_c at wire c+3 */
        }

        linear_combination<FieldT> A, B, C;
        A.add_term(lhs_wire, FieldT::one());
        B.add_term(rhs_wire, FieldT::one());
        C.add_term(out_wire, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    return state_transition_circuit<FieldT>(cs, state_size, transition_size);
}

/**
 * Generate an execution trace for the scalable circuit.
 *
 * @param num_steps           Number of sequential steps
 * @param target_constraints  Constraints per step (must match circuit)
 * @param states              Output: states[0..num_steps]
 * @param transitions         Output: transitions[0..num_steps-1]
 * @param witnesses           Output: per-step internal witness
 */
template<typename FieldT>
void make_scalable_trace(
    size_t num_steps,
    size_t target_constraints,
    std::vector<std::vector<FieldT> > &states,
    std::vector<std::vector<FieldT> > &transitions,
    std::vector<std::vector<FieldT> > &witnesses)
{
    states.clear(); transitions.clear(); witnesses.clear();

    FieldT s = FieldT(2); /* s_0 */
    states.push_back({s});

    for (size_t k = 1; k <= num_steps; ++k)
    {
        FieldT t = FieldT(3 + k);
        transitions.push_back({t});

        std::vector<FieldT> internal_witness;

        /* Constraint 1: s * t = w_1 */
        FieldT w_prev = s * t;
        internal_witness.push_back(w_prev);

        /* Constraints 2..target-1: repeated squaring */
        for (size_t c = 2; c < target_constraints; ++c)
        {
            FieldT w_next = w_prev * w_prev;
            internal_witness.push_back(w_next);
            w_prev = w_next;
        }

        /* Constraint target: squaring -> s_out */
        FieldT s_out = w_prev * w_prev;

        s = s_out;
        states.push_back({s});
        witnesses.push_back(internal_witness);
    }
}

} // libsnark

#endif // BENCH_SCALABLE_CIRCUIT_HPP_
