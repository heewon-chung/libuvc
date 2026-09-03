/** @file
*****************************************************************************

Benchmark-local circuit variants for the Groth16 chaining baselines.

Provides:
  - make_public_output_circuit    : base circuit with s_out exposed (single-step)
  - build_single_step_assignment  : assignment for one step of the base circuit
  - build_public_state_permutation: wire permutation exposing reported states
  - permute_constraint_system     : apply a wire permutation to an R1CS
  - permute_assignment            : apply a wire permutation to an assignment
  - build_composed_public_states  : composed C_j with reported states public
  - build_padded_composed_assignment : zero-padded C_j assignment for a fixed C_B CRS

These are benchmark-only helpers; they do not touch the UVC prover/verifier.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef GROTH16_BASELINE_CIRCUITS_HPP_
#define GROTH16_BASELINE_CIRCUITS_HPP_

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include <libsnark/relations/constraint_satisfaction_problems/r1cs/r1cs.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.hpp>

namespace libsnark {

/* ======================================================================== */
/* Single-step Groth16 baseline (work item A)                                */
/* ======================================================================== */

/**
 * Copy of the base circuit whose public input is [s_in, t, s_out].
 * No constraints change; only the primary/auxiliary split moves.
 */
template<typename FieldT>
r1cs_constraint_system<FieldT>
make_public_output_circuit(const state_transition_circuit<FieldT> &st)
{
    const size_t ss = st.state_size;
    const size_t ts = st.transition_size;
    assert(st.base_cs.num_variables() >= 2 * ss + ts);

    r1cs_constraint_system<FieldT> cs = st.base_cs;
    const size_t num_variables = cs.num_variables();
    cs.primary_input_size = 2 * ss + ts;
    cs.auxiliary_input_size = num_variables - cs.primary_input_size;
    return cs;
}

/**
 * Assignment for step j (1-indexed) of the base circuit with public s_out.
 *   primary = states[j-1] ++ transitions[j-1] ++ states[j]
 *   aux     = witnesses[j-1]
 */
template<typename FieldT>
std::pair<r1cs_primary_input<FieldT>, r1cs_auxiliary_input<FieldT> >
build_single_step_assignment(
    const state_transition_circuit<FieldT> &st, size_t j,
    const std::vector<std::vector<FieldT> > &states,
    const std::vector<std::vector<FieldT> > &transitions,
    const std::vector<std::vector<FieldT> > &witnesses)
{
    assert(j >= 1 && j + 1 <= states.size());

    const size_t ss = st.state_size;
    const size_t ts = st.transition_size;

    r1cs_primary_input<FieldT> primary;
    primary.reserve(2 * ss + ts);
    for (size_t i = 0; i < ss; ++i) primary.emplace_back(states[j-1][i]);
    for (size_t i = 0; i < ts; ++i) primary.emplace_back(transitions[j-1][i]);
    for (size_t i = 0; i < ss; ++i) primary.emplace_back(states[j][i]);

    r1cs_auxiliary_input<FieldT> aux(witnesses[j-1].begin(), witnesses[j-1].end());

    return std::make_pair(std::move(primary), std::move(aux));
}


/* ======================================================================== */
/* Public-state permutation of the composed circuit (work items B, B')       */
/* ======================================================================== */

enum class ExposeStates { FinalOnly, All };

/** Number of public inputs of the permuted C_j under the given exposure. */
template<typename FieldT>
size_t num_public_states(const state_transition_circuit<FieldT> &st,
                         size_t j, ExposeStates mode)
{
    const size_t ss = st.state_size;
    const size_t ts = st.transition_size;
    return ss + ts + (mode == ExposeStates::All ? j * ss : ss);
}

/**
 * Wire permutation of C_j moving the reported state wires into the public
 * prefix. Returns perm of length total_vars_j + 1 with perm[0] = 0 and, for
 * old 1-based wire index o, perm[o] = the new 1-based wire index.
 *
 * New order: s_0 | t_1 | exposed states | remaining wires in increasing old index.
 */
template<typename FieldT>
std::vector<size_t> build_public_state_permutation(
    const state_transition_circuit<FieldT> &st, size_t j, ExposeStates mode)
{
    const size_t n = st.base_cs.num_variables();
    const size_t ss = st.state_size;
    const size_t ts = st.transition_size;
    const size_t nps = n - ss;
    const size_t total_vars_j = n + (j - 1) * nps;

    std::vector<size_t> perm(total_vars_j + 1, 0);
    std::vector<char> assigned(total_vars_j + 1, 0);
    size_t next = 1;

    auto assign = [&](size_t old_index) {
        perm[old_index] = next++;
        assigned[old_index] = 1;
    };

    /* 1. s_0 */
    for (size_t o = 1; o <= ss; ++o) assign(o);

    /* 2. t_1 */
    const size_t off1 = ss; /* off(1) = 0 * nps + ss */
    for (size_t i = 1; i <= ts; ++i) assign(off1 + i);

    /* 3. exposed states */
    const size_t first_exposed = (mode == ExposeStates::All ? 1 : j);
    for (size_t k = first_exposed; k <= j; ++k) {
        const size_t offk = (k - 1) * nps + ss;
        for (size_t i = 1; i <= ss; ++i) assign(offk + ts + i);
    }

    /* 4. everything else, in increasing old index */
    for (size_t o = 1; o <= total_vars_j; ++o)
        if (!assigned[o]) assign(o);

    assert(next == total_vars_j + 1);
    return perm;
}

