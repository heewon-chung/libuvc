/** @file
*****************************************************************************

Performance benchmarks for the R1CS UVC (Updatable Verifiable Computation) scheme.

These benchmarks empirically validate the asymptotic complexity claims
from the paper and provide concrete timing data for comparison with
recursive proof composition approaches (e.g., Mina/Coda, Halo).

=== Benchmark descriptions ===

  Benchmark 1 (Setup time vs B):
    Measures generator wall-clock time as B (max compositions) increases.
    Expected: O(B·n) since the CRS has O(B·n) group elements.
    Also reports CRS size in G1 + G2 elements.

  Benchmark 2 (Prover: base vs incremental):
    Compares step-1 (base) proving time against step j>1 (incremental).
    Base proving requires a full multi-scalar multiplication over all C_B
    wires. Incremental proving reuses the previous proof and only adds
    contributions from new wires (n - state_size per step), so it should
    be significantly faster than re-proving from scratch.
    This is the core advantage of the UVC approach.

  Benchmark 3 (Verifier time):
    Measures verification time at each step. Since the verifier performs
    a fixed number of pairings (3 pairings + 1 input accumulation) that
    depend only on the primary input size (s_0, t_1), verification cost
    should be approximately CONSTANT regardless of step count j.

  Benchmark 4 (Proof size):
    Reports proof size at various steps and B values. The proof consists
    of exactly 2 G1 + 1 G2 elements regardless of B or j, so proof size
    should be constant (same as standard Groth16).

  Benchmark 5 (Circuit comparison):
    Compares performance between different circuit sizes (multiplier vs
    two-state) to show how per-step wire count affects scaling.

=== How to build and run ===

    cd build
    make bench_r1cs_uvc_ppzksnark
    ./bench_r1cs_uvc_ppzksnark

=== Expected output (example, timings vary by machine) ===

    ================================================================
    Benchmark: UVC.Setup time vs B
    ================================================================
      Circuit: multiplier (n=1 constraints/step)

      B       Total vars    Constraints   Time (ms)   CRS G1+G2
      1       3             1             ~7          10 + 6
      2       5             2             ~9          19 + 10
      5       11            5             ~11         46 + 22
      10      21            10            ~15         88 + 42
      20      41            20            ~24         172 + 82
      50      101           50            ~49         424 + 202
      ← Setup time and CRS size grow linearly with B

    ================================================================
    Benchmark: UVC.Prove time — base vs incremental
    ================================================================
      Circuit: multiplier, B=20

      Step    Type      Time (ms)     h coeff len
      1       base      ~3.5          15         ← full QAP witness computation
      2       incr      ~1.0          15         ← update: adds delta for 2 new wires
      3       incr      ~1.0          15
      ...
      20      incr      ~1.0          15
      ← Incremental proving is faster than base (fewer MSMs)
      ← h coefficient length is constant (fixed by C_B domain size)

    ================================================================
    Benchmark: UVC.Verify time (constant across steps)
    ================================================================
      Circuit: multiplier, B=10 (averaged over 5 runs)

      Step    Time (ms)     Result
      1       ~1.6          PASS
      2       ~1.6          PASS
      ...
      10      ~1.6          PASS
      ← Verification time is approximately constant (~3 pairings)

    ================================================================
    Benchmark: Proof size (constant across steps and B)
    ================================================================
      B       Step    G1 elts   G2 elts   Size (bits)
      1       1       2         1         1019
      5       1       2         1         1019
      5       5       2         1         1019
      10      1       2         1         1019
      10      10      2         1         1019
      ← Proof is always 2 G1 + 1 G2 = 1019 bits (same as Groth16)

    ================================================================
    Benchmark: Circuit size comparison
    ================================================================
      Multiplier (n=3, 1 constraint/step), B=10
        Setup: ~15 ms
        Prove (base): ~3.5 ms
        Prove (incr x4): ~4.0 ms total, ~1.0 ms/step avg

      Two-state mult (n=5, 2 constraints/step), B=10
        Setup: ~18 ms
        Prove (base): ~4.5 ms
        Prove (incr x4): ~5.0 ms total, ~1.2 ms/step avg
      ← Larger circuit → proportionally more work per step

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#include <cassert>
#include <cstdio>
#include <chrono>
#include <vector>

#include <libff/common/profiling.hpp>
#include <libff/common/utils.hpp>
#include <libff/algebra/curves/alt_bn128/alt_bn128_pp.hpp>

#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.hpp>

using namespace libsnark;

/* ======================================================================== */
/* Circuit factories                                                        */
/* ======================================================================== */

/** s_out = s_in * t — 1 constraint, 3 variables */
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

