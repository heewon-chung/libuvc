/** @file
*****************************************************************************

Implementation of the Updatable Verifiable Computation (UVC) scheme.

See r1cs_uvc_ppzksnark.hpp for details.

KEY DESIGN DECISIONS:
  1. A SINGLE QAP from C_B (the maximum composed circuit) is used for ALL
     CRS elements. This ensures all polynomial evaluations share the same
     evaluation domain and vanishing polynomial Z_B(x).

  2. For step j, h_j is computed by padding the I_j assignment with zeros
     for wires in I_B \ I_j, then computing the QAP witness map on C_B.
     This works for "well-structured" circuits where all R1CS constraints
     are pure multiplicative gates (no constant terms in A/B vectors),
     because the zero-padded assignment satisfies future constraints as
     0 * 0 = 0.

  3. Public inputs are fixed as s_0 (initial state) and t_1 (first
     transition) across all steps. The output s_j is part of the witness.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef R1CS_UVC_PPZKSNARK_TCC_
#define R1CS_UVC_PPZKSNARK_TCC_

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>

#include <libff/algebra/scalar_multiplication/multiexp.hpp>
#include <libff/common/profiling.hpp>
#include <libff/common/utils.hpp>
#include <libfqfft/evaluation_domain/get_evaluation_domain.hpp>

#ifdef MULTICORE
#include <omp.h>
#endif

#include <libsnark/knowledge_commitment/kc_multiexp.hpp>
#include <libsnark/reductions/r1cs_to_qap/r1cs_to_qap.hpp>

namespace libsnark {

/**
 * Build the composed R1CS constraint system C_j from j copies of the base circuit.
 *
 * Wire numbering for C_j (with wire sharing for state continuity):
 *   Wire 0: constant 1
 *   Step 1: wires [1, n]  (n = base_cs.num_variables())
 *     - [1, ss] = s_0 (initial state input)
 *     - [ss+1, ss+ts] = t_1
 *     - [ss+ts+1, 2*ss+ts] = s_1 (output state)
 *     - [2*ss+ts+1, n] = internal witness
 *   Step k (k>1): new_per_step = n - ss new wires
 *     - State input: reuse s_{k-1} output wires from previous step
 *     - New wires: t_k, s_k output, internal witness
 *
 * Public inputs (for R1CS): s_0 and t_1 (fixed across all steps).
 */
template<typename FieldT>
r1cs_constraint_system<FieldT> build_composed_constraint_system(
    const state_transition_circuit<FieldT> &st_circuit,
    size_t j)
{
    libff::enter_block("Build composed constraint system C_j");
    assert(j >= 1);

    const r1cs_constraint_system<FieldT> &base = st_circuit.base_cs;
    const size_t n = base.num_variables();
    const size_t ss = st_circuit.state_size;
    const size_t ts = st_circuit.transition_size;
    const size_t new_per_step = n - ss;

    const size_t total_vars = n + (j - 1) * new_per_step;

    r1cs_constraint_system<FieldT> composed;

    /* Wire mapping: for step k, map base wire index to composed wire index */
    auto wire_map = [&](size_t step_k, size_t base_wire) -> size_t {
        if (base_wire == 0) return 0; /* constant wire */

        if (base_wire <= ss) {
            /* State input wires: reuse previous step's output */
            if (step_k == 1) {
                return base_wire; /* s_0 at wires [1, ss] */
            } else {
                /* State output of step k-1 */
                size_t prev_output_start = (step_k - 2) * new_per_step + ss + ts + 1;
                return prev_output_start + (base_wire - 1);
            }
        } else {
            /* Non-state-input wires: t_k, s_k output, internal */
            size_t local_offset = base_wire - ss;
            size_t global_start = (step_k - 1) * new_per_step + ss;
            return global_start + local_offset;
        }
    };

    /* Add constraints for each step */
    for (size_t k = 1; k <= j; ++k)
    {
        for (size_t c = 0; c < base.constraints.size(); ++c)
        {
            const r1cs_constraint<FieldT> &orig = base.constraints[c];
            r1cs_constraint<FieldT> mapped;

            for (const auto &term : orig.a.terms)
                mapped.a.terms.emplace_back(
                    linear_term<FieldT>(wire_map(k, term.index), term.coeff));

            for (const auto &term : orig.b.terms)
                mapped.b.terms.emplace_back(
                    linear_term<FieldT>(wire_map(k, term.index), term.coeff));

            for (const auto &term : orig.c.terms)
                mapped.c.terms.emplace_back(
                    linear_term<FieldT>(wire_map(k, term.index), term.coeff));

            composed.constraints.emplace_back(std::move(mapped));
        }
    }

    composed.primary_input_size = ss + ts;
    composed.auxiliary_input_size = total_vars - composed.primary_input_size;

    libff::print_indent(); printf("* Composed C_%zu: %zu constraints, %zu variables, %zu inputs\n",
        j, composed.num_constraints(), total_vars, composed.primary_input_size);

    libff::leave_block("Build composed constraint system C_j");
    return composed;
}