/** Apply a wire permutation to an R1CS and re-split primary/auxiliary. */
template<typename FieldT>
r1cs_constraint_system<FieldT> permute_constraint_system(
    const r1cs_constraint_system<FieldT> &cs, const std::vector<size_t> &perm,
    size_t num_public)
{
    r1cs_constraint_system<FieldT> out = cs;
    for (auto &constraint : out.constraints) {
        for (auto &term : constraint.a.terms) term.index = perm[term.index];
        for (auto &term : constraint.b.terms) term.index = perm[term.index];
        for (auto &term : constraint.c.terms) term.index = perm[term.index];
    }
    const size_t num_variables = cs.num_variables();
    out.primary_input_size = num_public;
    out.auxiliary_input_size = num_variables - num_public;
    return out;
}

/** Apply a wire permutation to an assignment and re-split primary/auxiliary. */
template<typename FieldT>
std::pair<r1cs_primary_input<FieldT>, r1cs_auxiliary_input<FieldT> >
permute_assignment(const r1cs_primary_input<FieldT> &primary_old,
                   const r1cs_auxiliary_input<FieldT> &aux_old,
                   const std::vector<size_t> &perm, size_t num_public)
{
    std::vector<FieldT> full_old;
    full_old.reserve(primary_old.size() + aux_old.size());
    full_old.insert(full_old.end(), primary_old.begin(), primary_old.end());
    full_old.insert(full_old.end(), aux_old.begin(), aux_old.end());

    std::vector<FieldT> full_new(full_old.size(), FieldT::zero());
    for (size_t i = 0; i < full_old.size(); ++i)
        full_new[perm[i + 1] - 1] = full_old[i];

    r1cs_primary_input<FieldT> primary(full_new.begin(),
                                       full_new.begin() + num_public);
    r1cs_auxiliary_input<FieldT> aux(full_new.begin() + num_public,
                                     full_new.end());
    return std::make_pair(std::move(primary), std::move(aux));
}

template<typename FieldT>
struct PublicStateCircuit {
    r1cs_constraint_system<FieldT> cs;
    std::vector<size_t> perm;
    size_t num_public;
    size_t j;
};

/** Composed C_j with the reported states moved into the public input. */
template<typename FieldT>
PublicStateCircuit<FieldT> build_composed_public_states(
    const state_transition_circuit<FieldT> &st, size_t j, ExposeStates mode)
{
    PublicStateCircuit<FieldT> out;
    out.j = j;
    out.num_public = num_public_states(st, j, mode);
    out.perm = build_public_state_permutation(st, j, mode);
    out.cs = permute_constraint_system(
        build_composed_constraint_system(st, j), out.perm, out.num_public);
    return out;
}

/**
 * Assignment for step j padded to C_B's wire count and permuted with C_B's
 * permutation (fixed-CRS baseline).
 */
template<typename FieldT>
std::pair<r1cs_primary_input<FieldT>, r1cs_auxiliary_input<FieldT> >
build_padded_composed_assignment(
    const state_transition_circuit<FieldT> &st,
    size_t j, size_t B, const std::vector<size_t> &perm_B, size_t num_public_B,
    const std::vector<std::vector<FieldT> > &states,
    const std::vector<std::vector<FieldT> > &transitions,
    const std::vector<std::vector<FieldT> > &witnesses)
{
    const size_t n = st.base_cs.num_variables();
    const size_t ss = st.state_size;
    const size_t ts = st.transition_size;
    const size_t total_vars_B = n + (B - 1) * (n - ss);

    std::vector<std::vector<FieldT> > s(states.begin(), states.begin() + j + 1);
    std::vector<std::vector<FieldT> > t(transitions.begin(), transitions.begin() + j);
    std::vector<std::vector<FieldT> > w(witnesses.begin(), witnesses.begin() + j);
    auto assign = build_composed_assignment(st, j, s, t, w);

    assign.second.resize(total_vars_B - (ss + ts), FieldT::zero());

    return permute_assignment(assign.first, assign.second, perm_B, num_public_B);
}

} // libsnark

#endif // GROTH16_BASELINE_CIRCUITS_HPP_
