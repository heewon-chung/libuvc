/** @file
*****************************************************************************

Weighted multi-sensor fusion (EMA) circuit for UVC benchmarks.

Per step: s_out = sum_{k=1}^{K} w_k * x_k + beta * s_in
where w_k are fixed inverse-variance weights (var_k = k+1 for k=1..K, normalized),
x_k are K sensor readings, s_in is the previous fused estimate, and
beta = 1/10 is the EMA decay parameter.

Uses K+1 constraints per step via the one_t trick:
  Constraints 1..K:  A = x_k,     B = w_k * one_t,  C = p_k
  Constraint  K+1:   A = one_t,   B = p_1 + ... + p_K + beta * s_in,  C = s_out

Wire layout (1-indexed, wire 0 = constant, unused in A/B):
  Wire  [1]:            s_in            (state input: previous fused value)
  Wires [2..K+1]:       x_1,...,x_K     (transition: K sensor readings)
  Wire  [K+2]:          one_t           (transition: always = 1)
  Wire  [K+3]:          s_out           (state output: new fused value)
  Wires [K+4..2K+3]:    p_1,...,p_K     (internal witness: partial products)

  state_size = 1
  transition_size = K + 1
  num_variables = 2*K + 3

All gates are pure multiplicative — no wire 0 in any A or B linear combination.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef BENCH_SENSOR_FUSION_CIRCUIT_HPP_
#define BENCH_SENSOR_FUSION_CIRCUIT_HPP_

#include <vector>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark_params.hpp>

namespace libsnark {

/**
 * Compute normalized inverse-variance weights for K sensors.
 *
 * variance_k = k + 1  for k = 1..K  (0-indexed: k+2 for k = 0..K-1)
 * raw_k = 1 / variance_k
 * w_k = raw_k / sum(raw) * (1 - beta)  (normalized so sum(w_k) = 1 - beta = 9/10)
 *
 * @param K  Number of sensors
 * @return   Vector of K normalized weights as field elements
 */
template<typename FieldT>
std::vector<FieldT> compute_sensor_weights(size_t K)
{
    std::vector<FieldT> raw(K);
    FieldT sum = FieldT::zero();
    for (size_t k = 0; k < K; ++k)
    {
        raw[k] = FieldT(k + 2).inverse(); /* var_k = k+2 when k is 0-indexed (sensor 1 has var=2) */
        sum = sum + raw[k];
    }

    FieldT sum_inv = sum.inverse();
    FieldT beta = FieldT(10).inverse();          /* beta = 1/10 */
    FieldT one_minus_beta = FieldT::one() - beta; /* 1 - beta = 9/10 */
    std::vector<FieldT> weights(K);
    for (size_t k = 0; k < K; ++k)
        weights[k] = raw[k] * sum_inv * one_minus_beta; /* sum(w_k) = 1 - beta */

    return weights;
}

/**
 * Create a sensor fusion circuit with pure multiplicative gates.
 *
 * @param K  Number of sensors (constraints = K+1, wires = 2*K + 3)
 */