/**
 * Build the full variable assignment for C_j.
 *
 * Returns (primary, auxiliary) where primary = [s_0, t_1] and
 * auxiliary = [rest of step 1 wires, step 2 new wires, ..., step j new wires].
 */
template<typename FieldT>
std::pair<r1cs_primary_input<FieldT>, r1cs_auxiliary_input<FieldT>>
build_composed_assignment(
    const state_transition_circuit<FieldT> &st_circuit,
    size_t j,
    const std::vector<std::vector<FieldT> > &states,
    const std::vector<std::vector<FieldT> > &transitions,
    const std::vector<std::vector<FieldT> > &step_witnesses)
{
    assert(states.size() == j + 1);
    assert(transitions.size() == j);
    assert(step_witnesses.size() == j);

    const size_t ss = st_circuit.state_size;
    const size_t ts = st_circuit.transition_size;
    const size_t n = st_circuit.base_cs.num_variables();
    const size_t new_per_step = n - ss;
    const size_t total_vars = n + (j - 1) * new_per_step;

    std::vector<FieldT> full_assignment(total_vars, FieldT::zero());

    /* s_0: wires [1, ss] -> indices [0, ss-1] in assignment */
    for (size_t i = 0; i < ss; ++i)
        full_assignment[i] = states[0][i];

    /* For each step k = 1..j: fill non-state-input wires */
    for (size_t k = 1; k <= j; ++k)
    {
        size_t base_offset = (k - 1) * new_per_step + ss;

        /* t_k */
        for (size_t i = 0; i < ts; ++i)
            full_assignment[base_offset + i] = transitions[k-1][i];

        /* s_k (output state) */
        for (size_t i = 0; i < ss; ++i)
            full_assignment[base_offset + ts + i] = states[k][i];

        /* Internal witness */
        size_t internal_size = n - 2*ss - ts;
        for (size_t i = 0; i < internal_size; ++i)
            full_assignment[base_offset + ts + ss + i] = step_witnesses[k-1][i];
    }

    size_t num_primary = ss + ts;
    r1cs_primary_input<FieldT> primary(full_assignment.begin(),
                                        full_assignment.begin() + num_primary);
    r1cs_auxiliary_input<FieldT> auxiliary(full_assignment.begin() + num_primary,
                                           full_assignment.end());

    return std::make_pair(std::move(primary), std::move(auxiliary));
}


/**
 * UVC.Setup (Generator)
 *
 * Generates CRS for up to B sequential compositions of the base circuit.
 *
 * CRITICAL: Uses a SINGLE QAP from C_B for ALL CRS elements.
 * This ensures polynomial evaluations share the same domain and
 * vanishing polynomial, which is required for the incremental
 * proof update mechanism.
 */