/** (a_out, b_out) = (a_in * t, b_in * t) — 2 constraints, 5 variables */
template<typename FieldT>
state_transition_circuit<FieldT> make_two_state_mult()
{
    r1cs_constraint_system<FieldT> cs;
    cs.primary_input_size = 3;
    cs.auxiliary_input_size = 2;
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(1, FieldT::one());
        B.add_term(3, FieldT::one());
        C.add_term(4, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    {
        linear_combination<FieldT> A, B, C;
        A.add_term(2, FieldT::one());
        B.add_term(3, FieldT::one());
        C.add_term(5, FieldT::one());
        cs.add_constraint(r1cs_constraint<FieldT>(A, B, C));
    }
    return state_transition_circuit<FieldT>(cs, 2, 1);
}

/** Generate execution trace for multiplier: s_j = s_0 * prod(t_k) */
template<typename FieldT>
void make_multiplier_trace(
    size_t num_steps,
    std::vector<std::vector<FieldT> > &states,
    std::vector<std::vector<FieldT> > &transitions,
    std::vector<std::vector<FieldT> > &witnesses)
{
    states.clear(); transitions.clear(); witnesses.clear();

    FieldT s = FieldT(2); /* s_0 */
    states.push_back({s});

    for (size_t k = 1; k <= num_steps; ++k) {
        FieldT t = FieldT(3); /* constant transition for simplicity */
        transitions.push_back({t});
        s = s * t;
        states.push_back({s});
        witnesses.push_back({}); /* no internal witness */
    }
}

/** Generate execution trace for two-state multiplier */
template<typename FieldT>
void make_two_state_trace(
    size_t num_steps,
    std::vector<std::vector<FieldT> > &states,
    std::vector<std::vector<FieldT> > &transitions,
    std::vector<std::vector<FieldT> > &witnesses)
{
    states.clear(); transitions.clear(); witnesses.clear();

    FieldT a = FieldT(2), b = FieldT(3);
    states.push_back({a, b});

    for (size_t k = 1; k <= num_steps; ++k) {
        FieldT t = FieldT(5);
        transitions.push_back({t});
        a = a * t;
        b = b * t;
        states.push_back({a, b});
        witnesses.push_back({});
    }
}


/* ======================================================================== */
/* Timer utility                                                            */
/* ======================================================================== */

using hrclock = std::chrono::high_resolution_clock;

double elapsed_ms(hrclock::time_point start, hrclock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}


/* ======================================================================== */
/* Benchmark 1: Setup time vs B                                             */
/* ======================================================================== */

template<typename ppT>
void bench_setup()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n================================================================\n");
    printf("Benchmark: UVC.Setup time vs B\n");
    printf("================================================================\n");
    printf("  Measures generator time and CRS size as B grows.\n");
    printf("  Expected: O(B*n) — linear growth in both time and CRS elements.\n");

    auto st = make_multiplier<FieldT>();
    const size_t n = st.wires_per_step();

    printf("  Circuit: multiplier (n=%zu constraints/step)\n\n", st.base_cs.num_constraints());
    printf("  %-6s  %-12s  %-12s  %-10s  %-10s\n",
        "B", "Total vars", "Constraints", "Time (ms)", "CRS G1+G2");
    printf("  %-6s  %-12s  %-12s  %-10s  %-10s\n",
        "------", "------------", "------------", "----------", "----------");

    size_t B_values[] = {1, 2, 5, 10, 20, 50};
    for (size_t B : B_values)
    {
        size_t total_vars = n + (B - 1) * st.num_new_wires_per_step();
        size_t total_constraints = B * st.base_cs.num_constraints();

        auto t0 = hrclock::now();
        auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);
        auto t1 = hrclock::now();

        size_t crs_g1 = kp.pk.base_pk.A_query.size() + kp.pk.base_pk.L_query.size() +
                         kp.pk.base_pk.H_query.size() + 2; /* alpha_g1, beta_g1 */
        size_t crs_g2 = kp.pk.base_pk.B_query.domain_size_ + 2; /* beta_g2, delta_g2 */
        for (const auto &sd : kp.pk.step_data) {
            crs_g1 += sd.A_query_delta.size() + sd.L_query_delta.size();
            crs_g2 += sd.B_query_delta.domain_size_;
        }

        printf("  %-6zu  %-12zu  %-12zu  %-10.1f  %zu + %zu\n",
            B, total_vars, total_constraints,
            elapsed_ms(t0, t1), crs_g1, crs_g2);
    }
}


/* ======================================================================== */
/* Benchmark 2: Prover time — base vs incremental                           */
/* ======================================================================== */

