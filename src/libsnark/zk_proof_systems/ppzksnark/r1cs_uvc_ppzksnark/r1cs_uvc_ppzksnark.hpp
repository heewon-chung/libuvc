/** @file
*****************************************************************************

Declaration of interfaces for the Updatable Verifiable Computation (UVC) scheme.

This scheme enables incremental proof updates for sequential circuit
compositions without recursive proof composition. When the circuit grows
from C_j to C_{j+1}, the proof can be updated by adding polynomial
contributions for new wires only (O(n) instead of O(jn)).

Core idea (Paper Section 4.2, committed-state gamma-track construction):
  Given a previous proof pi_{j-1} = ([A_{j-1}]_1, [B_{j-1}]_2, [C_{j-1}]_1, [D_{j-1}]_1),
  the updated proof pi_j is computed as:
    A_j = A_{j-1} + sum_{i in I_j\I_{j-1}} a_i * u_i(x)
    B_j = B_{j-1} + sum_{i in I_j\I_{j-1}} a_i * v_i(x)
    C_j = C_{j-1} + incremental_witness_terms + (h_j - h_{j-1}) * t(x) / delta
    D_j = D_{j-1} + sum_{i in I_j^st \ I_{j-1}^st} a_i * [L_i(x)/gamma]_1
  Step j=1 is the same fold seeded by the base case
  (A_0, B_0, C_0, D_0) = (alpha, beta, 0, 0) with I_0 = {} and h_0 = 0,
  applied over the wire set {0} u I_1 (the constant wire 0 with a_0 = 1
  contributes the L_0 term).

Wire-track partition (single mode; strictly enforced at setup):
  - public io wires and state-output wires live on the gamma track:
      {[ (beta*u_i + alpha*v_i + w_i) / gamma ]_1}  for i in {0} u I^io u I^st
  - only free witness wires live on the delta track:
      {[ (beta*u_i + alpha*v_i + w_i) / delta ]_1}  for i in I^wt
  State wires NEVER carry delta-track elements. There is no eta track.

Verification (3 online pairings + O(|s_j|) increment MSM):
  - increment check: [D_j]_1 - [D_{j-1}]_1 = sum_i s_j[i] * [L_i(x)/gamma]_1
  - main equation:   e(A, B) = e(alpha, beta) * e(Q_j + D_j, gamma) * e(C, delta)
    with Q_j = sum_{i in {0} u I_j^io} a_i * [L_i(x)/gamma]_1 and
    e(alpha, beta) precomputed in the verification key.
  The verifier requires the caller-held trusted previous commitment D_prev
  (G1 zero is enforced inside Verify at step 1: the genesis guard).

NOTE: this single-mode gamma-track layout is intentionally incompatible
with the retired eta-track / bind_state dual-mode in-memory layouts. Old
keys and proofs cannot be reused.

Zero-extension admissibility invariant: every valid C_j assignment,
zero-padded on the wires of I_B \ I_j, must satisfy C_B. The prover
enforces this with a runtime preflight and rejects violating circuits
with a defined error (see r1cs_uvc_ppzksnark_prover).

Reference:
  "Lightweight Verifiable Computation with Constant-Size Proofs for
   Sequential State Transitions"
  Heewon Chung and Jae Hong Seo
  Sections 3.3 and 4.2

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef R1CS_UVC_PPZKSNARK_HPP_
#define R1CS_UVC_PPZKSNARK_HPP_

#include <memory>
#include <vector>

#include <libff/algebra/curves/public_params.hpp>

#include <libsnark/common/data_structures/accumulation_vector.hpp>
#include <libsnark/knowledge_commitment/knowledge_commitment.hpp>
#include <libsnark/relations/constraint_satisfaction_problems/r1cs/r1cs.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark_params.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/r1cs_vc_ppzksnark.hpp>

namespace libsnark {

/**
 * Per-step proving data for the UVC scheme.
 *
 * For each composition step j > 1, stores the CRS elements needed to
 * incrementally update the proof from step j-1 to step j.
 * These correspond to new wires I_j \ I_{j-1}.
 *
 * Step j=1 needs no materialized delta block: its fold is a view over the
 * base C_B queries (wires 0..|I_1|) and the gamma st_query slice for
 * I_1^st, so no CRS element is duplicated for the base case.
 */
