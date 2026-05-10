/** @file
*****************************************************************************

Declaration of public-parameter selector and circuit composition structures
for the R1CS Updatable Verifiable Computation (UVC) scheme.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef R1CS_UVC_PPZKSNARK_PARAMS_HPP_
#define R1CS_UVC_PPZKSNARK_PARAMS_HPP_

#include <vector>

#include <libff/algebra/curves/public_params.hpp>

#include <libsnark/relations/constraint_satisfaction_problems/r1cs/r1cs.hpp>

namespace libsnark {

template<typename ppT>
using r1cs_uvc_ppzksnark_constraint_system = r1cs_constraint_system<libff::Fr<ppT> >;

template<typename ppT>
using r1cs_uvc_ppzksnark_primary_input = r1cs_primary_input<libff::Fr<ppT> >;

template<typename ppT>
using r1cs_uvc_ppzksnark_auxiliary_input = r1cs_auxiliary_input<libff::Fr<ppT> >;

/**
 * Describes the wire structure of a state transition circuit C.
 *
 * The base circuit C computes: s_j = StUp(s_{j-1}, t_j)
 * Wire layout for one step:
 *   - state_size wires for state input s_{j-1}
 *   - transition_size wires for transition input t_j
 *   - state_size wires for state output s_j
 *   - (remaining wires are internal witness)
 *
 * For the primary input (statement), we expose: t_1 (first transition) and s_j (current output).
 * All intermediate states s_1,...,s_{j-1} and transitions t_2,...,t_j are witness.
 */
template<typename FieldT>
struct state_transition_circuit {
    r1cs_constraint_system<FieldT> base_cs;     /* R1CS for one step of computation */
    size_t state_size;                            /* number of wires for state (s) */
    size_t transition_size;                       /* number of wires for transition input (t) */

    /* Wire indices in base_cs (0-indexed after constant wire 0):
       [1, state_size]                              = state input s_{j-1}
       [state_size+1, state_size+transition_size]   = transition input t_j
       [state_size+transition_size+1, 2*state_size+transition_size] = state output s_j
       [2*state_size+transition_size+1, ...]         = internal witness
    */

    size_t wires_per_step() const
    {
        return base_cs.num_variables();  /* total wires per step (excluding constant wire 0) */
    }

    size_t num_new_wires_per_step() const
    {
        /* At step j>1, we reuse state_size wires from previous output,
           so new wires = total - state_size (the shared state input wires) */
        return wires_per_step() - state_size;
    }

    state_transition_circuit() : state_size(0), transition_size(0) {}
    state_transition_circuit(const r1cs_constraint_system<FieldT> &base_cs,
                             size_t state_size,
                             size_t transition_size) :
        base_cs(base_cs), state_size(state_size), transition_size(transition_size) {}
};

} // libsnark

#endif // R1CS_UVC_PPZKSNARK_PARAMS_HPP_