template <typename ppT>
r1cs_uvc_ppzksnark_keypair<ppT> r1cs_uvc_ppzksnark_generator(
    const state_transition_circuit<libff::Fr<ppT> > &st_circuit,
    size_t B)
{
    typedef libff::Fr<ppT> FieldT;
    libff::enter_block("Call to r1cs_uvc_ppzksnark_generator");

    assert(B >= 1);

    const size_t ss = st_circuit.state_size;
    const size_t ts = st_circuit.transition_size;
    const size_t n = st_circuit.base_cs.num_variables();
    const size_t new_per_step = n - ss;

    libff::print_indent(); printf("* UVC Setup: B=%zu, n=%zu, state=%zu, transition=%zu\n", B, n, ss, ts);

    /* ===== Build C_B and generate ONE QAP ===== */
    r1cs_constraint_system<FieldT> cs_B = build_composed_constraint_system(st_circuit, B);

    /* Generate secret randomness */
    const FieldT t = FieldT::random_element();
    const FieldT alpha = FieldT::random_element();
    const FieldT beta = FieldT::random_element();
    const FieldT gamma = FieldT::random_element();
    const FieldT delta = FieldT::random_element();
    const FieldT gamma_inverse = gamma.inverse();
    const FieldT delta_inverse = delta.inverse();

    /* Single QAP from C_B */
    qap_instance_evaluation<FieldT> qap_B = r1cs_to_qap_instance_map_with_evaluation(cs_B, t);

    libff::print_indent(); printf("* QAP (C_B) variables: %zu, degree: %zu, inputs: %zu\n",
        qap_B.num_variables(), qap_B.degree(), qap_B.num_inputs());

    std::vector<FieldT> At = std::move(qap_B.At);
    std::vector<FieldT> Bt = std::move(qap_B.Bt);
    std::vector<FieldT> Ct = std::move(qap_B.Ct);
    std::vector<FieldT> Ht = std::move(qap_B.Ht);
    const FieldT Zt = qap_B.Zt;

#ifdef MULTICORE
    const size_t chunks = omp_get_max_threads();
#else
    const size_t chunks = 1;
#endif

    /* Compute MSM window tables */
    size_t non_zero_At = 0, non_zero_Bt = 0;
    for (size_t i = 0; i < qap_B.num_variables() + 1; ++i) {
        if (!At[i].is_zero()) ++non_zero_At;
        if (!Bt[i].is_zero()) ++non_zero_Bt;
    }

    const libff::G1<ppT> g1_gen = libff::G1<ppT>::random_element();
    const size_t g1_scalar_size = FieldT::size_in_bits();
    const size_t g1_scalar_count = non_zero_At + non_zero_Bt + qap_B.num_variables();
    const size_t g1_window_size = libff::get_exp_window_size<libff::G1<ppT> >(g1_scalar_count);
    libff::window_table<libff::G1<ppT> > g1_table = libff::get_window_table(g1_scalar_size, g1_window_size, g1_gen);

    const libff::G2<ppT> g2_gen = libff::G2<ppT>::random_element();
    const size_t g2_scalar_size = FieldT::size_in_bits();
    const size_t g2_scalar_count = non_zero_Bt;
    const size_t g2_window_size = libff::get_exp_window_size<libff::G2<ppT> >(g2_scalar_count);
    libff::window_table<libff::G2<ppT> > g2_table = libff::get_window_table(g2_scalar_size, g2_window_size, g2_gen);

    /* Encode CRS group elements */
    libff::G1<ppT> alpha_g1 = alpha * g1_gen;
    libff::G1<ppT> beta_g1 = beta * g1_gen;
    libff::G2<ppT> beta_g2 = beta * g2_gen;
    libff::G2<ppT> delta_g2 = delta * g2_gen;
    libff::G2<ppT> gamma_g2 = gamma * g2_gen;

    /* ===== Full A query from C_B: [u_i(x)]_1 for ALL wires ===== */
    libff::enter_block("Encode full A query from C_B");
    libff::G1_vector<ppT> A_query = batch_exp(g1_scalar_size, g1_window_size, g1_table, At);
#ifdef USE_MIXED_ADDITION
    libff::batch_to_special<libff::G1<ppT> >(A_query);
#endif
    libff::leave_block("Encode full A query from C_B");

    /* ===== Full B query from C_B: [v_i(x)]_2 for ALL wires ===== */
    libff::enter_block("Encode full B query from C_B");
    knowledge_commitment_vector<libff::G2<ppT>, libff::G1<ppT> > B_query =
        kc_batch_exp(FieldT::size_in_bits(), g2_window_size, g1_window_size, g2_table, g1_table,
                     FieldT::one(), FieldT::one(), Bt, chunks);
    libff::leave_block("Encode full B query from C_B");

    /* ===== H query using C_B's domain: [x^i * Z_B(x) / delta]_1 ===== */
    libff::enter_block("Encode H query from C_B domain");
    Ht.resize(Ht.size() - 2);
    libff::G1_vector<ppT> H_query = batch_exp_with_coeff(g1_scalar_size, g1_window_size, g1_table,
                                                          Zt * delta_inverse, Ht);
#ifdef USE_MIXED_ADDITION
    libff::batch_to_special<libff::G1<ppT> >(H_query);
#endif
    libff::leave_block("Encode H query from C_B domain");

    /* ===== L query from C_B: [(beta*u_i + alpha*v_i + w_i)/delta]_1 for witness wires ===== */
    libff::enter_block("Encode L query from C_B");
    const size_t num_inputs_B = qap_B.num_inputs();
    const size_t Lt_offset = num_inputs_B + 1;
    std::vector<FieldT> Lt;
    Lt.reserve(qap_B.num_variables() - num_inputs_B);
    for (size_t i = Lt_offset; i < qap_B.num_variables() + 1; ++i) {
        Lt.emplace_back((beta * At[i] + alpha * Bt[i] + Ct[i]) * delta_inverse);
    }
    libff::G1_vector<ppT> L_query = batch_exp(g1_scalar_size, g1_window_size, g1_table, Lt);
#ifdef USE_MIXED_ADDITION
    libff::batch_to_special<libff::G1<ppT> >(L_query);
#endif
    libff::leave_block("Encode L query from C_B");

    /* ===== gamma_ABC: [(beta*u_i + alpha*v_i + w_i)/gamma]_1 for public wires ===== */
    libff::enter_block("Encode gamma_ABC for verification");
    const FieldT gamma_ABC_0 = (beta * At[0] + alpha * Bt[0] + Ct[0]) * gamma_inverse;
    std::vector<FieldT> gamma_ABC_vals;
    gamma_ABC_vals.reserve(num_inputs_B);
    for (size_t i = 1; i <= num_inputs_B; ++i) {
        gamma_ABC_vals.emplace_back((beta * At[i] + alpha * Bt[i] + Ct[i]) * gamma_inverse);
    }
    libff::G1<ppT> gamma_ABC_g1_0 = gamma_ABC_0 * g1_gen;
    libff::G1_vector<ppT> gamma_ABC_g1_vals = batch_exp(g1_scalar_size, g1_window_size, g1_table, gamma_ABC_vals);
    accumulation_vector<libff::G1<ppT> > gamma_ABC_g1(std::move(gamma_ABC_g1_0), std::move(gamma_ABC_g1_vals));
    libff::leave_block("Encode gamma_ABC for verification");

    /* ===== Per-step incremental data for steps 2..B ===== */
    libff::enter_block("Build per-step incremental proving data");
    std::vector<uvc_step_proving_data<ppT> > step_data;

    for (size_t step = 2; step <= B; ++step)
    {
        /* New wire indices in C_B's QAP:
           Previous step total: n + (step-2)*new_per_step
           Current step total: n + (step-1)*new_per_step
           New wires: prev_total+1 .. curr_total (1-indexed)
           In QAP arrays (0=constant wire): prev_total+1 .. curr_total */
        size_t prev_total = n + (step - 2) * new_per_step;
        size_t new_start_qap = prev_total + 1;
        size_t new_count = new_per_step;

        uvc_step_proving_data<ppT> sd;
        sd.new_wire_start = new_start_qap;
        sd.new_wire_count = new_count;
        sd.new_io_count = 0;       /* all new wires are witness (public inputs are fixed) */
        sd.new_wt_count = new_count;

        /* Extract A, B, L queries for new wires from the FULL C_B queries */
        std::vector<FieldT> new_At(At.begin() + new_start_qap, At.begin() + new_start_qap + new_count);
        std::vector<FieldT> new_Bt(Bt.begin() + new_start_qap, Bt.begin() + new_start_qap + new_count);

        std::vector<FieldT> new_Lt;
        new_Lt.reserve(new_count);
        for (size_t i = 0; i < new_count; ++i) {
            size_t qi = new_start_qap + i;
            new_Lt.emplace_back((beta * At[qi] + alpha * Bt[qi] + Ct[qi]) * delta_inverse);
        }

        sd.A_query_delta = batch_exp(g1_scalar_size, g1_window_size, g1_table, new_At);
#ifdef USE_MIXED_ADDITION
        libff::batch_to_special<libff::G1<ppT> >(sd.A_query_delta);
#endif

        sd.B_query_delta = kc_batch_exp(FieldT::size_in_bits(), g2_window_size, g1_window_size,
                                         g2_table, g1_table, FieldT::one(), FieldT::one(), new_Bt, chunks);

        sd.L_query_delta = batch_exp(g1_scalar_size, g1_window_size, g1_table, new_Lt);
#ifdef USE_MIXED_ADDITION
        libff::batch_to_special<libff::G1<ppT> >(sd.L_query_delta);
#endif

        step_data.push_back(std::move(sd));
    }
    libff::leave_block("Build per-step incremental proving data");

    /* ===== Assemble keys ===== */
    libff::GT<ppT> alpha_g1_beta_g2 = ppT::reduced_pairing(alpha_g1, beta_g2);

    /* Build base_pk with C_B's full queries */
    r1cs_vc_ppzksnark_proving_key<ppT> base_pk(
        std::move(alpha_g1), std::move(beta_g1), std::move(beta_g2), libff::G2<ppT>(delta_g2),
        std::move(A_query), std::move(B_query), std::move(H_query), std::move(L_query),
        std::move(cs_B));

    r1cs_uvc_ppzksnark_proving_key<ppT> pk;
    pk.base_pk = std::move(base_pk);
    pk.step_data = std::move(step_data);
    pk.H_query_full = pk.base_pk.H_query; /* alias — same H query */
    pk.max_compositions = B;
    pk.st_circuit = st_circuit;

    r1cs_uvc_ppzksnark_verification_key<ppT> vk;
    vk.alpha_g1_beta_g2 = alpha_g1_beta_g2;
    vk.gamma_g2 = gamma_g2;
    vk.delta_g2 = delta_g2;
    vk.gamma_ABC_g1 = std::move(gamma_ABC_g1);
    vk.max_compositions = B;

    pk.print_size();
    vk.print_size();

    libff::leave_block("Call to r1cs_uvc_ppzksnark_generator");

    return r1cs_uvc_ppzksnark_keypair<ppT>(std::move(pk), std::move(vk));
}


