/** @file Unit tests for the single-mode state-bound UVC prover and verifier. */

#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cctype>
#include <fstream>
#include <string>
#include <iterator>

#include <libff/common/profiling.hpp>
#include <libff/algebra/curves/alt_bn128/alt_bn128_pp.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.hpp>

using namespace libsnark;

template<typename FieldT>
state_transition_circuit<FieldT> make_multiplier()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 2;
    cs.auxiliary_input_size = 1;
    linear_combination<FieldT> A, B, C;
    A.add_term(1, FieldT::one());
    B.add_term(2, FieldT::one());
    C.add_term(3, FieldT::one());
    cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    return state_transition_circuit<FieldT>(cs, 1, 1);
}

template<typename FieldT>
state_transition_circuit<FieldT> make_constant_base_case()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 2;
    cs.auxiliary_input_size = 1;
    linear_combination<FieldT> A, B, C;
    A.add_term(0, FieldT::one());
    A.add_term(1, FieldT::one());
    A.add_term(2, FieldT::one());
    B.add_term(2, FieldT::one());
    C.add_term(3, FieldT::one());
    cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    return state_transition_circuit<FieldT>(cs, 1, 1);
}

template<typename FieldT>
state_transition_circuit<FieldT> make_zero_extension_rejector()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 2;
    cs.auxiliary_input_size = 1;
    linear_combination<FieldT> A, B, C;
    A.add_term(0, FieldT::one());
    A.add_term(1, FieldT::one());
    B.add_term(0, FieldT::one());
    B.add_term(2, FieldT::one());
    C.add_term(3, FieldT::one());
    cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    return state_transition_circuit<FieldT>(cs, 1, 1);
}

template<typename ppT>
struct multiplier_fixture {
    typedef libff::Fr<ppT> FieldT;
    state_transition_circuit<FieldT> st;
    r1cs_uvc_ppzksnark_keypair<ppT> kp;
    std::vector<std::vector<FieldT> > states;
    std::vector<std::vector<FieldT> > transitions;
    std::vector<std::vector<FieldT> > witnesses;
    std::vector<r1cs_uvc_ppzksnark_primary_input<ppT> > primaries;
    std::vector<r1cs_uvc_ppzksnark_proof<ppT> > proofs;

    multiplier_fixture()
    {
        st = make_multiplier<FieldT>();
        states = { {FieldT(3)}, {FieldT(15)}, {FieldT(105)}, {FieldT(210)} };
        transitions = { {FieldT(5)}, {FieldT(7)}, {FieldT(2)} };
        witnesses = { {}, {}, {} };
        kp = r1cs_uvc_ppzksnark_generator<ppT>(st, 3);
        for (size_t step = 1; step <= 3; ++step) {
            const auto assignment = build_composed_assignment(st, step, states, transitions, witnesses);
            primaries.push_back(assignment.first);
            proofs.push_back(r1cs_uvc_ppzksnark_prover<ppT>(
                kp.pk, step, assignment.first, assignment.second,
                step == 1 ? nullptr : &proofs[step - 2]));
        }
    }

    bool verify(const size_t index) const
    {
        return r1cs_uvc_ppzksnark_verifier<ppT>(
            kp.vk, index + 1, primaries[index], states[index + 1], proofs[index],
            index == 0 ? libff::G1<ppT>::zero() : proofs[index - 1].g_D);
    }
};