template<typename ppT>
struct uvc_step_proving_data {
    /* Index range of new wires at this step (in the composed constraint system) */
    size_t new_wire_start;    /* first new wire index (inclusive) */
    size_t new_wire_count;    /* number of new wires */

    /* Number of new input/output, state-output, and witness wires */
    size_t new_io_count;
    size_t new_st_count;
    size_t new_wt_count;

    /* G1 elements [u_i(x)]_1 for new A-query wires */
    libff::G1_vector<ppT> A_query_delta;

    /* G2 elements [v_i(x)]_2 for new B-query wires (knowledge commitment) */
    knowledge_commitment_vector<libff::G2<ppT>, libff::G1<ppT> > B_query_delta;

    /* G1 elements [(beta*u_i(x) + alpha*v_i(x) + w_i(x))/delta]_1 for new
       witness wires (state-output positions hold the zero element: state
       wires never carry delta-track elements). */
    libff::G1_vector<ppT> L_query_delta;
    /* G1 elements [(beta*u_i(x) + alpha*v_i(x) + w_i(x))/gamma]_1 for new
       state-output wires (a copy of the corresponding gamma st_query slice). */
    libff::G1_vector<ppT> st_query_delta;

    uvc_step_proving_data() : new_wire_start(0), new_wire_count(0), new_io_count(0), new_st_count(0), new_wt_count(0) {}
};


/******************************** UVC Proving key ****************************/

template<typename ppT>
class r1cs_uvc_ppzksnark_proving_key {
public:
    /* VC proving key holding C_B's full QAP queries.
       A/B queries cover ALL wires in C_B; the L query covers non-public
       wires with state-output positions zeroed (delta-track exclusion).
       H query uses C_B's evaluation domain and vanishing polynomial.
       constraint_system = C_B (the full composed circuit). */
    r1cs_vc_ppzksnark_proving_key<ppT> base_pk;

    /* Per-step incremental data for steps j=2,...,B */
    std::vector<uvc_step_proving_data<ppT> > step_data;

    /* H-query for the full domain (degree Bn-2): [x^i * Z(t) / delta]_1 */
    libff::G1_vector<ppT> H_query_full;
    /* G1 elements [(beta*u_i + alpha*v_i + w_i)/gamma]_1 for all I^st wires */
    libff::G1_vector<ppT> st_query;

    /* Maximum number of compositions */
    size_t max_compositions;

    /* State transition circuit description */
    state_transition_circuit<libff::Fr<ppT> > st_circuit;

    r1cs_uvc_ppzksnark_proving_key() : max_compositions(0) {}

    void print_size() const
    {
        libff::print_indent(); printf("* UVC max compositions (B): %zu\n", max_compositions);
        base_pk.print_size();
        libff::print_indent(); printf("* H_query_full size: %zu\n", H_query_full.size());
        libff::print_indent(); printf("* st_query size: %zu\n", st_query.size());
        for (size_t j = 0; j < step_data.size(); ++j)
        {
            libff::print_indent(); printf("* Step %zu: %zu new wires (%zu io, %zu st, %zu wt)\n",
                j+2, step_data[j].new_wire_count, step_data[j].new_io_count,
                step_data[j].new_st_count, step_data[j].new_wt_count);
        }
    }
};


/******************************* UVC Verification key ************************/

template<typename ppT>
class r1cs_uvc_ppzksnark_verification_key {
public:
    /* Precomputed pairing e(alpha, beta) */
    libff::GT<ppT> alpha_g1_beta_g2;
    libff::G2<ppT> gamma_g2;
    libff::G2<ppT> delta_g2;

