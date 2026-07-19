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

#include <algorithm>
#include <vector>
#include <cassert>
#include <stdexcept>

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
 * The public statement is (s_0, t_1). The reported current output s_j is
 * supplied out-of-band to the state-bound verifier; all intermediate states
 * s_1,...,s_{j-1} and transitions t_2,...,t_j are witness.
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
enum class uvc_wire_class {
    io,
    st,
    wt
};

/*
 * Release-effective validation of the state-transition wire layout.
 * Throws std::logic_error on malformed metadata (B = 0, state/transition
 * sizes exceeding the per-step wire count, or state-output positions that
 * fall outside the composed wire range). Called before any helper indexes
 * a class table so the fail-hard partition invariant cannot be preceded
 * by out-of-bounds writes.
 */
template<typename FieldT>
void uvc_validate_layout(
    const state_transition_circuit<FieldT> &st_circuit,
    const size_t B)
{
    const size_t n = st_circuit.wires_per_step();
    const size_t ss = st_circuit.state_size;
    const size_t ts = st_circuit.transition_size;
    if (B < 1) {
        throw std::logic_error("uvc: composition bound B must be >= 1");
    }
    if (ss == 0 || ss > n || ts > n || 2 * ss + ts > n) {
        throw std::logic_error(
            "uvc: malformed state-transition layout "
            "(need 0 < state_size and 2*state_size + transition_size <= wires_per_step)");
    }
    const size_t new_per_step = n - ss;
    const size_t total_vars = n + (B - 1) * new_per_step;
    /* Last step's state-output slice must fit inside the composed wires. */
    const size_t last_output_end = (B - 1) * new_per_step + ss + ts + ss;
    if (last_output_end > total_vars) {
        throw std::logic_error(
            "uvc: state-output wires fall outside the composed circuit");
    }
}

/*
 * Returns I^st in increasing composed-wire order. The position for state
 * component i (0-based) of step k (1-based) is (k - 1) * state_size + i.
 */
template<typename FieldT>
std::vector<size_t> uvc_state_output_indices(
    const state_transition_circuit<FieldT> &st_circuit,
    const size_t B)
{
    uvc_validate_layout(st_circuit, B);

    const size_t ss = st_circuit.state_size;
    const size_t ts = st_circuit.transition_size;
    const size_t new_per_step = st_circuit.num_new_wires_per_step();
    std::vector<size_t> result;
    result.reserve(B * ss);

    for (size_t k = 1; k <= B; ++k) {
        const size_t output_start = (k - 1) * new_per_step + ss + ts + 1;
        for (size_t i = 0; i < ss; ++i) {
            result.emplace_back(output_start + i);
        }
    }

    return result;
}

/*
 * Precomputed per-wire class table for the composed circuit C_B.
 * Index by composed wire number 1..total_vars (index 0 is the constant
 * wire and is set to io as a placeholder; callers must not query it).
 *
 * Use this table instead of repeated uvc_wire_class_of() calls when
 * classifying many wires: uvc_wire_class_of() rebuilds the I^st index
 * vector on every call (O(B*state_size)), which is prohibitive inside
 * generator loops over all wires.
 */
template<typename FieldT>
std::vector<uvc_wire_class> uvc_wire_classes(
    const state_transition_circuit<FieldT> &st_circuit,
    const size_t B)
{
    uvc_validate_layout(st_circuit, B);

    const size_t total_vars = st_circuit.wires_per_step() +
                              (B - 1) * st_circuit.num_new_wires_per_step();
    const size_t num_inputs = st_circuit.state_size + st_circuit.transition_size;

    std::vector<uvc_wire_class> classes(total_vars + 1, uvc_wire_class::wt);
    classes[0] = uvc_wire_class::io; /* constant-wire placeholder; do not query */

    for (size_t wire = 1; wire <= num_inputs; ++wire) {
        classes[wire] = uvc_wire_class::io;
    }
    for (const size_t wire : uvc_state_output_indices(st_circuit, B)) {
        classes[wire] = uvc_wire_class::st;
    }

    return classes;
}

/*
 * Point query for a single wire's class. NOTE: rebuilds the I^st index
 * vector on every call (O(B*state_size)); for bulk classification use
 * uvc_wire_classes() instead.
 */
template<typename FieldT>
uvc_wire_class uvc_wire_class_of(
    const state_transition_circuit<FieldT> &st_circuit,
    const size_t B,
    const size_t wire_index)
{
    assert(B >= 1);
    assert(wire_index != 0);

    assert(wire_index <= st_circuit.wires_per_step() +
                         (B - 1) * st_circuit.num_new_wires_per_step());

    if (wire_index <= st_circuit.state_size + st_circuit.transition_size) {
        return uvc_wire_class::io;
    }

    const std::vector<size_t> state_indices = uvc_state_output_indices(st_circuit, B);
    if (std::binary_search(state_indices.begin(), state_indices.end(), wire_index)) {
        return uvc_wire_class::st;
    }

    return uvc_wire_class::wt;
}

/*
 * Checks that I^io, I^st, and I^wt are a disjoint cover of the composed
 * non-constant wires. Track encoding checks are performed by the generator.
 */
template<typename FieldT>
bool uvc_check_track_disjointness(
    const state_transition_circuit<FieldT> &st_circuit,
    const size_t B,
    const size_t total_vars)
{
    if (B == 0 ||
        total_vars != st_circuit.wires_per_step() +
                      (B - 1) * st_circuit.num_new_wires_per_step()) {
        return false;
    }
    /* Malformed layouts throw in uvc_validate_layout (via the helpers);
       this checker keeps its boolean contract and reports them as
       violations instead of propagating. */
    try {
        uvc_validate_layout(st_circuit, B);
    } catch (const std::logic_error &) {
        return false;
    }

    const size_t num_inputs = st_circuit.state_size + st_circuit.transition_size;
    const std::vector<size_t> state_indices = uvc_state_output_indices(st_circuit, B);
    for (size_t i = 0; i < state_indices.size(); ++i) {
        if (state_indices[i] <= num_inputs ||
            state_indices[i] > total_vars ||
            (i > 0 && state_indices[i - 1] >= state_indices[i])) {
            return false;
        }
    }

    const std::vector<uvc_wire_class> classes = uvc_wire_classes(st_circuit, B);

    for (size_t wire = 1; wire <= total_vars; ++wire) {
        size_t membership_count = 0;
        membership_count += (wire <= num_inputs);
        membership_count += std::binary_search(state_indices.begin(),
                                               state_indices.end(), wire);
        membership_count += (classes[wire] == uvc_wire_class::wt);
        if (membership_count != 1) {
            return false;
        }
    }

    return true;
}

} // libsnark

#endif // R1CS_UVC_PPZKSNARK_PARAMS_HPP_
