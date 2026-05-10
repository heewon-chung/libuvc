/** @file
*****************************************************************************

PageRank (power iteration) circuit for UVC benchmarks.

Per step: s_out = d * M * s_in + (1-d)/N
where M is a column-stochastic transition matrix for a deterministic graph,
d is the damping factor (0.85), and N is the number of nodes.

Uses 2*N constraints per step via multi-term linear combinations in A.
This is the first circuit in the UVC codebase to exercise multi-term A LCs.

Wire layout (1-indexed, wire 0 = constant, unused in A/B):
  Wires [1..N]:         s_in[0..N-1]     (state input)
  Wire  [N+1]:          d_wire            (transition: damping factor)
  Wire  [N+2]:          one_t             (transition: always 1)
  Wires [N+3..2N+2]:    s_out[0..N-1]    (state output)
  Wires [2N+3..3N+2]:   w[0..N-1]        (internal: d * (M * s_in)[k])

Constraint types (pure multiplicative, no wire 0 in A or B):
  Type 1: A = sum_j M[k][j] * s_in[j],  B = d_wire,  C = w[k]
  Type 2: A = one_t,  B = w[k] + bias * one_t,  C = s_out[k]

one_t = 1 is enforced as a public/transition input visible to the verifier.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef BENCH_PAGERANK_CIRCUIT_HPP_
#define BENCH_PAGERANK_CIRCUIT_HPP_

#include <vector>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark_params.hpp>

namespace libsnark {

/**
 * Generate a deterministic directed graph for PageRank benchmarks.
 *
 * Construction:
 *   - Cycle: node i -> node (i+1) % N  (ensures strong connectivity)
 *   - Extra: node i -> node (2*i+1) % N (adds non-trivial structure)
 *
 * @param N  Number of nodes
 * @return   Adjacency list: adj[j] = set of nodes that j points to
 */
inline std::vector<std::vector<size_t> > make_deterministic_graph(size_t N)
{
    std::vector<std::vector<size_t> > adj(N);
    for (size_t i = 0; i < N; ++i)
    {
        /* Cycle edge */
        size_t cycle_target = (i + 1) % N;
        adj[i].push_back(cycle_target);

        /* Extra edge */
        size_t extra_target = (2 * i + 1) % N;
        /* Avoid duplicates */
        bool dup = false;
        for (size_t t : adj[i])
            if (t == extra_target) { dup = true; break; }
        if (!dup)
            adj[i].push_back(extra_target);
    }
    return adj;
}

/**
 * Build column-stochastic transition matrix from adjacency list.
 *
 * M[k][j] = 1/out_degree(j) if edge j->k exists, else 0.
 * Returns dense matrix as vector of vectors of field elements.
 */
template<typename FieldT>
std::vector<std::vector<FieldT> > build_transition_matrix(
    size_t N, const std::vector<std::vector<size_t> > &adj)
{
    std::vector<std::vector<FieldT> > M(N, std::vector<FieldT>(N, FieldT::zero()));

    for (size_t j = 0; j < N; ++j)
    {
        FieldT inv_deg = FieldT(adj[j].size()).inverse();
        for (size_t target : adj[j])
            M[target][j] = inv_deg;
    }
    return M;
}

