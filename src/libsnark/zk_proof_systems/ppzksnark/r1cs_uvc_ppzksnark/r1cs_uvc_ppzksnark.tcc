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
#include <stdexcept>

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

    const std::vector<size_t> state_output_indices = uvc_state_output_indices(st_circuit, B);
    /* Precomputed once: per-wire class table (uvc_wire_class_of would rebuild
       the I^st index vector on every call inside the loops below). */
    const std::vector<uvc_wire_class> wire_classes = uvc_wire_classes(st_circuit, B);
    /* ===== L query from C_B: [(beta*u_i + alpha*v_i + w_i)/delta]_1 for witness wires ===== */
    libff::enter_block("Encode L query from C_B");
    const size_t num_inputs_B = qap_B.num_inputs();
    const size_t Lt_offset = num_inputs_B + 1;
    std::vector<FieldT> Lt;
    Lt.reserve(qap_B.num_variables() - num_inputs_B);
    for (size_t i = Lt_offset; i < qap_B.num_variables() + 1; ++i) {
        Lt.emplace_back(wire_classes[i] == uvc_wire_class::st
                        ? FieldT::zero()
                        : (beta * At[i] + alpha * Bt[i] + Ct[i]) * delta_inverse);
    }
    libff::G1_vector<ppT> L_query = batch_exp(g1_scalar_size, g1_window_size, g1_table, Lt);
#ifdef USE_MIXED_ADDITION
    libff::batch_to_special<libff::G1<ppT> >(L_query);
#endif
    libff::leave_block("Encode L query from C_B");
    /* ===== State-output query: [(beta*u_i + alpha*v_i + w_i)/gamma]_1 for I^st =====
       State wires live on the gamma track (never delta): io u st = gamma, wt = delta. */
    libff::enter_block("Encode state-output query from C_B");
    std::vector<FieldT> st_scalars;
    st_scalars.reserve(state_output_indices.size());
    for (const size_t i : state_output_indices) {
        st_scalars.emplace_back((beta * At[i] + alpha * Bt[i] + Ct[i]) * gamma_inverse);
    }
    libff::G1_vector<ppT> st_query = batch_exp(g1_scalar_size, g1_window_size, g1_table, st_scalars);
#ifdef USE_MIXED_ADDITION
    libff::batch_to_special<libff::G1<ppT> >(st_query);
#endif
    libff::leave_block("Encode state-output query from C_B");

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
    /* ===== Always-on partition invariant (AC1): io u st = gamma, wt = delta =====
       Enforced in release builds: a violated partition breaks knowledge
       soundness (public/witness collision), so Setup fails hard. */
    {
        bool tracks_are_disjoint =
            uvc_check_track_disjointness(st_circuit, B, qap_B.num_variables()) &&
            state_output_indices.size() == B * ss &&
            st_query.size() == state_output_indices.size();
        for (size_t i = 1; i <= qap_B.num_variables() && tracks_are_disjoint; ++i) {
            const uvc_wire_class wire_class = wire_classes[i];
            if (wire_class == uvc_wire_class::io) {
                tracks_are_disjoint = (i <= num_inputs_B);
            } else if (wire_class == uvc_wire_class::st) {
                tracks_are_disjoint = (i > num_inputs_B &&
                                       L_query[i - Lt_offset].is_zero());
            } else {
                tracks_are_disjoint = (i > num_inputs_B);
            }
        }
        if (!tracks_are_disjoint) {
            throw std::logic_error(
                "uvc: wire-track partition invariant violated "
                "(io u st must be on gamma, wt on delta, tracks disjoint)");
        }
    }

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
        sd.new_st_count = ss;
        sd.new_wt_count = new_count - ss;

        /* Extract A, B, L queries for new wires from the FULL C_B queries */
        std::vector<FieldT> new_At(At.begin() + new_start_qap, At.begin() + new_start_qap + new_count);
        std::vector<FieldT> new_Bt(Bt.begin() + new_start_qap, Bt.begin() + new_start_qap + new_count);

        std::vector<FieldT> new_Lt;
        new_Lt.reserve(new_count);
        for (size_t i = 0; i < new_count; ++i) {
            const size_t qi = new_start_qap + i;
            new_Lt.emplace_back(wire_classes[qi] == uvc_wire_class::st
                                ? FieldT::zero()
                                : (beta * At[qi] + alpha * Bt[qi] + Ct[qi]) * delta_inverse);
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
        {
            const size_t st_query_offset = (step - 1) * ss;
            sd.st_query_delta.insert(sd.st_query_delta.end(),
                                     st_query.begin() + st_query_offset,
                                     st_query.begin() + st_query_offset + ss);
#ifdef USE_MIXED_ADDITION
            libff::batch_to_special<libff::G1<ppT> >(sd.st_query_delta);
#endif
        }

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
    pk.st_query = st_query;
    pk.max_compositions = B;
    pk.st_circuit = st_circuit;

    r1cs_uvc_ppzksnark_verification_key<ppT> vk;
    vk.alpha_g1_beta_g2 = alpha_g1_beta_g2;
    vk.gamma_g2 = gamma_g2;
    vk.delta_g2 = delta_g2;
    vk.gamma_ABC_g1 = std::move(gamma_ABC_g1);
    vk.st_ABC_g1 = st_query;
    vk.max_compositions = B;
    vk.state_size = ss;
    vk.transition_size = ts;
    vk.new_per_step = new_per_step;

    pk.print_size();
    vk.print_size();

    libff::leave_block("Call to r1cs_uvc_ppzksnark_generator");

    return r1cs_uvc_ppzksnark_keypair<ppT>(std::move(pk), std::move(vk));
}