    /* Accumulation vector for public input wires (s_0, t_1).
       Fixed across all steps since the public inputs are always
       the initial state and first transition. */
    accumulation_vector<libff::G1<ppT> > gamma_ABC_g1;
    /* State-output wire commitments [(beta*u_i + alpha*v_i + w_i)/gamma]_1 in I^st order */
    libff::G1_vector<ppT> st_ABC_g1;

    size_t max_compositions;
    size_t state_size;
    size_t transition_size;
    size_t new_per_step;

    r1cs_uvc_ppzksnark_verification_key() :
        max_compositions(0),
        state_size(0), transition_size(0), new_per_step(0) {}

    void print_size() const
    {
        libff::print_indent(); printf("* UVC VK: %zu max steps\n", max_compositions);
        libff::print_indent(); printf("* G2 elements in VK: 2\n");
        libff::print_indent(); printf("* GT elements in VK: 1\n");
        libff::print_indent(); printf("* gamma_ABC size: %zu\n", gamma_ABC_g1.size());
        libff::print_indent(); printf("* st_ABC size: %zu\n", st_ABC_g1.size());
        libff::print_indent(); printf("* State layout: state=%zu, transition=%zu, new/step=%zu\n",
                                     state_size, transition_size, new_per_step);
    }
};


/********************************** Key pair *********************************/

template<typename ppT>
class r1cs_uvc_ppzksnark_keypair {
public:
    r1cs_uvc_ppzksnark_proving_key<ppT> pk;
    r1cs_uvc_ppzksnark_verification_key<ppT> vk;

    r1cs_uvc_ppzksnark_keypair() = default;
    r1cs_uvc_ppzksnark_keypair(r1cs_uvc_ppzksnark_proving_key<ppT> &&pk,
                               r1cs_uvc_ppzksnark_verification_key<ppT> &&vk) :
        pk(std::move(pk)), vk(std::move(vk)) {}
};


/*********************************** Proof ***********************************/

/**
 * A UVC proof contains the VC proof elements (A, B, C), the state-binding
 * commitment D, and cached state for incremental updates. Every proof is
 * state-bound; verification requires a trusted caller-held D_prev.
 *
 * Published proof: 3 G1 (A, C, D) + 1 G2 (B) = 160 bytes on BN254
 * with point compression.
 */
template<typename ppT>
class r1cs_uvc_ppzksnark_proof {
public:
    /* The actual proof: VC proof elements plus the state commitment */
    libff::G1<ppT> g_A;
    libff::G2<ppT> g_B;
    libff::G1<ppT> g_C;
    libff::G1<ppT> g_D;

    /* Step index (which composition step this proof is for) */
    size_t step;

    /* Cached h(x) polynomial coefficients for incremental update */
    std::vector<libff::Fr<ppT> > cached_h_coefficients;
    r1cs_uvc_ppzksnark_proof() : g_D(libff::G1<ppT>::zero()), step(0)
    {
        g_A = libff::G1<ppT>::one();
        g_B = libff::G2<ppT>::one();
        g_C = libff::G1<ppT>::one();
    }

    r1cs_uvc_ppzksnark_proof(libff::G1<ppT> &&g_A,
                              libff::G2<ppT> &&g_B,
                              libff::G1<ppT> &&g_C,
                              libff::G1<ppT> &&g_D,
                              size_t step,
                              std::vector<libff::Fr<ppT> > &&cached_h) :
        g_A(std::move(g_A)), g_B(std::move(g_B)), g_C(std::move(g_C)),
        g_D(std::move(g_D)),
        step(step),
        cached_h_coefficients(std::move(cached_h)) {}

    size_t G1_size() const { return 3; }
    size_t G2_size() const { return 1; }

    size_t size_in_bits() const
    {
        return G1_size() * libff::G1<ppT>::size_in_bits() + G2_size() * libff::G2<ppT>::size_in_bits();
    }