/**
 * Create a PageRank circuit with pure multiplicative gates.
 *
 * @param N  Number of nodes (constraints = 2*N, wires = 3*N + 2)
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_pagerank_circuit(size_t N)
{
    const size_t state_size = N;
    const size_t transition_size = 2; /* d_wire, one_t */
    const size_t num_variables = 3 * N + 2; /* s_in(N) + trans(2) + s_out(N) + w(N) */

    /* Build graph and matrix */
    auto adj = make_deterministic_graph(N);
    auto M = build_transition_matrix<FieldT>(N, adj);

    /* Compute bias = (1 - d) / N where d = 85/100 */
    FieldT d = FieldT(85) * FieldT(100).inverse();
    FieldT bias = (FieldT::one() - d) * FieldT(N).inverse();

    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = state_size + transition_size; /* N + 2 */
    cs.auxiliary_input_size = num_variables - cs.primary_input_size; /* 2*N */

    for (size_t k = 0; k < N; ++k)
    {
        /* Wire indices */
        size_t wire_d     = N + 1;
        size_t wire_one_t = N + 2;
        size_t wire_s_out = N + 3 + k;
        size_t wire_w     = 2 * N + 3 + k;

        /* Constraint 1: w[k] = d * sum_j M[k][j] * s_in[j]
         * A = sum_j M[k][j] * wire(s_in[j])   (multi-term LC)
         * B = wire(d_wire)
         * C = wire(w[k])
         */
        {
            linear_combination<FieldT> A, B, C;
            for (size_t j = 0; j < N; ++j)
            {
                if (M[k][j] != FieldT::zero())
                {
                    size_t wire_s_in_j = 1 + j;
                    A.add_term(wire_s_in_j, M[k][j]);
                }
            }
            B.add_term(wire_d, FieldT::one());
            C.add_term(wire_w, FieldT::one());
            cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
        }

        /* Constraint 2: s_out[k] = w[k] + bias  (via one_t trick)
         * A = wire(one_t)
         * B = wire(w[k]) + bias * wire(one_t)
         * C = wire(s_out[k])
         *
         * Expands to: one_t * w[k] + bias * one_t^2 = s_out[k]
         * Since one_t = 1 (enforced by verifier): s_out[k] = w[k] + bias
         */
        {
            linear_combination<FieldT> A, B, C;
            A.add_term(wire_one_t, FieldT::one());
            B.add_term(wire_w, FieldT::one());
            B.add_term(wire_one_t, bias);
            C.add_term(wire_s_out, FieldT::one());
            cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
        }
    }

    return state_transition_circuit<FieldT>(cs, state_size, transition_size);
}

/**
 * Generate an execution trace for the PageRank circuit.
 *
 * Computes: s_{j+1} = d * M * s_j + bias  for each step.
 * Initial state s_0 = uniform distribution (1/N for each node).
 *
 * @param num_steps    Number of sequential steps
 * @param N            Number of nodes (must match circuit)
 * @param states       Output: states[0..num_steps], each an N-element vector
 * @param transitions  Output: transitions[0..num_steps-1], each = {d, 1}
 * @param witnesses    Output: per-step internal witness w[0..N-1]
 */
template<typename FieldT>
void make_pagerank_trace(
    size_t num_steps,
    size_t N,
    std::vector<std::vector<FieldT> > &states,
    std::vector<std::vector<FieldT> > &transitions,
    std::vector<std::vector<FieldT> > &witnesses)
{
    states.clear(); transitions.clear(); witnesses.clear();

    /* Build graph and matrix */
    auto adj = make_deterministic_graph(N);
    auto M = build_transition_matrix<FieldT>(N, adj);

    /* Field constants */
    FieldT d = FieldT(85) * FieldT(100).inverse();
    FieldT bias = (FieldT::one() - d) * FieldT(N).inverse();

    /* s_0 = uniform distribution */
    FieldT inv_N = FieldT(N).inverse();
    std::vector<FieldT> s(N, inv_N);
    states.push_back(s);

    for (size_t step = 1; step <= num_steps; ++step)
    {
        /* Transition input: {d, 1} */
        std::vector<FieldT> t = { d, FieldT::one() };
        transitions.push_back(t);

        /* Compute M * s_in */
        std::vector<FieldT> Mv(N, FieldT::zero());
        for (size_t k = 0; k < N; ++k)
            for (size_t j = 0; j < N; ++j)
                Mv[k] = Mv[k] + M[k][j] * s[j];

        /* Witness: w[k] = d * (M * s_in)[k] */
        std::vector<FieldT> w(N);
        for (size_t k = 0; k < N; ++k)
            w[k] = d * Mv[k];
        witnesses.push_back(w);

        /* State output: s_out[k] = w[k] + bias */
        std::vector<FieldT> s_out(N);
        for (size_t k = 0; k < N; ++k)
            s_out[k] = w[k] + bias;

        s = s_out;
        states.push_back(s);
    }
}

} // libsnark

#endif // BENCH_PAGERANK_CIRCUIT_HPP_