/**
 * Internal per-step fold view (single fold path for all steps, AC3).
 *
 * Holds iterator ranges over the CRS elements consumed by step j's fold
 * together with the full_assignment offsets of the matching scalar spans.
 * Step 1 VIEWS the base C_B queries over {0} u I_1 (no CRS element is
 * duplicated for the base case); steps j>1 view step_data[step-2].
 * Selecting the view is the only step-dependent code; the four fold
 * multi-exponentiations below execute one shared path.
 */
template<typename ppT>
struct uvc_step_fold_view {
    typename libff::G1_vector<ppT>::const_iterator A_begin, A_end;
    const knowledge_commitment_vector<libff::G2<ppT>, libff::G1<ppT> > *B_vec;
    size_t B_lo, B_hi;
    typename libff::G1_vector<ppT>::const_iterator L_begin, L_end;
    typename libff::G1_vector<ppT>::const_iterator st_begin, st_end;
    size_t AB_scalar_offset;  /* full_assignment index of the A/B span */
    size_t L_scalar_offset;   /* full_assignment index of the L span */
    size_t st_scalar_offset;  /* full_assignment index of the st span */
};

/**
 * UVC.Prove
 *
 * Every step is the same fold. For j=1 the previous accumulator is the
 * base case (alpha, beta, 0, 0) with I_0 = {} and h_0 = 0, and the fold
 * ranges over {0} u I_1 (wire 0 contributes with a_0 = 1). For j>1 the
 * previous accumulator is prev_proof and the fold ranges over I_j \ I_{j-1}.
 *
 * Both cases compute h_j by padding the I_j assignment with zeros
 * to fill C_B's full wire set, then using r1cs_to_qap_witness_map
 * on C_B. Zero-extension admissibility (the padded assignment must
 * satisfy C_B) is enforced by a runtime preflight below; violating
 * circuits are rejected with std::invalid_argument in release builds.
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

    /* ===== Zero-extension admissibility preflight (AC9) =====
       Runtime-enforced (effective in release builds, unlike the debug-only
       assert inside r1cs_to_qap_witness_map): the zero-padded C_j
       assignment must satisfy C_B, otherwise h_j computed on C_B's domain
       is invalid for this circuit. */
    if (!cs_B.is_satisfied(primary_input, padded_auxiliary)) {
        libff::leave_block("Call to r1cs_uvc_ppzksnark_prover");
        throw std::invalid_argument(
            "uvc: circuit violates zero-extension admissibility "
            "(zero-padded C_j assignment does not satisfy C_B)");
    }

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
    /* ===== Uniform fold: every step updates the previous accumulator (AC3) =====
       Base case j=1: (A_0, B_0, C_0, D_0) = (alpha, beta, 0, 0), I_0 = {},
       h_0 = 0. The fold ranges over {0} u I_1 as a VIEW of C_B's base
       queries (wire 0 contributes with a_0 = 1; no CRS element is
       duplicated for step 1). Step j>1 folds over I_j \ I_{j-1} using
       step_data[step-2] and the previous proof. */
    libff::enter_block("UVC uniform fold");

    libff::G1<ppT> prev_A;
    libff::G2<ppT> prev_B;
    libff::G1<ppT> prev_C;
    libff::G1<ppT> prev_D;
    const std::vector<FieldT> empty_h;
    const std::vector<FieldT> *prev_h = &empty_h;

    /* Step-dependent code selects only the previous accumulator and the
       fold view; the multi-exponentiations below are one shared path. */
    uvc_step_fold_view<ppT> view;
    if (step == 1)
    {
        prev_A = pk.base_pk.alpha_g1;
        prev_B = pk.base_pk.beta_g2;
        prev_C = libff::G1<ppT>::zero();
        prev_D = libff::G1<ppT>::zero();

        /* A/B span: {0} u I_1 with a_0 = 1 (length 1 + |I_1|). */
        assert(full_assignment.size() == 1 + total_vars_j);
        view.A_begin = pk.base_pk.A_query.begin();
        view.A_end   = pk.base_pk.A_query.begin() + total_vars_j + 1;
        view.B_vec   = &pk.base_pk.B_query;
        view.B_lo    = 0;
        view.B_hi    = total_vars_j + 1;
        view.AB_scalar_offset = 0;

        /* C (delta track) consumes ONLY I_1^wt (plus the h-block below):
           the base L query holds the zero element at every state-output
           position, so s_1 cannot double-count into both C (delta) and
           D (gamma). */
        const size_t num_wt_span_1 = total_vars_j - num_inputs_B;
        assert(pk.base_pk.L_query.size() >= num_wt_span_1);
        view.L_begin = pk.base_pk.L_query.begin();
        view.L_end   = pk.base_pk.L_query.begin() + num_wt_span_1;
        view.L_scalar_offset = num_inputs_B + 1;

        /* D (gamma track) consumes ONLY I_1^st via the base st_query slice
           (a view over already-published CRS elements; nothing duplicated). */
        assert(pk.st_query.size() >= ss);
        view.st_begin = pk.st_query.begin();
        view.st_end   = pk.st_query.begin() + ss;
        view.st_scalar_offset = ss + ts + 1;
    }
    else
    {
        assert(prev_proof != nullptr);
        assert(prev_proof->step == step - 1);

        const uvc_step_proving_data<ppT> &sd = pk.step_data[step - 2]; /* 0-indexed */
        prev_A = prev_proof->g_A;
        prev_B = prev_proof->g_B;
        prev_C = prev_proof->g_C;
        prev_D = prev_proof->g_D;
        prev_h = &prev_proof->cached_h_coefficients;

        view.A_begin = sd.A_query_delta.begin();
        view.A_end   = sd.A_query_delta.end();
        view.B_vec   = &sd.B_query_delta;
        view.B_lo    = 0;
        view.B_hi    = sd.new_wire_count;
        view.AB_scalar_offset = sd.new_wire_start;

        /* C (delta track): state-output positions in L_query_delta hold the
           zero element, so only I_j^wt contributes. */
        view.L_begin = sd.L_query_delta.begin();
        view.L_end   = sd.L_query_delta.end();
        view.L_scalar_offset = sd.new_wire_start;

        /* D_j = D_{j-1} + sum_{new st} a_i * [L_i/gamma]_1.
           new_wire_start + ts = (step-1)*new_per_step + ss + ts + 1,
           so that offset is exactly step j's state-output slice. */
        assert(sd.st_query_delta.size() == ss);
        assert(sd.new_wire_start + ts == (step - 1) * new_per_step + ss + ts + 1);
        view.st_begin = sd.st_query_delta.begin();
        view.st_end   = sd.st_query_delta.end();
        view.st_scalar_offset = sd.new_wire_start + ts;
    }

    /* ===== One fold kernel for every step j = 1..B ===== */
    const size_t AB_len = static_cast<size_t>(view.A_end - view.A_begin);
    const size_t L_len = static_cast<size_t>(view.L_end - view.L_begin);
    const size_t st_len = static_cast<size_t>(view.st_end - view.st_begin);
    assert(view.B_hi - view.B_lo == AB_len);
    assert(st_len == ss);
    assert(view.AB_scalar_offset + AB_len <= full_assignment.size());
    assert(view.L_scalar_offset + L_len <= full_assignment.size());
    assert(view.st_scalar_offset + st_len <= full_assignment.size());

    libff::G1<ppT> delta_A = libff::multi_exp_with_mixed_addition<libff::G1<ppT>, FieldT,
                                                                   libff::multi_exp_method_BDLO12>(
        view.A_begin, view.A_end,
        full_assignment.begin() + view.AB_scalar_offset,
        full_assignment.begin() + view.AB_scalar_offset + AB_len,
        chunks);

    knowledge_commitment<libff::G2<ppT>, libff::G1<ppT> > delta_B =
        kc_multi_exp_with_mixed_addition<libff::G2<ppT>, libff::G1<ppT>, FieldT,
                                          libff::multi_exp_method_BDLO12>(
            *view.B_vec, view.B_lo, view.B_hi,
            full_assignment.begin() + view.AB_scalar_offset,
            full_assignment.begin() + view.AB_scalar_offset + AB_len,
            chunks);

    libff::G1<ppT> delta_L = libff::multi_exp_with_mixed_addition<libff::G1<ppT>, FieldT,
                                                                   libff::multi_exp_method_BDLO12>(
        view.L_begin, view.L_end,
        full_assignment.begin() + view.L_scalar_offset,
        full_assignment.begin() + view.L_scalar_offset + L_len,
        chunks);

    libff::G1<ppT> delta_st = libff::multi_exp_with_mixed_addition<libff::G1<ppT>, FieldT,
                                                                    libff::multi_exp_method_BDLO12>(
        view.st_begin, view.st_end,
        full_assignment.begin() + view.st_scalar_offset,
        full_assignment.begin() + view.st_scalar_offset + st_len,
        chunks);

    /* Shared h contribution: (h_j - h_{j-1}) coefficients (h_0 = 0). */
    size_t max_h_len = std::max(h_coeffs.size(), prev_h->size());
    std::vector<FieldT> h_diff(max_h_len, FieldT::zero());
    for (size_t i = 0; i < h_coeffs.size(); ++i) {
        h_diff[i] += h_coeffs[i];
    }
    for (size_t i = 0; i < prev_h->size(); ++i) {
        h_diff[i] -= (*prev_h)[i];
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

    libff::G1<ppT> g1_A = prev_A + delta_A;
    libff::G2<ppT> g2_B = prev_B + delta_B.g;
    libff::G1<ppT> g1_C = prev_C + delta_L + delta_H;
    libff::G1<ppT> g1_D = prev_D + delta_st;

    libff::leave_block("UVC uniform fold");

    r1cs_uvc_ppzksnark_proof<ppT> proof(
        std::move(g1_A), std::move(g2_B), std::move(g1_C), std::move(g1_D),
        step, std::move(h_coeffs));

    proof.print_size();
    libff::leave_block("Call to r1cs_uvc_ppzksnark_prover");
    return proof;
}


