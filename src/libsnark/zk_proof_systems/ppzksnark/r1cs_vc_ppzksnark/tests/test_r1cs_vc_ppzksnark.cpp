/** @file
*****************************************************************************

Test for the R1CS VC (Verifiable Computation) scheme.
Tests Setup, Prove, Verify with simple arithmetic circuits.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#include <cassert>
#include <cstdio>

#include <libff/common/profiling.hpp>
#include <libff/common/utils.hpp>
#include <libff/algebra/curves/alt_bn128/alt_bn128_pp.hpp>

#include <libsnark/relations/constraint_satisfaction_problems/r1cs/examples/r1cs_examples.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/r1cs_vc_ppzksnark.hpp>

using namespace libsnark;

/**
 * Test the VC scheme with an auto-generated R1CS example.
 */
template<typename ppT>
bool test_vc_with_example(const size_t num_constraints,
                          const size_t input_size)
{
    printf("================================================================\n");
    printf("Testing VC scheme: %zu constraints, %zu inputs\n", num_constraints, input_size);
    printf("================================================================\n");

    const r1cs_example<libff::Fr<ppT> > example = generate_r1cs_example_with_binary_input<libff::Fr<ppT> >(num_constraints, input_size);

    printf("* R1CS num constraints: %zu\n", example.constraint_system.num_constraints());
    printf("* R1CS num variables: %zu\n", example.constraint_system.num_variables());
    printf("* R1CS num inputs: %zu\n", example.constraint_system.num_inputs());

    /* Test completeness: valid witness should verify */
    printf("\n--- Testing completeness ---\n");

    const r1cs_vc_ppzksnark_keypair<ppT> keypair = r1cs_vc_ppzksnark_generator<ppT>(example.constraint_system);

    const r1cs_vc_ppzksnark_proof<ppT> proof = r1cs_vc_ppzksnark_prover<ppT>(
        keypair.pk, example.primary_input, example.auxiliary_input);

    const bool ans_weak = r1cs_vc_ppzksnark_verifier_weak_IC<ppT>(
        keypair.vk, example.primary_input, proof);
    printf("* Verification (weak IC): %s\n", ans_weak ? "PASS" : "FAIL");

    const bool ans_strong = r1cs_vc_ppzksnark_verifier_strong_IC<ppT>(
        keypair.vk, example.primary_input, proof);
    printf("* Verification (strong IC): %s\n", ans_strong ? "PASS" : "FAIL");

    /* Test determinism: same inputs should produce same proof */
    printf("\n--- Testing determinism ---\n");

    const r1cs_vc_ppzksnark_proof<ppT> proof2 = r1cs_vc_ppzksnark_prover<ppT>(
        keypair.pk, example.primary_input, example.auxiliary_input);

    const bool deterministic = (proof == proof2);
    printf("* Proof determinism: %s\n", deterministic ? "PASS" : "FAIL");

    /* Test soundness: wrong input should fail verification */
    printf("\n--- Testing soundness ---\n");

    bool soundness_ok = true;
    if (example.primary_input.size() > 0)
    {
        r1cs_vc_ppzksnark_primary_input<ppT> bad_input = example.primary_input;
        bad_input[0] = libff::Fr<ppT>::random_element();

        const bool ans_bad = r1cs_vc_ppzksnark_verifier_strong_IC<ppT>(
            keypair.vk, bad_input, proof);
        if (ans_bad)
        {
            printf("* Soundness check: FAIL (accepted bad input)\n");
            soundness_ok = false;
        }
        else
        {
            printf("* Soundness check: PASS (rejected bad input)\n");
        }
    }

    printf("\n");
    return ans_weak && ans_strong && deterministic && soundness_ok;
}

int main()
{
    libff::alt_bn128_pp::init_public_params();
    libff::inhibit_profiling_info = true;

    bool all_pass = true;

    /* Test 1: Small circuit */
    all_pass &= test_vc_with_example<libff::alt_bn128_pp>(10, 5);

    /* Test 2: Medium circuit */
    all_pass &= test_vc_with_example<libff::alt_bn128_pp>(50, 10);

    /* Test 3: Larger circuit */
    all_pass &= test_vc_with_example<libff::alt_bn128_pp>(300, 20);

    if (all_pass)
    {
        printf("================================================================\n");
        printf("ALL VC TESTS PASSED\n");
        printf("================================================================\n");
        return 0;
    }
    else
    {
        printf("================================================================\n");
        printf("SOME VC TESTS FAILED\n");
        printf("================================================================\n");
        return 1;
    }
}
