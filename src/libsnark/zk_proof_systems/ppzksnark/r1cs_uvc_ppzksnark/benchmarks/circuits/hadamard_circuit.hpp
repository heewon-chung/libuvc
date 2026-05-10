/** @file
*****************************************************************************

Hadamard (element-wise) product circuit for UVC benchmarks.

Per step: s_out[i] = s_in[i] * t[i] for dimension d.
Each constraint is a single pure multiplicative gate.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef BENCH_HADAMARD_CIRCUIT_HPP_
#define BENCH_HADAMARD_CIRCUIT_HPP_

#include <vector>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark_params.hpp>

namespace libsnark {

/**
 * Create a Hadamard product circuit with pure multiplicative gates.
 *
 * Wire layout (1-indexed):
 *   Wires [1, d]:       s_in[0..d-1]  (state input)
 *   Wires [d+1, 2d]:    t[0..d-1]     (transition input)
 *   Wires [2d+1, 3d]:   s_out[0..d-1] (state output)
 *
 * Constraints: d (one per dimension)
 *   s_in[i] * t[i] = s_out[i]   for i = 0..d-1
 *
 * @param dim  Dimension d (e.g. 16 or 32)
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_hadamard_circuit(size_t dim)
{
    const size_t state_size = dim;
    const size_t transition_size = dim;
    const size_t num_variables = 3 * dim; /* s_in + t + s_out */

    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = state_size + transition_size; /* 2*dim */
    cs.auxiliary_input_size = num_variables - cs.primary_input_size; /* dim */

    for (size_t i = 0; i < dim; ++i)
    {
        size_t wire_s_in  = 1 + i;              /* s_in[i] */
        size_t wire_t     = dim + 1 + i;         /* t[i] */
        size_t wire_s_out = 2 * dim + 1 + i;     /* s_out[i] */

        linear_combination<FieldT> A, B, C;
        A.add_term(wire_s_in, FieldT::one());
        B.add_term(wire_t, FieldT::one());
        C.add_term(wire_s_out, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    return state_transition_circuit<FieldT>(cs, state_size, transition_size);
}

/**
 * Generate an execution trace for the Hadamard product circuit.
 *
 * Computes: s_j[i] = s_{j-1}[i] * t_j[i] for each dimension i.
 *
 * @param num_steps    Number of sequential steps
 * @param dim          Dimension (must match circuit)
 * @param states       Output: states[0..num_steps], each a dim-element vector
 * @param transitions  Output: transitions[0..num_steps-1]
 * @param witnesses    Output: per-step internal witness (empty, no internal wires)
 */
template<typename FieldT>
void make_hadamard_trace(
    size_t num_steps,
    size_t dim,
    std::vector<std::vector<FieldT> > &states,
    std::vector<std::vector<FieldT> > &transitions,
    std::vector<std::vector<FieldT> > &witnesses)
{
    states.clear(); transitions.clear(); witnesses.clear();

    /* s_0: initialize with small field elements */
    std::vector<FieldT> s(dim);
    for (size_t i = 0; i < dim; ++i)
        s[i] = FieldT(2 + i);
    states.push_back(s);

    for (size_t k = 1; k <= num_steps; ++k)
    {
        /* Transition: different per step */
        std::vector<FieldT> t(dim);
        for (size_t i = 0; i < dim; ++i)
            t[i] = FieldT(3 + k + i);
        transitions.push_back(t);

        /* Compute s_out */
        std::vector<FieldT> s_out(dim);
        for (size_t i = 0; i < dim; ++i)
            s_out[i] = s[i] * t[i];

        s = s_out;
        states.push_back(s);
        witnesses.push_back({}); /* no internal witness */
    }
}

} // libsnark

#endif // BENCH_HADAMARD_CIRCUIT_HPP_
