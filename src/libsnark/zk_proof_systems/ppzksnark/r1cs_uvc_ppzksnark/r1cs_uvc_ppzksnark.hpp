/** @file
*****************************************************************************

Declaration of interfaces for the Updatable Verifiable Computation (UVC) scheme.

This scheme enables incremental proof updates for sequential circuit
compositions without recursive proof composition. When the circuit grows
from C_j to C_{j+1}, the proof can be updated by adding polynomial
contributions for new wires only (O(n) instead of O(jn)).

Core idea (Paper Section 4.2):
  Given a previous proof pi_{j-1} = ([A_{j-1}]_1, [B_{j-1}]_2, [C_{j-1}]_1),
  the updated proof pi_j is computed as:
    A_j = A_{j-1} + sum_{i in I_j\I_{j-1}} a_i * u_i(x)
    B_j = B_{j-1} + sum_{i in I_j\I_{j-1}} a_i * v_i(x)
    C_j = C_{j-1} + incremental_witness_terms + (h_j - h_{j-1}) * t(x) / delta

The legacy verifier delegates non-state-bound proofs to VC.Verify. The state-bound
verifier checks a four-pairing equation and requires caller-held trusted D_prev
(the previous accepted state commitment).

Reference:
  "Updatable Verifiable Computation without Recursive Proof Compositions"
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
 * For each composition step j, stores the CRS elements needed to
 * incrementally update the proof from step j-1 to step j.
 * These correspond to new wires I_j \ I_{j-1}.
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

    /* G1 elements [(beta*u_i(x) + alpha*v_i(x) + w_i(x))/delta]_1 for new witness wires */
    libff::G1_vector<ppT> L_query_delta;
    /* G1 elements [(beta*u_i(x) + alpha*v_i(x) + w_i(x))/eta]_1 for new state-output wires */
    libff::G1_vector<ppT> st_query_delta;

    uvc_step_proving_data() : new_wire_start(0), new_wire_count(0), new_io_count(0), new_st_count(0), new_wt_count(0) {}
};


/******************************** UVC Proving key ****************************/

template<typename ppT>
class r1cs_uvc_ppzksnark_proving_key {
public:
    /* VC proving key holding C_B's full QAP queries.
       A/B/L queries cover ALL wires in C_B.
       H query uses C_B's evaluation domain and vanishing polynomial.
       constraint_system = C_B (the full composed circuit). */
    r1cs_vc_ppzksnark_proving_key<ppT> base_pk;

    /* Per-step incremental data for steps j=2,...,B */
    std::vector<uvc_step_proving_data<ppT> > step_data;

    /* H-query for the full domain (degree Bn-2): [x^i * Z(t) / delta]_1 */
    libff::G1_vector<ppT> H_query_full;
    /* G1 elements [(beta*u_i(x) + alpha*v_i(x) + w_i(x))/eta]_1 for all I^st wires */
    libff::G1_vector<ppT> st_query;

    /* Maximum number of compositions */
    size_t max_compositions;
    /* Whether this key carries the eta state-binding track. */
    bool bind_state;

    /* State transition circuit description */
    state_transition_circuit<libff::Fr<ppT> > st_circuit;

    r1cs_uvc_ppzksnark_proving_key() : max_compositions(0), bind_state(false) {}