/**
 * UVC.Prove
 *
 * For step j=1: generates a fresh proof using C_B's QAP.
 * For step j>1: incrementally updates A, B, C from the previous proof.
 *
 * Both cases compute h_j by padding the I_j assignment with zeros
 * to fill C_B's full wire set, then using r1cs_to_qap_witness_map
 * on C_B. This works because for well-structured circuits (pure
 * multiplicative gates), future constraints evaluate to 0*0=0
 * with zero-padded new wires.
 */
template <typename ppT>
r1cs_uvc_ppzksnark_proof<ppT> r1cs_uvc_ppzksnark_prover(
    const r1cs_uvc_ppzksnark_proving_key<ppT> &pk,
    size_t step,
    const r1cs_uvc_ppzksnark_primary_input<ppT> &primary_input,
    const r1cs_uvc_ppzksnark_auxiliary_input<ppT> &auxiliary_input,
    const r1cs_uvc_ppzksnark_proof<ppT> *prev_proof)
{
    typedef libff::Fr<ppT> FieldT;
    libff::enter_block("Call to r1cs_uvc_ppzksnark_prover");
    libff::print_indent(); printf("* UVC Prove step %zu\n", step);

    assert(step >= 1 && step <= pk.max_compositions);

#ifdef MULTICORE
    const size_t chunks = omp_get_max_threads();
#else
    const size_t chunks = 1;
#endif

    const r1cs_constraint_system<FieldT> &cs_B = pk.base_pk.constraint_system;
    const size_t ss = pk.st_circuit.state_size;
    const size_t ts = pk.st_circuit.transition_size;
    const size_t n = pk.st_circuit.base_cs.num_variables();
    const size_t new_per_step = n - ss;
    const size_t total_vars_B = cs_B.num_variables();
    const size_t num_inputs_B = cs_B.num_inputs(); /* = ss + ts */

    /* Total wires at step j (excluding constant wire) */
    const size_t total_vars_j = n + (step - 1) * new_per_step;

    /* ===== Pad assignment to C_B size ===== */
    libff::enter_block("Pad assignment to C_B", false);

    /* primary_input has size num_inputs_B = ss + ts (same for all steps) */
    assert(primary_input.size() == num_inputs_B);
    /* auxiliary_input has size total_vars_j - num_inputs_B */
    assert(auxiliary_input.size() == total_vars_j - num_inputs_B);

    /* Pad auxiliary with zeros for wires in I_B \ I_j */
    r1cs_auxiliary_input<FieldT> padded_auxiliary = auxiliary_input;
    size_t pad_size = total_vars_B - total_vars_j;
    padded_auxiliary.resize(padded_auxiliary.size() + pad_size, FieldT::zero());
    assert(padded_auxiliary.size() == total_vars_B - num_inputs_B);

    libff::leave_block("Pad assignment to C_B", false);

    /* ===== Compute h_j using C_B's QAP ===== */
    libff::enter_block("Compute h_j via C_B QAP witness map", false);

    const qap_witness<FieldT> qap_wit = r1cs_to_qap_witness_map(
        cs_B, primary_input, padded_auxiliary,
        FieldT::zero(), FieldT::zero(), FieldT::zero());

    /* Extract h coefficients (degree <= d_B - 2, so d_B - 1 coefficients) */
    size_t h_len = qap_wit.degree() - 1;
    std::vector<FieldT> h_coeffs(qap_wit.coefficients_for_H.begin(),
                                  qap_wit.coefficients_for_H.begin() + h_len);

    libff::leave_block("Compute h_j via C_B QAP witness map", false);

    /* Build full assignment vector (with constant wire 0 = 1) for multi-exp indexing */
    std::vector<FieldT> full_assignment;
    full_assignment.reserve(1 + total_vars_j);
    full_assignment.emplace_back(FieldT::one()); /* wire 0 = constant 1 */
    full_assignment.insert(full_assignment.end(), primary_input.begin(), primary_input.end());
    full_assignment.insert(full_assignment.end(), auxiliary_input.begin(), auxiliary_input.end());

    if (step == 1)
    {
        /* ===== Base case: compute proof from scratch using C_B's queries ===== */
        libff::enter_block("UVC base case: fresh proof for step 1");

        /* A = alpha + sum_{i in I_1} a_i * [u_i(x)]_1 */
        libff::G1<ppT> g1_A = pk.base_pk.alpha_g1 +
            libff::multi_exp_with_mixed_addition<libff::G1<ppT>, FieldT,
                                                  libff::multi_exp_method_BDLO12>(
                pk.base_pk.A_query.begin(),
                pk.base_pk.A_query.begin() + total_vars_j + 1,
                full_assignment.begin(),
                full_assignment.begin() + total_vars_j + 1,
                chunks);

        /* B = beta + sum_{i in I_1} a_i * [v_i(x)]_2 */
        knowledge_commitment<libff::G2<ppT>, libff::G1<ppT> > Bt_acc =
            kc_multi_exp_with_mixed_addition<libff::G2<ppT>, libff::G1<ppT>, FieldT,
                                              libff::multi_exp_method_BDLO12>(
                pk.base_pk.B_query, 0, total_vars_j + 1,
                full_assignment.begin(),
                full_assignment.begin() + total_vars_j + 1,
                chunks);
        libff::G2<ppT> g2_B = pk.base_pk.beta_g2 + Bt_acc.g;

        /* C = sum_{i in I_1^wt} a_i * L_query[i'] + h_1 * H_query
           Witness wires are indices num_inputs_B+1 .. total_vars_j in C_B's numbering.
           In L_query, these are at positions 0 .. (total_vars_j - num_inputs_B - 1).
           In full_assignment, witness starts at index num_inputs_B + 1. */
        size_t num_wt_j = total_vars_j - num_inputs_B;
        libff::G1<ppT> g1_C = libff::multi_exp_with_mixed_addition<libff::G1<ppT>, FieldT,
                                                                     libff::multi_exp_method_BDLO12>(
            pk.base_pk.L_query.begin(),
            pk.base_pk.L_query.begin() + num_wt_j,
            full_assignment.begin() + num_inputs_B + 1,
            full_assignment.begin() + num_inputs_B + 1 + num_wt_j,
            chunks);

        /* Add h contribution */
        g1_C = g1_C + libff::multi_exp<libff::G1<ppT>, FieldT, libff::multi_exp_method_BDLO12>(
            pk.base_pk.H_query.begin(),
            pk.base_pk.H_query.begin() + h_len,
            h_coeffs.begin(),
            h_coeffs.begin() + h_len,
            chunks);

        libff::leave_block("UVC base case: fresh proof for step 1");

        r1cs_uvc_ppzksnark_proof<ppT> proof(
            std::move(g1_A), std::move(g2_B), std::move(g1_C),
            step, std::move(h_coeffs));

        proof.print_size();
        libff::leave_block("Call to r1cs_uvc_ppzksnark_prover");
        return proof;
    }

    /* ===== Incremental case: step > 1 ===== */
    libff::enter_block("UVC incremental update");
    assert(prev_proof != nullptr);
    assert(prev_proof->step == step - 1);

    const uvc_step_proving_data<ppT> &sd = pk.step_data[step - 2]; /* 0-indexed */

    /* Extract new wire values from full_assignment */
    std::vector<FieldT> new_wire_values;
    new_wire_values.reserve(sd.new_wire_count);
    for (size_t i = 0; i < sd.new_wire_count; ++i) {
        new_wire_values.push_back(full_assignment[sd.new_wire_start + i]);
    }

    /* A_j = A_{j-1} + sum_{new wires} a_i * [u_i(x)]_1 */
    libff::enter_block("Incremental A update", false);
    libff::G1<ppT> delta_A = libff::multi_exp_with_mixed_addition<libff::G1<ppT>,
                                                                   FieldT,
                                                                   libff::multi_exp_method_BDLO12>(
        sd.A_query_delta.begin(), sd.A_query_delta.end(),
        new_wire_values.begin(), new_wire_values.end(),
        chunks);
    libff::G1<ppT> g1_A = prev_proof->g_A + delta_A;
    libff::leave_block("Incremental A update", false);

    /* B_j = B_{j-1} + sum_{new wires} a_i * [v_i(x)]_2 */
    libff::enter_block("Incremental B update", false);
    knowledge_commitment<libff::G2<ppT>, libff::G1<ppT> > delta_Bt =
        kc_multi_exp_with_mixed_addition<libff::G2<ppT>, libff::G1<ppT>, FieldT,
                                          libff::multi_exp_method_BDLO12>(
            sd.B_query_delta, 0, sd.new_wire_count,
            new_wire_values.begin(), new_wire_values.end(),
            chunks);
    libff::G2<ppT> g2_B = prev_proof->g_B + delta_Bt.g;
    libff::leave_block("Incremental B update", false);

    /* C_j = C_{j-1} + sum_{new witness wires} a_i * L_delta[i]
                      + (h_j - h_{j-1}) * H_query */
    libff::enter_block("Incremental C update", false);

    /* L contribution from new witness wires */
    libff::G1<ppT> delta_L = libff::multi_exp_with_mixed_addition<libff::G1<ppT>,
                                                                   FieldT,
                                                                   libff::multi_exp_method_BDLO12>(
        sd.L_query_delta.begin(), sd.L_query_delta.end(),
        new_wire_values.begin(), new_wire_values.end(),
        chunks);

    /* H contribution: (h_j - h_{j-1}) coefficients */
    size_t max_h_len = std::max(h_coeffs.size(), prev_proof->cached_h_coefficients.size());
    std::vector<FieldT> h_diff(max_h_len, FieldT::zero());
    for (size_t i = 0; i < h_coeffs.size(); ++i) {
        h_diff[i] += h_coeffs[i];
    }
    for (size_t i = 0; i < prev_proof->cached_h_coefficients.size(); ++i) {
        h_diff[i] -= prev_proof->cached_h_coefficients[i];
    }

    /* Trim trailing zeros */
    while (!h_diff.empty() && h_diff.back().is_zero()) {
        h_diff.pop_back();
    }

    libff::G1<ppT> delta_H = libff::G1<ppT>::zero();
    if (!h_diff.empty()) {
        size_t h_query_len = std::min(h_diff.size(), pk.base_pk.H_query.size());
        delta_H = libff::multi_exp<libff::G1<ppT>, FieldT, libff::multi_exp_method_BDLO12>(
            pk.base_pk.H_query.begin(),
            pk.base_pk.H_query.begin() + h_query_len,
            h_diff.begin(),
            h_diff.begin() + h_query_len,
            chunks);
    }

    libff::G1<ppT> g1_C = prev_proof->g_C + delta_L + delta_H;

    libff::leave_block("Incremental C update", false);

    libff::leave_block("UVC incremental update");

    r1cs_uvc_ppzksnark_proof<ppT> proof(
        std::move(g1_A), std::move(g2_B), std::move(g1_C),
        step, std::move(h_coeffs));

    proof.print_size();
    libff::leave_block("Call to r1cs_uvc_ppzksnark_prover");
    return proof;
}


