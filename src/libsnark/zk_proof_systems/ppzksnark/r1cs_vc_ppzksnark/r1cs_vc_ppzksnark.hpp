/** @file
*****************************************************************************

Declaration of interfaces for a Verifiable Computation (VC) scheme for R1CS,
based on Groth16 without zero-knowledge randomization.

This is a variant of Groth's [Gro16] proof system with the randomization
(r, s blinding factors) removed. The resulting scheme provides:
- Perfect completeness
- Knowledge soundness
but NOT zero-knowledge (proofs are deterministic given the witness).

This is suitable for verifiable computation where the witness need not be
hidden from the verifier.

Reference:
  "Updatable Verifiable Computation without Recursive Proof Compositions"
  Heewon Chung and Jae Hong Seo
  Section 4.1

The verification equation is:
  e([A]_1, [B]_2) = e([alpha]_1, [beta]_2)
                   * e(acc, [gamma]_2)
                   * e([C]_1, [delta]_2)

where acc = sum_{i in I^io} a_i * [(beta*u_i(x) + alpha*v_i(x) + w_i(x))/gamma]_1

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef R1CS_VC_PPZKSNARK_HPP_
#define R1CS_VC_PPZKSNARK_HPP_

#include <memory>

#include <libff/algebra/curves/public_params.hpp>

#include <libsnark/common/data_structures/accumulation_vector.hpp>
#include <libsnark/knowledge_commitment/knowledge_commitment.hpp>
#include <libsnark/relations/constraint_satisfaction_problems/r1cs/r1cs.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/r1cs_vc_ppzksnark_params.hpp>

namespace libsnark {

/******************************** Proving key ********************************/

template<typename ppT>
class r1cs_vc_ppzksnark_proving_key;

template<typename ppT>
std::ostream& operator<<(std::ostream &out, const r1cs_vc_ppzksnark_proving_key<ppT> &pk);

template<typename ppT>
std::istream& operator>>(std::istream &in, r1cs_vc_ppzksnark_proving_key<ppT> &pk);

/**
 * A proving key for the R1CS VC scheme.
 *
 * Same structure as Groth16 proving key. The key does not include
 * delta_g1 since there is no randomization (no r*delta term).
 */
template<typename ppT>
class r1cs_vc_ppzksnark_proving_key {
public:
    libff::G1<ppT> alpha_g1;
    libff::G1<ppT> beta_g1;
    libff::G2<ppT> beta_g2;
    libff::G2<ppT> delta_g2;

    libff::G1_vector<ppT> A_query;
    knowledge_commitment_vector<libff::G2<ppT>, libff::G1<ppT> > B_query;
    libff::G1_vector<ppT> H_query;
    libff::G1_vector<ppT> L_query;

    r1cs_vc_ppzksnark_constraint_system<ppT> constraint_system;

    r1cs_vc_ppzksnark_proving_key() {};
    r1cs_vc_ppzksnark_proving_key<ppT>& operator=(const r1cs_vc_ppzksnark_proving_key<ppT> &other) = default;
    r1cs_vc_ppzksnark_proving_key(const r1cs_vc_ppzksnark_proving_key<ppT> &other) = default;
    r1cs_vc_ppzksnark_proving_key(r1cs_vc_ppzksnark_proving_key<ppT> &&other) = default;
    r1cs_vc_ppzksnark_proving_key(libff::G1<ppT> &&alpha_g1,
                                  libff::G1<ppT> &&beta_g1,
                                  libff::G2<ppT> &&beta_g2,
                                  libff::G2<ppT> &&delta_g2,
                                  libff::G1_vector<ppT> &&A_query,
                                  knowledge_commitment_vector<libff::G2<ppT>, libff::G1<ppT> > &&B_query,
                                  libff::G1_vector<ppT> &&H_query,
                                  libff::G1_vector<ppT> &&L_query,
                                  r1cs_vc_ppzksnark_constraint_system<ppT> &&constraint_system) :
        alpha_g1(std::move(alpha_g1)),
        beta_g1(std::move(beta_g1)),
        beta_g2(std::move(beta_g2)),
        delta_g2(std::move(delta_g2)),
        A_query(std::move(A_query)),
        B_query(std::move(B_query)),
        H_query(std::move(H_query)),
        L_query(std::move(L_query)),
        constraint_system(std::move(constraint_system))
    {};

    size_t G1_size() const
    {
        return 1 + A_query.size() + B_query.domain_size() + H_query.size() + L_query.size();
    }

    size_t G2_size() const
    {
        return 1 + B_query.domain_size();
    }

    size_t size_in_bits() const
    {
        return (libff::size_in_bits(A_query) + B_query.size_in_bits() +
                libff::size_in_bits(H_query) + libff::size_in_bits(L_query) +
                1 * libff::G1<ppT>::size_in_bits() + 1 * libff::G2<ppT>::size_in_bits());
    }

    void print_size() const
    {
        libff::print_indent(); printf("* G1 elements in PK: %zu\n", this->G1_size());
        libff::print_indent(); printf("* G2 elements in PK: %zu\n", this->G2_size());
        libff::print_indent(); printf("* PK size in bits: %zu\n", this->size_in_bits());
    }

