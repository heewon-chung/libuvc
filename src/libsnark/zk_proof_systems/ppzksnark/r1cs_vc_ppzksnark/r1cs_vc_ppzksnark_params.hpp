/** @file
*****************************************************************************

Declaration of public-parameter selector for the R1CS VC (Verifiable Computation)
scheme based on Groth16 without zero-knowledge randomization.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef R1CS_VC_PPZKSNARK_PARAMS_HPP_
#define R1CS_VC_PPZKSNARK_PARAMS_HPP_

#include <libff/algebra/curves/public_params.hpp>

#include <libsnark/relations/constraint_satisfaction_problems/r1cs/r1cs.hpp>

namespace libsnark {

template<typename ppT>
using r1cs_vc_ppzksnark_constraint_system = r1cs_constraint_system<libff::Fr<ppT> >;

template<typename ppT>
using r1cs_vc_ppzksnark_primary_input = r1cs_primary_input<libff::Fr<ppT> >;

template<typename ppT>
using r1cs_vc_ppzksnark_auxiliary_input = r1cs_auxiliary_input<libff::Fr<ppT> >;

} // libsnark

#endif // R1CS_VC_PPZKSNARK_PARAMS_HPP_