    void print_size() const
    {
        libff::print_indent(); printf("* UVC max compositions (B): %zu\n", max_compositions);
        base_pk.print_size();
        libff::print_indent(); printf("* H_query_full size: %zu\n", H_query_full.size());
        if (bind_state) {
            libff::print_indent(); printf("* st_query size: %zu\n", st_query.size());
        }
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
    libff::G2<ppT> eta_g2;

    /* Accumulation vector for public input wires (s_0, t_1).
       Fixed across all steps since the public inputs are always
       the initial state and first transition. */
    accumulation_vector<libff::G1<ppT> > gamma_ABC_g1;
    /* State-output wire commitments [(beta*u_i + alpha*v_i + w_i)/eta]_1 in I^st order */
    libff::G1_vector<ppT> st_ABC_g1;

    size_t max_compositions;
    bool bind_state;
    size_t state_size;
    size_t transition_size;
    size_t new_per_step;

    r1cs_uvc_ppzksnark_verification_key() :
        max_compositions(0), bind_state(false),
        state_size(0), transition_size(0), new_per_step(0) {}

    void print_size() const
    {
        libff::print_indent(); printf("* UVC VK: %zu max steps\n", max_compositions);
        libff::print_indent(); printf("* G2 elements in VK: %d\n", bind_state ? 3 : 2);
        libff::print_indent(); printf("* GT elements in VK: 1\n");
        libff::print_indent(); printf("* gamma_ABC size: %zu\n", gamma_ABC_g1.size());
        if (bind_state) {
            libff::print_indent(); printf("* st_ABC size: %zu\n", st_ABC_g1.size());
            libff::print_indent(); printf("* State layout: state=%zu, transition=%zu, new/step=%zu\n",
                                         state_size, transition_size, new_per_step);
        }
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
 * A UVC proof contains VC proof elements, an optional state-binding commitment,
 * and cached state for incremental updates. State-bound proofs must be verified
 * with the state-bound overload and a trusted D_prev from the caller.
 */
template<typename ppT>
class r1cs_uvc_ppzksnark_proof {
public:
    /* The actual proof: same format as VC proof */
    libff::G1<ppT> g_A;
    libff::G2<ppT> g_B;
    libff::G1<ppT> g_C;

    /* Step index (which composition step this proof is for) */
    size_t step;

    /* State-binding commitment; present only when bind_state is true. */
    bool bind_state;
    libff::G1<ppT> g_D;

    /* Cached h(x) polynomial coefficients for incremental update */
    std::vector<libff::Fr<ppT> > cached_h_coefficients;
    r1cs_uvc_ppzksnark_proof() : step(0), bind_state(false), g_D(libff::G1<ppT>::zero())
    {
        g_A = libff::G1<ppT>::one();
        g_B = libff::G2<ppT>::one();
        g_C = libff::G1<ppT>::one();
    }

    r1cs_uvc_ppzksnark_proof(libff::G1<ppT> &&g_A,
                              libff::G2<ppT> &&g_B,
                              libff::G1<ppT> &&g_C,
                              size_t step,
                              std::vector<libff::Fr<ppT> > &&cached_h) :
        g_A(std::move(g_A)), g_B(std::move(g_B)), g_C(std::move(g_C)),
        step(step), bind_state(false), g_D(libff::G1<ppT>::zero()),
        cached_h_coefficients(std::move(cached_h)) {}

    size_t G1_size() const { return bind_state ? 3 : 2; }
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
        libff::print_indent(); printf("* State binding: %s\n", bind_state ? "present" : "absent");
    }

    bool is_well_formed() const
    {
        return (g_A.is_well_formed() && g_B.is_well_formed() && g_C.is_well_formed() &&
                (!bind_state || g_D.is_well_formed()));
    }
};


/***************************** Main algorithms *******************************/

/**
 * UVC.Setup: Generate proving and verification keys for up to B compositions.
 *
 * @param st_circuit  The base state transition circuit
 * @param B           Maximum number of sequential compositions
 */
template<typename ppT>
r1cs_uvc_ppzksnark_keypair<ppT> r1cs_uvc_ppzksnark_generator(
    const state_transition_circuit<libff::Fr<ppT> > &st_circuit,
    size_t B,
    bool bind_state = false);

/**
 * UVC.Prove: Generate or incrementally update a proof.
 *
 * For step j=1: generates a fresh proof using VC.Prove
 * For step j>1: incrementally updates the previous proof
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
 * UVC.Verify: Verify a legacy non-state-bound proof at step j.
 *
 * Rejects state-bound keys and proofs; only non-state-bound proofs delegate to VC.Verify.
 */
template<typename ppT>
bool r1cs_uvc_ppzksnark_verifier(
    const r1cs_uvc_ppzksnark_verification_key<ppT> &vk,
    size_t step,
    const r1cs_uvc_ppzksnark_primary_input<ppT> &primary_input,
    const r1cs_uvc_ppzksnark_proof<ppT> &proof);
/**
 * UVC.Verify: Verify a state-bound proof at step j.
 *
 * D_prev is trusted caller-held state: G1::zero() at step 1, then g_D from
 * the previous accepted proof. It is never sourced from the proof under
 * verification.
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