template<typename FieldT>
state_transition_circuit<FieldT> make_sensor_fusion_circuit(size_t K)
{
    const size_t state_size = 1;
    const size_t transition_size = K + 1; /* K sensor readings + one_t */
    const size_t num_variables = 2 * K + 3; /* s_in + K sensors + one_t + s_out + K partials */

    auto weights = compute_sensor_weights<FieldT>(K);

    /* EMA decay: beta = 1/10 */
    FieldT beta = FieldT(10).inverse();

    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = state_size + transition_size; /* K + 2 */
    cs.auxiliary_input_size = num_variables - cs.primary_input_size; /* K + 1 */

    /* Wire indices (1-indexed) */
    const size_t wire_s_in = 1;
    const size_t wire_one_t = K + 2;
    const size_t wire_s_out = K + 3;
    /* x_k at wire (1 + k) for k = 1..K, i.e., wires 2..K+1 */
    /* p_k at wire (K + 3 + k) for k = 1..K, i.e., wires K+4..2K+3 */

    /* Constraints 1..K: p_k = w_k * x_k
     * A = x_k,  B = w_k * one_t,  C = p_k
     * Pure multiplicative: A uses wire >= 2, B uses wire K+2, no wire 0.
     */
    for (size_t k = 0; k < K; ++k)
    {
        size_t wire_x_k = 2 + k;     /* wires 2..K+1 */
        size_t wire_p_k = K + 4 + k; /* wires K+4..2K+3 */

        linear_combination<FieldT> A, B, C;
        A.add_term(wire_x_k, FieldT::one());
        B.add_term(wire_one_t, weights[k]);
        C.add_term(wire_p_k, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    /* Constraint K+1: s_out = sum(p_k) + beta * s_in
     * A = one_t
     * B = p_1 + p_2 + ... + p_K + beta * s_in
     * C = s_out
     *
     * Expands to: one_t * (sum(p_k) + beta * s_in) = s_out
     * Since one_t = 1: s_out = sum(p_k) + beta * s_in
     *
     * Pure multiplicative: A uses wire K+2, B uses wires >= K+4 and wire 1.
     */
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(wire_one_t, FieldT::one());

        for (size_t k = 0; k < K; ++k)
        {
            size_t wire_p_k = K + 4 + k;
            B.add_term(wire_p_k, FieldT::one());
        }
        B.add_term(wire_s_in, beta);

        C.add_term(wire_s_out, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }

    return state_transition_circuit<FieldT>(cs, state_size, transition_size);
}

/**
 * Generate an execution trace for the sensor fusion circuit.
 *
 * Computes: s_{j+1} = sum(w_k * x_{j,k}) + beta * s_j  for each step.
 * Initial state s_0 = 0. Sensor readings: x_{j,k} = j * K + k + 1 (deterministic).
 *
 * @param num_steps    Number of sequential steps
 * @param K            Number of sensors (must match circuit)
 * @param states       Output: states[0..num_steps], each a 1-element vector
 * @param transitions  Output: transitions[0..num_steps-1], each = {x_1,...,x_K, 1}
 * @param witnesses    Output: per-step internal witness {p_1,...,p_K}
 */
template<typename FieldT>
void make_sensor_fusion_trace(
    size_t num_steps,
    size_t K,
    std::vector<std::vector<FieldT> > &states,
    std::vector<std::vector<FieldT> > &transitions,
    std::vector<std::vector<FieldT> > &witnesses)
{
    states.clear(); transitions.clear(); witnesses.clear();

    auto weights = compute_sensor_weights<FieldT>(K);
    FieldT beta = FieldT(10).inverse();

    /* s_0 = 0 */
    std::vector<FieldT> s = { FieldT::zero() };
    states.push_back(s);

    for (size_t step = 1; step <= num_steps; ++step)
    {
        /* Transition: {x_1, ..., x_K, one_t = 1} */
        std::vector<FieldT> t(K + 1);
        for (size_t k = 0; k < K; ++k)
            t[k] = FieldT((step - 1) * K + k + 1); /* deterministic readings */
        t[K] = FieldT::one(); /* one_t */
        transitions.push_back(t);

        /* Witness: p_k = w_k * x_k */
        std::vector<FieldT> w(K);
        FieldT sum_p = FieldT::zero();
        for (size_t k = 0; k < K; ++k)
        {
            w[k] = weights[k] * t[k];
            sum_p = sum_p + w[k];
        }
        witnesses.push_back(w);

        /* State output: s_out = sum(p_k) + beta * s_in */
        FieldT s_out = sum_p + beta * s[0];
        s = { s_out };
        states.push_back(s);
    }
}

} // libsnark

#endif // BENCH_SENSOR_FUSION_CIRCUIT_HPP_