template<typename ppT>
void bench_prover()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n================================================================\n");
    printf("Benchmark: UVC.Prove time — base vs incremental\n");
    printf("================================================================\n");
    printf("  Compares step-1 (base) vs step j>1 (incremental) proving time.\n");
    printf("  Expected: incremental is faster — only adds delta for new wires.\n");

    auto st = make_multiplier<FieldT>();
    const size_t B = 20;

    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    make_multiplier_trace(B, states, transitions, witnesses);

    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

    printf("  Circuit: multiplier, B=%zu\n\n", B);
    printf("  %-6s  %-8s  %-12s  %-12s\n",
        "Step", "Type", "Time (ms)", "h coeff len");
    printf("  %-6s  %-8s  %-12s  %-12s\n",
        "------", "--------", "------------", "------------");

    r1cs_uvc_ppzksnark_proof<ppT> prev;
    bool have_prev = false;

    for (size_t step = 1; step <= B; ++step)
    {
        auto assign = build_composed_assignment(st, step, states, transitions, witnesses);

        auto t0 = hrclock::now();
        auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, step, assign.first, assign.second,
            have_prev ? &prev : nullptr);
        auto t1 = hrclock::now();

        printf("  %-6zu  %-8s  %-12.2f  %-12zu\n",
            step, step == 1 ? "base" : "incr",
            elapsed_ms(t0, t1), proof.cached_h_coefficients.size());

        prev = proof;
        have_prev = true;
    }
}


/* ======================================================================== */
/* Benchmark 3: Verifier time (should be constant across steps)             */
/* ======================================================================== */

template<typename ppT>
void bench_verifier()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n================================================================\n");
    printf("Benchmark: UVC.Verify time (constant across steps)\n");
    printf("================================================================\n");
    printf("  Measures verification time at each step (averaged over 5 runs).\n");
    printf("  Expected: ~constant — 3 pairings + input accumulation, independent of j.\n");

    auto st = make_multiplier<FieldT>();
    const size_t B = 10;

    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    make_multiplier_trace(B, states, transitions, witnesses);

    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

    /* Generate all proofs */
    std::vector<r1cs_uvc_ppzksnark_proof<ppT> > proofs;
    std::vector<r1cs_uvc_ppzksnark_primary_input<ppT> > primaries;

    r1cs_uvc_ppzksnark_proof<ppT> prev;
    bool have_prev = false;
    for (size_t step = 1; step <= B; ++step)
    {
        auto assign = build_composed_assignment(st, step, states, transitions, witnesses);
        auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, step, assign.first, assign.second,
            have_prev ? &prev : nullptr);
        proofs.push_back(proof);
        primaries.push_back(assign.first);
        prev = proof;
        have_prev = true;
    }

    printf("  Circuit: multiplier, B=%zu\n\n", B);
    printf("  %-6s  %-12s  %-8s\n", "Step", "Time (ms)", "Result");
    printf("  %-6s  %-12s  %-8s\n", "------", "------------", "--------");

    for (size_t i = 0; i < proofs.size(); ++i)
    {
        /* Average over multiple runs for more stable timing */
        const size_t runs = 5;
        double total_ms = 0;
        bool result = false;
        for (size_t r = 0; r < runs; ++r)
        {
            auto t0 = hrclock::now();
            result = r1cs_uvc_ppzksnark_verifier<ppT>(kp.vk, i + 1, primaries[i], proofs[i]);
            auto t1 = hrclock::now();
            total_ms += elapsed_ms(t0, t1);
        }
        printf("  %-6zu  %-12.2f  %-8s\n",
            i + 1, total_ms / runs, result ? "PASS" : "FAIL");
    }
}


/* ======================================================================== */
/* Benchmark 4: Proof size (constant across steps)                          */
/* ======================================================================== */

template<typename ppT>
void bench_proof_size()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n================================================================\n");
    printf("Benchmark: Proof size (constant across steps and B)\n");
    printf("================================================================\n");
    printf("  Reports proof size at various steps and B values.\n");
    printf("  Expected: always 2 G1 + 1 G2 elements (same as Groth16).\n");

    auto st = make_multiplier<FieldT>();

    printf("\n  %-6s  %-6s  %-8s  %-8s  %-12s\n",
        "B", "Step", "G1 elts", "G2 elts", "Size (bits)");
    printf("  %-6s  %-6s  %-8s  %-8s  %-12s\n",
        "------", "------", "--------", "--------", "------------");

    size_t B_values[] = {1, 5, 10};
    for (size_t B : B_values)
    {
        std::vector<std::vector<FieldT> > states, transitions, witnesses;
        make_multiplier_trace(B, states, transitions, witnesses);

        auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);

        r1cs_uvc_ppzksnark_proof<ppT> prev;
        bool have_prev = false;
        for (size_t step = 1; step <= B; ++step)
        {
            auto assign = build_composed_assignment(st, step, states, transitions, witnesses);
            auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
                kp.pk, step, assign.first, assign.second,
                have_prev ? &prev : nullptr);

            printf("  %-6zu  %-6zu  %-8zu  %-8zu  %-12zu\n",
                B, step, proof.G1_size(), proof.G2_size(), proof.size_in_bits());

            prev = proof;
            have_prev = true;
        }
    }
}