    bool operator==(const r1cs_vc_ppzksnark_proving_key<ppT> &other) const;
    friend std::ostream& operator<< <ppT>(std::ostream &out, const r1cs_vc_ppzksnark_proving_key<ppT> &pk);
    friend std::istream& operator>> <ppT>(std::istream &in, r1cs_vc_ppzksnark_proving_key<ppT> &pk);
};


/******************************* Verification key ****************************/

template<typename ppT>
class r1cs_vc_ppzksnark_verification_key;

template<typename ppT>
std::ostream& operator<<(std::ostream &out, const r1cs_vc_ppzksnark_verification_key<ppT> &vk);

template<typename ppT>
std::istream& operator>>(std::istream &in, r1cs_vc_ppzksnark_verification_key<ppT> &vk);

/**
 * A verification key for the R1CS VC scheme.
 */
template<typename ppT>
class r1cs_vc_ppzksnark_verification_key {
public:
    libff::GT<ppT> alpha_g1_beta_g2;
    libff::G2<ppT> gamma_g2;
    libff::G2<ppT> delta_g2;

    accumulation_vector<libff::G1<ppT> > gamma_ABC_g1;

    r1cs_vc_ppzksnark_verification_key() = default;
    r1cs_vc_ppzksnark_verification_key(const libff::GT<ppT> &alpha_g1_beta_g2,
                                       const libff::G2<ppT> &gamma_g2,
                                       const libff::G2<ppT> &delta_g2,
                                       const accumulation_vector<libff::G1<ppT> > &gamma_ABC_g1) :
        alpha_g1_beta_g2(alpha_g1_beta_g2),
        gamma_g2(gamma_g2),
        delta_g2(delta_g2),
        gamma_ABC_g1(gamma_ABC_g1)
    {};

    size_t G1_size() const { return gamma_ABC_g1.size(); }
    size_t G2_size() const { return 2; }
    size_t GT_size() const { return 1; }

    size_t size_in_bits() const
    {
        return (gamma_ABC_g1.size_in_bits() + 2 * libff::G2<ppT>::size_in_bits());
    }

    void print_size() const
    {
        libff::print_indent(); printf("* G1 elements in VK: %zu\n", this->G1_size());
        libff::print_indent(); printf("* G2 elements in VK: %zu\n", this->G2_size());
        libff::print_indent(); printf("* GT elements in VK: %zu\n", this->GT_size());
        libff::print_indent(); printf("* VK size in bits: %zu\n", this->size_in_bits());
    }

    bool operator==(const r1cs_vc_ppzksnark_verification_key<ppT> &other) const;
    friend std::ostream& operator<< <ppT>(std::ostream &out, const r1cs_vc_ppzksnark_verification_key<ppT> &vk);
    friend std::istream& operator>> <ppT>(std::istream &in, r1cs_vc_ppzksnark_verification_key<ppT> &vk);
};


/************************ Processed verification key *************************/

template<typename ppT>
class r1cs_vc_ppzksnark_processed_verification_key {
public:
    libff::GT<ppT> vk_alpha_g1_beta_g2;
    libff::G2_precomp<ppT> vk_gamma_g2_precomp;
    libff::G2_precomp<ppT> vk_delta_g2_precomp;

    accumulation_vector<libff::G1<ppT> > gamma_ABC_g1;

    bool operator==(const r1cs_vc_ppzksnark_processed_verification_key &other) const;
};


/********************************** Key pair *********************************/

template<typename ppT>
class r1cs_vc_ppzksnark_keypair {
public:
    r1cs_vc_ppzksnark_proving_key<ppT> pk;
    r1cs_vc_ppzksnark_verification_key<ppT> vk;

    r1cs_vc_ppzksnark_keypair() = default;
    r1cs_vc_ppzksnark_keypair(const r1cs_vc_ppzksnark_keypair<ppT> &other) = default;
    r1cs_vc_ppzksnark_keypair(r1cs_vc_ppzksnark_proving_key<ppT> &&pk,
                              r1cs_vc_ppzksnark_verification_key<ppT> &&vk) :
        pk(std::move(pk)),
        vk(std::move(vk))
    {}
    r1cs_vc_ppzksnark_keypair(r1cs_vc_ppzksnark_keypair<ppT> &&other) = default;
};


/*********************************** Proof ***********************************/

template<typename ppT>
class r1cs_vc_ppzksnark_proof;

template<typename ppT>
std::ostream& operator<<(std::ostream &out, const r1cs_vc_ppzksnark_proof<ppT> &proof);

template<typename ppT>
std::istream& operator>>(std::istream &in, r1cs_vc_ppzksnark_proof<ppT> &proof);

/**
 * A proof for the R1CS VC scheme.
 *
 * The proof consists of three group elements: [A]_1, [B]_2, [C]_1.
 * Unlike Groth16, these are deterministic (no r,s randomization).
 */
template<typename ppT>
class r1cs_vc_ppzksnark_proof {
public:
    libff::G1<ppT> g_A;
    libff::G2<ppT> g_B;
    libff::G1<ppT> g_C;