/**
 * UVC.Verify (single verifier; every proof is state-bound)
 *
 * Verifies a proof at step j against the caller-held trusted previous
 * state commitment D_prev:
 *   genesis guard:   step == 1 requires D_prev == 0 (checked first,
 *                    before the increment MSM)
 *   increment check: [D_j]_1 - [D_{j-1}]_1 = sum_i s_j[i] * [L_i/gamma]_1
 *   main equation:   e(A, B) = e(alpha, beta) * e(Q_j + D_j, gamma) * e(C, delta)
 * with Q_j accumulated from the single gamma_ABC that encodes s_0 and t_1
 * (fixed public inputs) and e(alpha, beta) precomputed in the VK.
 *
 * Online cost: exactly 3 pairings + O(|s_j|) increment MSM.
 */
template <typename ppT>
bool r1cs_uvc_ppzksnark_verifier(
    const r1cs_uvc_ppzksnark_verification_key<ppT> &vk,
    size_t step,
    const r1cs_uvc_ppzksnark_primary_input<ppT> &primary_input,
    const std::vector<libff::Fr<ppT> > &reported_s_j,
    const r1cs_uvc_ppzksnark_proof<ppT> &proof,
    const libff::G1<ppT> &D_prev)
{
    libff::enter_block("Call to r1cs_uvc_ppzksnark_verifier");

    /* Genesis guard (AC4): the protocol precondition D_0 = 0 is enforced
       inside Verify, before the increment MSM — caller ownership of the
       authentic D_prev does not relax the verifier's own check. */
    if (step == 1 && !D_prev.is_zero())
    {
        if (!libff::inhibit_profiling_info) {
            libff::print_indent(); printf("Genesis guard: step 1 requires a zero previous commitment.\n");
        }
        libff::leave_block("Call to r1cs_uvc_ppzksnark_verifier");
        return false;
    }

    if (proof.step != step || step < 1 || step > vk.max_compositions)
    {
        if (!libff::inhibit_profiling_info) {
            libff::print_indent(); printf("Proof step does not match a valid verification step.\n");
        }
        libff::leave_block("Call to r1cs_uvc_ppzksnark_verifier");
        return false;
    }
    if (primary_input.size() != vk.state_size + vk.transition_size)
    {
        if (!libff::inhibit_profiling_info) {
            libff::print_indent(); printf("Primary input has incorrect size.\n");
        }
        libff::leave_block("Call to r1cs_uvc_ppzksnark_verifier");
        return false;
    }
    if (reported_s_j.size() != vk.state_size)
    {
        if (!libff::inhibit_profiling_info) {
            libff::print_indent(); printf("Reported state has incorrect size.\n");
        }
        libff::leave_block("Call to r1cs_uvc_ppzksnark_verifier");
        return false;
    }

    if (!proof.is_well_formed() || !D_prev.is_well_formed())
    {
        if (!libff::inhibit_profiling_info) {
            libff::print_indent(); printf("At least one proof or previous commitment element does not lie on the curve.\n");
        }
        libff::leave_block("Call to r1cs_uvc_ppzksnark_verifier");
        return false;
    }

    libff::G1<ppT> expected_increment = libff::G1<ppT>::zero();
    const size_t state_offset = (step - 1) * vk.state_size;
    for (size_t i = 0; i < vk.state_size; ++i) {
        expected_increment = expected_increment +
            reported_s_j[i] * vk.st_ABC_g1[state_offset + i];
    }
    if (proof.g_D - D_prev != expected_increment)
    {
        if (!libff::inhibit_profiling_info) {
            libff::print_indent(); printf("State commitment increment check failed.\n");
        }
        libff::leave_block("Call to r1cs_uvc_ppzksnark_verifier");
        return false;
    }

    const accumulation_vector<libff::G1<ppT> > accumulated_IC =
        vk.gamma_ABC_g1.template accumulate_chunk<libff::Fr<ppT> >(
            primary_input.begin(), primary_input.end(), 0);
    const libff::G1<ppT> &acc = accumulated_IC.first;

    /* Q_j + D_j share the gamma generator, so they fold into ONE pairing. */
    const libff::G1<ppT> acc_plus_D = acc + proof.g_D;

    const libff::G1_precomp<ppT> proof_g_A_precomp = ppT::precompute_G1(proof.g_A);
    const libff::G2_precomp<ppT> proof_g_B_precomp = ppT::precompute_G2(proof.g_B);
    const libff::G1_precomp<ppT> proof_g_C_precomp = ppT::precompute_G1(proof.g_C);
    const libff::G1_precomp<ppT> acc_plus_D_precomp = ppT::precompute_G1(acc_plus_D);
    const libff::G2_precomp<ppT> gamma_g2_precomp = ppT::precompute_G2(vk.gamma_g2);
    const libff::G2_precomp<ppT> delta_g2_precomp = ppT::precompute_G2(vk.delta_g2);

    /* AC5: exactly three online pairings — A·B, (Q_j+D_j)·gamma, C·delta —
       against the precomputed e(alpha, beta). */
    const libff::Fqk<ppT> QAP1 = ppT::miller_loop(proof_g_A_precomp, proof_g_B_precomp);
    const libff::Fqk<ppT> QAP23 = ppT::double_miller_loop(
        acc_plus_D_precomp, gamma_g2_precomp, proof_g_C_precomp, delta_g2_precomp);
    const libff::GT<ppT> QAP = ppT::final_exponentiation(
        QAP1 * QAP23.unitary_inverse());

    const bool result = (QAP == vk.alpha_g1_beta_g2);
    if (!result && !libff::inhibit_profiling_info) {
        libff::print_indent(); printf("QAP divisibility check failed.\n");
    }

    libff::leave_block("Call to r1cs_uvc_ppzksnark_verifier");
    return result;
}


} // libsnark
#endif // R1CS_UVC_PPZKSNARK_TCC_