/* ======================================================================== */
/* Benchmark 5: Scaling comparison — multiplier vs two-state                */
/* ======================================================================== */

template<typename ppT>
void bench_circuit_comparison()
{
    typedef libff::Fr<ppT> FieldT;
    printf("\n================================================================\n");
    printf("Benchmark: Circuit size comparison\n");
    printf("================================================================\n");
    printf("  Compares multiplier (n=3) vs two-state (n=5) circuit performance.\n");
    printf("  Expected: larger circuit → proportionally more work per step.\n");

    const size_t B = 10;

    /* Multiplier: n=3, 1 constraint/step */
    {
        auto st = make_multiplier<FieldT>();
        std::vector<std::vector<FieldT> > states, transitions, witnesses;
        make_multiplier_trace(B, states, transitions, witnesses);

        auto t0 = hrclock::now();
        auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);
        auto t1 = hrclock::now();

        auto assign1 = build_composed_assignment(st, (size_t)1, states, transitions, witnesses);
        auto t2 = hrclock::now();
        auto proof1 = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, 1, assign1.first, assign1.second, nullptr);
        auto t3 = hrclock::now();

        auto assign_B_half = build_composed_assignment(st, B/2, states, transitions, witnesses);
        auto t4 = hrclock::now();
        /* Build up to step B/2 incrementally */
        r1cs_uvc_ppzksnark_proof<ppT> prev = proof1;
        r1cs_uvc_ppzksnark_proof<ppT> last_inc;
        for (size_t s = 2; s <= B/2; ++s)
        {
            auto assign = build_composed_assignment(st, s, states, transitions, witnesses);
            last_inc = r1cs_uvc_ppzksnark_prover<ppT>(
                kp.pk, s, assign.first, assign.second, &prev);
            prev = last_inc;
        }
        auto t5 = hrclock::now();

        printf("\n  Multiplier (n=3, 1 constraint/step), B=%zu\n", B);
        printf("    Setup:           %.1f ms\n", elapsed_ms(t0, t1));
        printf("    Prove (base):    %.2f ms\n", elapsed_ms(t2, t3));
        printf("    Prove (incr x%zu): %.2f ms total, %.2f ms/step avg\n",
            B/2 - 1, elapsed_ms(t4, t5),
            elapsed_ms(t4, t5) / (B/2 - 1));
    }

    /* Two-state: n=5, 2 constraints/step */
    {
        auto st = make_two_state_mult<FieldT>();
        std::vector<std::vector<FieldT> > states, transitions, witnesses;
        make_two_state_trace(B, states, transitions, witnesses);

        auto t0 = hrclock::now();
        auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, B);
        auto t1 = hrclock::now();

        auto assign1 = build_composed_assignment(st, (size_t)1, states, transitions, witnesses);
        auto t2 = hrclock::now();
        auto proof1 = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, 1, assign1.first, assign1.second, nullptr);
        auto t3 = hrclock::now();

        r1cs_uvc_ppzksnark_proof<ppT> prev = proof1;
        r1cs_uvc_ppzksnark_proof<ppT> last_inc;
        auto t4 = hrclock::now();
        for (size_t s = 2; s <= B/2; ++s)
        {
            auto assign = build_composed_assignment(st, s, states, transitions, witnesses);
            last_inc = r1cs_uvc_ppzksnark_prover<ppT>(
                kp.pk, s, assign.first, assign.second, &prev);
            prev = last_inc;
        }
        auto t5 = hrclock::now();

        printf("\n  Two-state mult (n=5, 2 constraints/step), B=%zu\n", B);
        printf("    Setup:           %.1f ms\n", elapsed_ms(t0, t1));
        printf("    Prove (base):    %.2f ms\n", elapsed_ms(t2, t3));
        printf("    Prove (incr x%zu): %.2f ms total, %.2f ms/step avg\n",
            B/2 - 1, elapsed_ms(t4, t5),
            elapsed_ms(t4, t5) / (B/2 - 1));
    }
}


/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */

int main()
{
    libff::alt_bn128_pp::init_public_params();
    libff::inhibit_profiling_info = true;

    bench_setup<libff::alt_bn128_pp>();
    bench_prover<libff::alt_bn128_pp>();
    bench_verifier<libff::alt_bn128_pp>();
    bench_proof_size<libff::alt_bn128_pp>();
    bench_circuit_comparison<libff::alt_bn128_pp>();

    printf("\n================================================================\n");
    printf("All benchmarks complete.\n");
    printf("================================================================\n");
    return 0;
}