/**
 * UVC.Verify
 *
 * Verifies a proof at step j using the VC verification equation:
 *   e(A, B) = e(alpha, beta) * e(acc, gamma) * e(C, delta)
 *
 * Uses a single gamma_ABC that encodes s_0 and t_1 (fixed public inputs).
 */
template <typename ppT>
bool r1cs_uvc_ppzksnark_verifier(
    const r1cs_uvc_ppzksnark_verification_key<ppT> &vk,
    size_t step,
    const r1cs_uvc_ppzksnark_primary_input<ppT> &primary_input,
    const r1cs_uvc_ppzksnark_proof<ppT> &proof)
{
    libff::enter_block("Call to r1cs_uvc_ppzksnark_verifier");
    assert(step >= 1 && step <= vk.max_compositions);

    /* Build a VC verification key using the single gamma_ABC */
    r1cs_vc_ppzksnark_verification_key<ppT> vc_vk(
        vk.alpha_g1_beta_g2, vk.gamma_g2, vk.delta_g2, vk.gamma_ABC_g1);

    /* Build a VC proof from the UVC proof */
    r1cs_vc_ppzksnark_proof<ppT> vc_proof;
    vc_proof.g_A = proof.g_A;
    vc_proof.g_B = proof.g_B;
    vc_proof.g_C = proof.g_C;

    bool result = r1cs_vc_ppzksnark_verifier_weak_IC<ppT>(vc_vk, primary_input, vc_proof);

    libff::print_indent(); printf("* UVC Verify step %zu: %s\n", step, result ? "PASS" : "FAIL");
    libff::leave_block("Call to r1cs_uvc_ppzksnark_verifier");

    return result;
}


} // libsnark
#endif // R1CS_UVC_PPZKSNARK_TCC_