    void print_size() const
    {
        libff::print_indent(); printf("* UVC proof at step %zu\n", step);
        libff::print_indent(); printf("* G1 elements in proof: %zu\n", G1_size());
        libff::print_indent(); printf("* G2 elements in proof: %zu\n", G2_size());
        libff::print_indent(); printf("* Proof size in bits: %zu\n", size_in_bits());
        libff::print_indent(); printf("* Cached h poly size: %zu\n", cached_h_coefficients.size());
    }

    bool is_well_formed() const
    {
        return (g_A.is_well_formed() && g_B.is_well_formed() && g_C.is_well_formed() &&
                g_D.is_well_formed());
    }
};


/***************************** Main algorithms *******************************/

/**
 * UVC.Setup: Generate proving and verification keys for up to B compositions.
 *
 * Enforces the wire-track partition invariant (io u st on gamma, wt on
 * delta, tracks disjoint) and fails hard when it is violated.
 *
 * @param st_circuit  The base state transition circuit
 * @param B           Maximum number of sequential compositions
 */
template<typename ppT>
r1cs_uvc_ppzksnark_keypair<ppT> r1cs_uvc_ppzksnark_generator(
    const state_transition_circuit<libff::Fr<ppT> > &st_circuit,
    size_t B);

/**
 * UVC.Prove: Generate or incrementally update a proof.
 *
 * Every step is the same fold. For j=1 the previous accumulator is the
 * base case (alpha, beta, 0, 0) and the fold ranges over {0} u I_1
 * (wire 0 contributes with a_0 = 1); for j>1 the previous accumulator is
 * prev_proof and the fold ranges over I_j \ I_{j-1}.
 *
 * Throws std::invalid_argument when the padded assignment violates the
 * zero-extension admissibility invariant (runtime preflight; effective
 * in release builds).
 *
 * @param pk             UVC proving key
 * @param step           Current composition step j (1-indexed)
 * @param primary_input  Statement for step j (s_0 and t_1)
 * @param auxiliary_input Full witness for step j
 * @param prev_proof     Previous proof (nullptr for j=1)
 */
template<typename ppT>
r1cs_uvc_ppzksnark_proof<ppT> r1cs_uvc_ppzksnark_prover(
    const r1cs_uvc_ppzksnark_proving_key<ppT> &pk,
    size_t step,
    const r1cs_uvc_ppzksnark_primary_input<ppT> &primary_input,
    const r1cs_uvc_ppzksnark_auxiliary_input<ppT> &auxiliary_input,
    const r1cs_uvc_ppzksnark_proof<ppT> *prev_proof = nullptr);

/**
 * UVC.Verify: Verify a proof at step j against the caller-held trusted
 * previous state commitment D_prev.
 *
 * D_prev is trusted caller-held state: G1::zero() at step 1 (enforced
 * inside Verify by the genesis guard), then g_D from the previous
 * accepted proof. It is never sourced from the proof under verification.
 * Callers advance their stored commitment only after acceptance.
 *
 * Online cost: exactly 3 pairings (e(A,B), e(Q_j + D_j, gamma),
 * e(C, delta) against the precomputed e(alpha, beta)) plus the
 * O(|s_j|) increment MSM.
 */
template<typename ppT>
bool r1cs_uvc_ppzksnark_verifier(
    const r1cs_uvc_ppzksnark_verification_key<ppT> &vk,
    size_t step,
    const r1cs_uvc_ppzksnark_primary_input<ppT> &primary_input,
    const std::vector<libff::Fr<ppT> > &reported_s_j,
    const r1cs_uvc_ppzksnark_proof<ppT> &proof,
    const libff::G1<ppT> &D_prev);


/***************************** Helper functions ******************************/

/**
 * Build the composed R1CS constraint system for step j.
 * C_j = sequential composition of j copies of the base circuit.
 */
template<typename FieldT>
r1cs_constraint_system<FieldT> build_composed_constraint_system(
    const state_transition_circuit<FieldT> &st_circuit,
    size_t j);

} // libsnark

#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.tcc>

#endif // R1CS_UVC_PPZKSNARK_HPP_