template<typename ppT>
bool test_honest_chain_and_proof_shape()
{
    multiplier_fixture<ppT> fix;
    bool pass = true;
    for (size_t i = 0; i < fix.proofs.size(); ++i) {
        pass &= fix.proofs[i].is_well_formed();
        pass &= fix.proofs[i].step == i + 1;
        pass &= fix.verify(i);
    }
    pass &= fix.proofs[0].G1_size() == 3 && fix.proofs[0].G2_size() == 1;
    printf("  honest chain and 3-G1/1-G2 proof shape: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_step1_d_identity()
{
    typedef libff::Fr<ppT> FieldT;
    multiplier_fixture<ppT> fix;
    libff::G1<ppT> expected = libff::G1<ppT>::zero();
    for (size_t i = 0; i < fix.st.state_size; ++i) {
        expected = expected + fix.states[1][i] * fix.kp.vk.st_ABC_g1[i];
    }
    bool pass = fix.proofs[0].g_D == expected;
    for (size_t step = 2; step <= fix.proofs.size(); ++step) {
        libff::G1<ppT> increment = libff::G1<ppT>::zero();
        for (size_t i = 0; i < fix.st.state_size; ++i) {
            increment = increment + fix.states[step][i] *
                fix.kp.vk.st_ABC_g1[(step - 1) * fix.st.state_size + i];
        }
        pass &= fix.proofs[step - 1].g_D - fix.proofs[step - 2].g_D == increment;
    }
    printf("  step-1 and incremental D identities: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_verifier_negatives()
{
    typedef libff::Fr<ppT> FieldT;
    multiplier_fixture<ppT> fix;
    const r1cs_uvc_ppzksnark_proof<ppT> &proof = fix.proofs[1];
    const libff::G1<ppT> D_prev = fix.proofs[0].g_D;
    bool pass = true;

    std::vector<FieldT> bad_primary = fix.primaries[1];
    bad_primary[0] += FieldT::one();
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, bad_primary, fix.states[2], proof, D_prev);
    std::vector<FieldT> short_primary = fix.primaries[1];
    short_primary.pop_back();
    std::vector<FieldT> long_primary = fix.primaries[1];
    long_primary.push_back(FieldT::zero());
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, short_primary, fix.states[2], proof, D_prev);
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, long_primary, fix.states[2], proof, D_prev);
    std::vector<FieldT> bad_state = fix.states[2];
    bad_state[0] += FieldT::one();
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, fix.primaries[1], bad_state, proof, D_prev);
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, fix.primaries[1], fix.states[2], proof, libff::G1<ppT>::zero());
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 3, fix.primaries[2], fix.states[3], fix.proofs[2], D_prev);
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 3, fix.primaries[2], fix.states[3], fix.proofs[2], fix.proofs[0].g_D);

    r1cs_uvc_ppzksnark_proof<ppT> forged = proof;
    forged.g_D = forged.g_D + libff::G1<ppT>::one();
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, fix.primaries[1], fix.states[2], forged, D_prev);
    r1cs_uvc_ppzksnark_proof<ppT> bad_A = proof;
    bad_A.g_A = libff::G1<ppT>::random_element();
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, fix.primaries[1], fix.states[2], bad_A, D_prev);
    r1cs_uvc_ppzksnark_proof<ppT> bad_B = proof;
    bad_B.g_B = libff::G2<ppT>::random_element();
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, fix.primaries[1], fix.states[2], bad_B, D_prev);
    r1cs_uvc_ppzksnark_proof<ppT> bad_C = proof;
    bad_C.g_C = libff::G1<ppT>::random_element();
    pass &= !r1cs_uvc_ppzksnark_verifier<ppT>(fix.kp.vk, 2, fix.primaries[1], fix.states[2], bad_C, D_prev);
    printf("  wrong inputs, states, D_prev, step, forged D, and A/B/C: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_genesis_guard()
{
    typedef libff::Fr<ppT> FieldT;
    multiplier_fixture<ppT> fix;
    FieldT p = FieldT::random_element();
    if (p.is_zero()) p = FieldT::one();
    const libff::G1<ppT> P = p * libff::G1<ppT>::one();
    r1cs_uvc_ppzksnark_proof<ppT> shifted = fix.proofs[0];
    shifted.g_D = shifted.g_D + P;
    const bool honest = r1cs_uvc_ppzksnark_verifier<ppT>(
        fix.kp.vk, 1, fix.primaries[0], fix.states[1], fix.proofs[0], libff::G1<ppT>::zero());
    const bool guarded = !r1cs_uvc_ppzksnark_verifier<ppT>(
        fix.kp.vk, 1, fix.primaries[0], fix.states[1], shifted, P);
    printf("  genesis guard: %s\n", honest && guarded ? "PASS" : "FAIL");
    return honest && guarded;
}

template<typename ppT>
bool test_wire0_constant_base_case()
{
    typedef libff::Fr<ppT> FieldT;
    const state_transition_circuit<FieldT> st = make_constant_base_case<FieldT>();
    const std::vector<std::vector<FieldT> > states = { {FieldT(2)}, {FieldT(18)}, {FieldT(42)} };
    const std::vector<std::vector<FieldT> > transitions = { {FieldT(3)}, {FieldT(2)} };
    const std::vector<std::vector<FieldT> > witnesses = { {}, {} };
    bool pass = true;
    for (size_t B = 1; B <= 2; ++B) {
        const auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);
        const auto assignment_1 = build_composed_assignment(st, 1, states, transitions, witnesses);
        const auto proof_1 = r1cs_uvc_ppzksnark_prover<ppT>(kp.pk, 1, assignment_1.first, assignment_1.second);
        pass &= r1cs_uvc_ppzksnark_verifier<ppT>(
            kp.vk, 1, assignment_1.first, states[1], proof_1, libff::G1<ppT>::zero());
        if (B == 2) {
            const auto assignment_2 = build_composed_assignment(st, 2, states, transitions, witnesses);
            const auto proof_2 = r1cs_uvc_ppzksnark_prover<ppT>(
                kp.pk, 2, assignment_2.first, assignment_2.second, &proof_1);
            pass &= r1cs_uvc_ppzksnark_verifier<ppT>(
                kp.vk, 2, assignment_2.first, states[2], proof_2, proof_1.g_D);
        }
    }
    printf("  wire-0 constant base case (B=1 and B=2): %s\n", pass ? "PASS" : "FAIL");
    return pass;
}
template<typename ppT>
bool test_partial_B()
{
    typedef libff::Fr<ppT> FieldT;
    const state_transition_circuit<FieldT> st = make_multiplier<FieldT>();
    const std::vector<std::vector<FieldT> > states = { {FieldT(2)}, {FieldT(6)} };
    const std::vector<std::vector<FieldT> > transitions = { {FieldT(3)} };
    const std::vector<std::vector<FieldT> > witnesses = { {} };
    const auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, 10);
    const auto assignment = build_composed_assignment(st, 1, states, transitions, witnesses);
    const auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
        kp.pk, 1, assignment.first, assignment.second);
    const bool pass = r1cs_uvc_ppzksnark_verifier<ppT>(
        kp.vk, 1, assignment.first, states[1], proof, libff::G1<ppT>::zero());
    printf("  partial B=10 step-1 proof: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}


template<typename ppT>
bool test_zero_extension_admissibility_reject()
{
    typedef libff::Fr<ppT> FieldT;
    const state_transition_circuit<FieldT> st = make_zero_extension_rejector<FieldT>();
    const std::vector<std::vector<FieldT> > states = { {FieldT(2)}, {FieldT(12)} };
    const std::vector<std::vector<FieldT> > transitions = { {FieldT(3)} };
    const std::vector<std::vector<FieldT> > witnesses = { {} };
    const auto assignment = build_composed_assignment(st, 1, states, transitions, witnesses);
    const auto kp_1 = r1cs_uvc_ppzksnark_generator<ppT>(st, 1);
    const auto proof_1 = r1cs_uvc_ppzksnark_prover<ppT>(kp_1.pk, 1, assignment.first, assignment.second);
    const bool B1_passes = r1cs_uvc_ppzksnark_verifier<ppT>(
        kp_1.vk, 1, assignment.first, states[1], proof_1, libff::G1<ppT>::zero());
    const auto kp_2 = r1cs_uvc_ppzksnark_generator<ppT>(st, 2);
    bool rejected = false;
    try {
        (void)r1cs_uvc_ppzksnark_prover<ppT>(kp_2.pk, 1, assignment.first, assignment.second);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    printf("  B=1 passes and B=2 zero-extension rejects: %s\n", B1_passes && rejected ? "PASS" : "FAIL");
    return B1_passes && rejected;
}

#ifdef UVC_TCC_SOURCE_PATH
size_t count_literal(const std::string &body, const std::string &token)
{
    size_t count = 0;
    size_t pos = 0;
    while ((pos = body.find(token, pos)) != std::string::npos) {
        ++count;
        pos += token.size();
    }
    return count;
}

bool is_identifier_character(const char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

size_t count_eta_tokens(const std::string &body)
{
    size_t count = 0;
    size_t pos = 0;
    while ((pos = body.find("eta", pos)) != std::string::npos) {
        const bool left_is_identifier = pos != 0 && is_identifier_character(body[pos - 1]);
        const size_t right = pos + 3;
        const bool right_is_identifier =
            right < body.size() && is_identifier_character(body[right]);
        if (!left_is_identifier && !right_is_identifier) {
            ++count;
        }
        pos += 3;
    }
    return count;
}

bool test_verifier_source_structure()
{
    std::ifstream source(UVC_TCC_SOURCE_PATH);
    const std::string contents((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
    const std::string marker = "bool r1cs_uvc_ppzksnark_verifier(";
    size_t verifier = std::string::npos;
    size_t found = 0;
    while ((found = contents.find(marker, found)) != std::string::npos) {
        verifier = found;
        ++found;
    }

    bool pass = source.good() || source.eof();
    const size_t opening = verifier == std::string::npos
        ? std::string::npos : contents.find('{', verifier + marker.size());
    if (opening == std::string::npos) {
        pass = false;
    }

    size_t closing = std::string::npos;
    if (pass) {
        size_t depth = 0;
        for (size_t i = opening; i < contents.size(); ++i) {
            if (contents[i] == '{') {
                ++depth;
            } else if (contents[i] == '}' && --depth == 0) {
                closing = i;
                break;
            }
        }
        if (closing == std::string::npos) {
            pass = false;
        }
    }

    const std::string body = pass ? contents.substr(opening, closing - opening + 1) : "";
    pass &= count_literal(body, "ppT::miller_loop(") == 1;
    pass &= count_literal(body, "ppT::double_miller_loop(") == 1;
    pass &= count_literal(body, "ppT::final_exponentiation(") == 1;
    pass &= count_eta_tokens(body) == 0;
    pass &= count_literal(body, "reduced_pairing") == 0;
    pass &= count_literal(body, "::pairing") == 0;
    pass &= count_literal(body, "alpha_g1_beta_g2") != 0;
    printf("  verifier source pairing structure: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}
#endif

template<typename ppT>
bool test_proof_step_mismatch()
{
    multiplier_fixture<ppT> fix;
    const bool step_3_at_2 = !r1cs_uvc_ppzksnark_verifier<ppT>(
        fix.kp.vk, 2, fix.primaries[2], fix.states[3], fix.proofs[2], fix.proofs[1].g_D);
    const bool step_2_at_3 = !r1cs_uvc_ppzksnark_verifier<ppT>(
        fix.kp.vk, 3, fix.primaries[1], fix.states[2], fix.proofs[1], fix.proofs[0].g_D);
    const bool pass = step_3_at_2 && step_2_at_3;
    printf("  proof step mismatch rejection: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

template<typename ppT>
bool test_no_double_count_identity()
{
    typedef libff::Fr<ppT> FieldT;
    multiplier_fixture<ppT> fix;
    const size_t ss = fix.st.state_size;
    const size_t ts = fix.st.transition_size;
    const size_t num_inputs = ss + ts;
    libff::G1<ppT> delta_msm = libff::G1<ppT>::zero();
    libff::G1<ppT> gamma_msm = libff::G1<ppT>::zero();

    for (size_t i = 0; i < ss; ++i) {
        const size_t state_output_wire = ss + ts + 1 + i;
        const size_t L_query_index = state_output_wire - (num_inputs + 1);
        const FieldT &scalar = fix.states[1][i];
        delta_msm = delta_msm + scalar * fix.kp.pk.base_pk.L_query[L_query_index];
        gamma_msm = gamma_msm + scalar * fix.kp.vk.st_ABC_g1[i];
    }

    const bool pass = delta_msm.is_zero() && fix.proofs[0].g_D == gamma_msm;
    printf("  state output is gamma-only (no C/D double count): %s\n",
           pass ? "PASS" : "FAIL");
    return pass;
}
int main()
{
    libff::alt_bn128_pp::init_public_params();
    libff::inhibit_profiling_info = true;
    bool pass = true;
    pass &= test_honest_chain_and_proof_shape<libff::alt_bn128_pp>();
    pass &= test_step1_d_identity<libff::alt_bn128_pp>();
    pass &= test_verifier_negatives<libff::alt_bn128_pp>();
    pass &= test_genesis_guard<libff::alt_bn128_pp>();
    pass &= test_wire0_constant_base_case<libff::alt_bn128_pp>();
    pass &= test_partial_B<libff::alt_bn128_pp>();
    pass &= test_zero_extension_admissibility_reject<libff::alt_bn128_pp>();
#ifdef UVC_TCC_SOURCE_PATH
    pass &= test_verifier_source_structure();
#endif
    pass &= test_proof_step_mismatch<libff::alt_bn128_pp>();
    pass &= test_no_double_count_identity<libff::alt_bn128_pp>();
    return pass ? 0 : 1;
}