    r1cs_vc_ppzksnark_proof()
    {
        this->g_A = libff::G1<ppT>::one();
        this->g_B = libff::G2<ppT>::one();
        this->g_C = libff::G1<ppT>::one();
    }
    r1cs_vc_ppzksnark_proof(libff::G1<ppT> &&g_A,
                            libff::G2<ppT> &&g_B,
                            libff::G1<ppT> &&g_C) :
        g_A(std::move(g_A)),
        g_B(std::move(g_B)),
        g_C(std::move(g_C))
    {};

    size_t G1_size() const { return 2; }
    size_t G2_size() const { return 1; }

    size_t size_in_bits() const
    {
        return G1_size() * libff::G1<ppT>::size_in_bits() + G2_size() * libff::G2<ppT>::size_in_bits();
    }

    void print_size() const
    {
        libff::print_indent(); printf("* G1 elements in proof: %zu\n", this->G1_size());
        libff::print_indent(); printf("* G2 elements in proof: %zu\n", this->G2_size());
        libff::print_indent(); printf("* Proof size in bits: %zu\n", this->size_in_bits());
    }

    bool is_well_formed() const
    {
        return (g_A.is_well_formed() && g_B.is_well_formed() && g_C.is_well_formed());
    }

    bool operator==(const r1cs_vc_ppzksnark_proof<ppT> &other) const;
    friend std::ostream& operator<< <ppT>(std::ostream &out, const r1cs_vc_ppzksnark_proof<ppT> &proof);
    friend std::istream& operator>> <ppT>(std::istream &in, r1cs_vc_ppzksnark_proof<ppT> &proof);
};


/***************************** Main algorithms *******************************/

/**
 * Generator (Setup) for the R1CS VC scheme.
 *
 * Given a R1CS constraint system CS, produces proving and verification keys.
 */
template<typename ppT>
r1cs_vc_ppzksnark_keypair<ppT> r1cs_vc_ppzksnark_generator(const r1cs_vc_ppzksnark_constraint_system<ppT> &cs);

/**
 * Prover for the R1CS VC scheme.
 *
 * Given a proving key, primary input (statement), and auxiliary input (witness),
 * produces a deterministic proof (no ZK randomization).
 *
 * Computes:
 *   A = [alpha + sum_i a_i * u_i(x)]_1
 *   B = [beta  + sum_i a_i * v_i(x)]_2
 *   C = [sum_{i in I^wt} a_i * (alpha*v_i(x) + beta*u_i(x) + w_i(x))/delta + h(x)*Z(x)/delta]_1
 */
template<typename ppT>
r1cs_vc_ppzksnark_proof<ppT> r1cs_vc_ppzksnark_prover(const r1cs_vc_ppzksnark_proving_key<ppT> &pk,
                                                       const r1cs_vc_ppzksnark_primary_input<ppT> &primary_input,
                                                       const r1cs_vc_ppzksnark_auxiliary_input<ppT> &auxiliary_input);

/**
 * Verifier (weak input consistency) for the R1CS VC scheme.
 */
template<typename ppT>
bool r1cs_vc_ppzksnark_verifier_weak_IC(const r1cs_vc_ppzksnark_verification_key<ppT> &vk,
                                        const r1cs_vc_ppzksnark_primary_input<ppT> &primary_input,
                                        const r1cs_vc_ppzksnark_proof<ppT> &proof);

/**
 * Verifier (strong input consistency) for the R1CS VC scheme.
 */
template<typename ppT>
bool r1cs_vc_ppzksnark_verifier_strong_IC(const r1cs_vc_ppzksnark_verification_key<ppT> &vk,
                                          const r1cs_vc_ppzksnark_primary_input<ppT> &primary_input,
                                          const r1cs_vc_ppzksnark_proof<ppT> &proof);

/**
 * Process verification key for faster online verification.
 */
template<typename ppT>
r1cs_vc_ppzksnark_processed_verification_key<ppT> r1cs_vc_ppzksnark_verifier_process_vk(const r1cs_vc_ppzksnark_verification_key<ppT> &vk);

/**
 * Online verifier (weak IC) using processed verification key.
 */
template<typename ppT>
bool r1cs_vc_ppzksnark_online_verifier_weak_IC(const r1cs_vc_ppzksnark_processed_verification_key<ppT> &pvk,
                                               const r1cs_vc_ppzksnark_primary_input<ppT> &primary_input,
                                               const r1cs_vc_ppzksnark_proof<ppT> &proof);

/**
 * Online verifier (strong IC) using processed verification key.
 */
template<typename ppT>
bool r1cs_vc_ppzksnark_online_verifier_strong_IC(const r1cs_vc_ppzksnark_processed_verification_key<ppT> &pvk,
                                                 const r1cs_vc_ppzksnark_primary_input<ppT> &primary_input,
                                                 const r1cs_vc_ppzksnark_proof<ppT> &proof);

} // libsnark

#include <libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/r1cs_vc_ppzksnark.tcc>

#endif // R1CS_VC_PPZKSNARK_HPP_
