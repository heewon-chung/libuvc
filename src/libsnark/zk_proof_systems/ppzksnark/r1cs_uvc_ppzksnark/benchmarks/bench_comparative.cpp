/** @file
*****************************************************************************

Comparative benchmark: UVC vs Groth16 re-prove baseline.

Measures setup, prove, and verify times for both schemes across multiple
circuit types and composition steps.

Usage:
  ./bench_comparative --circuit {mimc|hadamard|scalable} --n {size} --B {bound}
                      --output-dir {path} [--reps {count}]

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>

#ifdef MULTICORE
#include <omp.h>
#endif

#include <libff/common/profiling.hpp>
#include <libff/common/utils.hpp>
#include <libff/algebra/curves/alt_bn128/alt_bn128_pp.hpp>

#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_gg_ppzksnark/r1cs_gg_ppzksnark.hpp>

#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/mimc_circuit.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/hadamard_circuit.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/scalable_circuit.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/pagerank_circuit.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/circuits/sensor_fusion_circuit.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/bench_utils.hpp>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/groth16_baseline_circuits.hpp>

using namespace libsnark;

using ppT = libff::alt_bn128_pp;
using FieldT = libff::Fr<ppT>;

/* ======================================================================== */
/* Configuration                                                             */
/* ======================================================================== */

struct BenchConfig {
    std::string circuit;    /* "mimc", "hadamard", "scalable" */
    size_t n;               /* constraints per step */
    size_t B;               /* max compositions */
    std::string output_dir; /* CSV output directory */
    std::string commit;     /* short git commit hash */
    size_t reps;            /* timing repetitions */
    bool verify_measured_only = false; /* pre-check only measured steps */
    std::string scheme = "both"; /* see parse_args for the accepted values */
    bool self_test = false;      /* run the correctness self-test and exit */
};

/* ======================================================================== */
/* groth16_modes_results.csv writer                                          */
/* ======================================================================== */

/** Append one row to groth16_modes_results.csv, writing the header if absent. */
void write_groth16_mode_row(
    const BenchConfig &cfg, const char *mode, size_t step,
    double setup_ms, const bench::BenchStats &prove_stats,
    const bench::BenchStats &verify_stats,
    size_t crs_g1, size_t crs_g2, size_t proof_bytes,
    size_t num_constraints, size_t num_primary)
{
    const std::string path = cfg.output_dir + "/groth16_modes_results.csv";
    FILE *csv_check = fopen(path.c_str(), "r");
    const bool write_header = (csv_check == nullptr);
    if (csv_check) fclose(csv_check);

    FILE *csv = fopen(path.c_str(), "a");
    if (!csv) { fprintf(stderr, "Cannot open %s\n", path.c_str()); return; }
    if (write_header) {
        fprintf(csv, "mode,circuit,n,B,step,setup_ms,prove_ms,prove_ms_mean,"
                     "prove_ms_stddev,prove_ms_min,prove_ms_max,verify_ms,"
                     "verify_ms_stddev,crs_g1,crs_g2,proof_bytes,"
                     "num_constraints,num_primary,commit\n");
    }
    fprintf(csv, "%s,%s,%zu,%zu,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                 "%zu,%zu,%zu,%zu,%zu,%s\n",
        mode, cfg.circuit.c_str(), cfg.n, cfg.B, step,
        setup_ms, prove_stats.median, prove_stats.mean, prove_stats.stddev,
        prove_stats.min_val, prove_stats.max_val,
        verify_stats.median, verify_stats.stddev,
        crs_g1, crs_g2, proof_bytes, num_constraints, num_primary,
        cfg.commit.c_str());
    fclose(csv);
}

/* ======================================================================== */
/* Circuit and trace factories                                               */
/* ======================================================================== */

state_transition_circuit<FieldT> create_circuit(const std::string &type, size_t n)
{
    if (type == "mimc") {
        size_t rounds = n / 3;
        if (rounds < 1) rounds = 1;
        return make_mimc_circuit<FieldT>(rounds);
    } else if (type == "hadamard") {
        return make_hadamard_circuit<FieldT>(n);
    } else if (type == "scalable") {
        return make_scalable_circuit<FieldT>(n);
    } else if (type == "pagerank") {
        return make_pagerank_circuit<FieldT>(n); /* n = number of nodes, constraints = 2*n */
    } else if (type == "sensor_fusion") {
        return make_sensor_fusion_circuit<FieldT>(n); /* n = K sensors, constraints = K+1 */
    }
    fprintf(stderr, "Unknown circuit type: %s\n", type.c_str());
    exit(1);
}

void create_trace(
    const std::string &type, size_t n, size_t num_steps,
    std::vector<std::vector<FieldT> > &states,
    std::vector<std::vector<FieldT> > &transitions,
    std::vector<std::vector<FieldT> > &witnesses)
{
    if (type == "mimc") {
        make_mimc_trace<FieldT>(num_steps, n / 3, states, transitions, witnesses);
    } else if (type == "hadamard") {
        make_hadamard_trace<FieldT>(num_steps, n, states, transitions, witnesses);
    } else if (type == "scalable") {
        make_scalable_trace<FieldT>(num_steps, n, states, transitions, witnesses);
    } else if (type == "pagerank") {
        make_pagerank_trace<FieldT>(num_steps, n, states, transitions, witnesses);
    } else if (type == "sensor_fusion") {
        make_sensor_fusion_trace<FieldT>(num_steps, n, states, transitions, witnesses);
    }
}

/* ======================================================================== */
/* Helpers                                                                   */
/* ======================================================================== */

/** Generate measured steps: powers of 2 up to B, plus B itself */
std::vector<size_t> get_measured_steps(size_t B)
{
    std::vector<size_t> steps;
    for (size_t s = 1; s <= B; s *= 2)
        steps.push_back(s);
    if (steps.back() != B)
        steps.push_back(B);
    return steps;
}

/** Safely call build_composed_assignment with correctly-sized vectors */
std::pair<r1cs_primary_input<FieldT>, r1cs_auxiliary_input<FieldT> >
build_assignment_for_step(
    const state_transition_circuit<FieldT> &st, size_t j,
    const std::vector<std::vector<FieldT> > &states,
    const std::vector<std::vector<FieldT> > &transitions,
    const std::vector<std::vector<FieldT> > &witnesses)
{
    std::vector<std::vector<FieldT> > s(states.begin(), states.begin() + j + 1);
    std::vector<std::vector<FieldT> > t(transitions.begin(), transitions.begin() + j);
    std::vector<std::vector<FieldT> > w(witnesses.begin(), witnesses.begin() + j);
    return build_composed_assignment(st, j, s, t, w);
}


/* ======================================================================== */
/* UVC Benchmark                                                             */
/* ======================================================================== */

void bench_uvc(const BenchConfig &cfg)
{
    printf("\n================================================================\n");
    printf("UVC (state-bound) Benchmark: circuit=%s, n=%zu, B=%zu\n",
        cfg.circuit.c_str(), cfg.n, cfg.B);
    printf("================================================================\n");

    auto st = create_circuit(cfg.circuit, cfg.n);
    printf("  Circuit: %zu constraints, %zu vars, state=%zu, transition=%zu\n",
        st.base_cs.num_constraints(), st.wires_per_step(),
        st.state_size, st.transition_size);

    /* Generate trace for B steps */
    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    create_trace(cfg.circuit, cfg.n, cfg.B, states, transitions, witnesses);

    /* --- Setup --- */
    printf("\n  [Setup] Generating CRS for B=%zu...\n", cfg.B);
    bench::BenchTimer setup_timer;
    setup_timer.start();
    auto kp = r1cs_uvc_ppzksnark_generator<ppT>(st, cfg.B);
    setup_timer.stop();
    double setup_ms = setup_timer.elapsed_ms();

    /* CRS size */
    size_t crs_g1 = kp.pk.base_pk.A_query.size() + kp.pk.base_pk.H_query.size() +
                    (kp.pk.base_pk.L_query.size() - cfg.B * st.state_size) +
                    kp.pk.st_query.size() + 2;
    size_t crs_g2 = kp.pk.base_pk.B_query.domain_size_ + 3;
    for (const auto &sd : kp.pk.step_data) {
        crs_g1 += sd.A_query_delta.size() + sd.L_query_delta.size() +
                  sd.st_query_delta.size();
        crs_g2 += sd.B_query_delta.domain_size_;
    }
    printf("  Setup: %.1f ms, CRS: %zu G1 + %zu G2\n", setup_ms, crs_g1, crs_g2);

    /* --- Build full proof chain --- */
    printf("  Building proof chain (steps 1..%zu)...\n", cfg.B);
    std::vector<r1cs_uvc_ppzksnark_proof<ppT> > proofs(cfg.B + 1);
    std::vector<std::pair<r1cs_primary_input<FieldT>, r1cs_auxiliary_input<FieldT> > > assigns(cfg.B + 1);

    for (size_t j = 1; j <= cfg.B; ++j)
    {
        assigns[j] = build_assignment_for_step(st, j, states, transitions, witnesses);
        proofs[j] = r1cs_uvc_ppzksnark_prover<ppT>(
            kp.pk, j, assigns[j].first, assigns[j].second,
            j > 1 ? &proofs[j-1] : nullptr);
    }

    auto measured = get_measured_steps(cfg.B);

    /* --- Untimed honest-chain verification --- */
    printf("  Verifying honest proof chain...\n");
    for (size_t j = 1; j <= cfg.B; ++j) {
        if (cfg.verify_measured_only &&
            std::find(measured.begin(), measured.end(), j) == measured.end()) {
            continue;
        }

        const bool verified = r1cs_uvc_ppzksnark_verifier<ppT>(
            kp.vk, j, assigns[j].first, states[j], proofs[j],
            j > 1 ? proofs[j-1].g_D : libff::G1<ppT>::zero());
        if (!verified) {
            fprintf(stderr, "Honest proof-chain verification failed at step %zu.\n", j);
            abort();
        }
    }
    printf("  Honest proof-chain verification: PASS\n");

    /* Open CSV */
    std::string uvc_path = cfg.output_dir + "/uvc_results.csv";
    FILE *csv_check = fopen(uvc_path.c_str(), "r");
    bool write_header = (csv_check == nullptr);
    if (csv_check) fclose(csv_check);

    FILE *csv = fopen(uvc_path.c_str(), "a");
    if (!csv) { fprintf(stderr, "Cannot open %s\n", uvc_path.c_str()); return; }
    if (write_header) {
        fprintf(csv, "scheme,circuit,n,B,step,setup_ms,prove_ms,verify_ms,crs_g1_published,crs_g2,vk_st_abc_g1,proof_bytes,proof_bytes_compressed,peak_mem_mb,commit\n");
    }

    printf("\n  %-6s  %-12s  %-12s  %-12s  %-10s\n",
        "Step", "Prove (ms)", "Verify (ms)", "Proof bits", "Verified");
    printf("  %-6s  %-12s  %-12s  %-12s  %-10s\n",
        "------", "------------", "------------", "------------", "----------");

    for (size_t j : measured)
    {
        /* Prove timing: run reps using cached prev_proof */
        std::vector<double> prove_times;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            auto proof = r1cs_uvc_ppzksnark_prover<ppT>(
                kp.pk, j, assigns[j].first, assigns[j].second,
                j > 1 ? &proofs[j-1] : nullptr);
            t.stop();
            prove_times.push_back(t.elapsed_ms());
        }
        auto prove_stats = bench::compute_stats(prove_times);

        /* Verify timing */
        std::vector<double> verify_times;
        bool verified = false;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            verified = r1cs_uvc_ppzksnark_verifier<ppT>(
                kp.vk, j, assigns[j].first, states[j], proofs[j],
                j > 1 ? proofs[j-1].g_D : libff::G1<ppT>::zero());
            t.stop();
            verify_times.push_back(t.elapsed_ms());
        }
        auto verify_stats = bench::compute_stats(verify_times);

        size_t proof_bytes = (proofs[j].size_in_bits() + 7) / 8;
        size_t peak_mem = bench::get_peak_memory_bytes();
        double peak_mem_mb = peak_mem / (1024.0 * 1024.0);

        printf("  %-6zu  %-12.2f  %-12.2f  %-12zu  %-10s\n",
            j, prove_stats.median, verify_stats.median,
            proofs[j].size_in_bits(), verified ? "PASS" : "FAIL");

        const size_t proof_bytes_compressed = 160;
        fprintf(csv, "uvc_gamma_v1,%s,%zu,%zu,%zu,%.2f,%.2f,%.2f,%zu,%zu,%zu,%zu,%zu,%.1f,%s\n",
            cfg.circuit.c_str(), cfg.n, cfg.B, j,
            setup_ms, prove_stats.median, verify_stats.median,
            crs_g1, crs_g2, kp.vk.st_ABC_g1.size(), proof_bytes,
            proof_bytes_compressed, peak_mem_mb, cfg.commit.c_str());
    }

    fclose(csv);
    printf("\n  UVC results written to %s\n", uvc_path.c_str());
}


/* ======================================================================== */
/* Groth16 Re-prove Benchmark                                                */
/* ======================================================================== */

void bench_groth16_ondemand(const BenchConfig &cfg)
{
    printf("\n================================================================\n");
    printf("Groth16 Benchmark: circuit=%s, n=%zu, B=%zu\n", cfg.circuit.c_str(), cfg.n, cfg.B);
    printf("================================================================\n");

    auto st = create_circuit(cfg.circuit, cfg.n);

    /* Generate trace for B steps */
    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    create_trace(cfg.circuit, cfg.n, cfg.B, states, transitions, witnesses);

    /* Limit Groth16 to reasonable step counts for large circuits */
    size_t groth16_max_step = cfg.B;
    if (cfg.n >= 1024 && groth16_max_step > 256) {
        groth16_max_step = 256;
        printf("  Note: Limiting Groth16 to step <= %zu for n=%zu\n", groth16_max_step, cfg.n);
    }
    if (cfg.n >= 4096 && groth16_max_step > 64) {
        groth16_max_step = 64;
        printf("  Note: Limiting Groth16 to step <= %zu for n=%zu\n", groth16_max_step, cfg.n);
    }

    auto measured = get_measured_steps(cfg.B);

    /* Open CSV */
    std::string g16_path = cfg.output_dir + "/groth16_results.csv";
    FILE *csv_check = fopen(g16_path.c_str(), "r");
    bool write_header = (csv_check == nullptr);
    if (csv_check) fclose(csv_check);

    FILE *csv = fopen(g16_path.c_str(), "a");
    if (!csv) { fprintf(stderr, "Cannot open %s\n", g16_path.c_str()); return; }
    if (write_header) {
        fprintf(csv, "circuit,n,step,setup_ms,prove_ms,verify_ms,crs_g1,crs_g2,proof_bytes\n");
    }

    printf("\n  %-6s  %-10s  %-12s  %-12s  %-12s  %-10s\n",
        "Step", "C_j constr", "Setup (ms)", "Prove (ms)", "Verify (ms)", "Verified");
    printf("  %-6s  %-10s  %-12s  %-12s  %-12s  %-10s\n",
        "------", "----------", "------------", "------------", "------------", "----------");

    for (size_t j : measured)
    {
        if (j > groth16_max_step) {
            printf("  %-6zu  (skipped — exceeds limit)\n", j);
            continue;
        }

        /* Build composed circuit C_j with the reported state s_j public */
        auto PJ = build_composed_public_states(st, j, ExposeStates::FinalOnly);
        size_t num_constraints = PJ.cs.num_constraints();

        /* Build assignment */
        auto assign0 = build_assignment_for_step(st, j, states, transitions, witnesses);
        auto assign = permute_assignment(assign0.first, assign0.second,
                                         PJ.perm, PJ.num_public);

        if (!PJ.cs.is_satisfied(assign.first, assign.second)) {
            fprintf(stderr, "ondemand preflight failed at step %zu\n", j);
            abort();
        }

        /* Setup (1 run — expensive for large j) */
        bench::BenchTimer setup_timer;
        setup_timer.start();
        auto kp = r1cs_gg_ppzksnark_generator<ppT>(PJ.cs);
        setup_timer.stop();
        double setup_ms = setup_timer.elapsed_ms();

        /* CRS size */
        size_t g16_crs_g1 = kp.pk.G1_size();
        size_t g16_crs_g2 = kp.pk.G2_size();

        /* Prove timing */
        std::vector<double> prove_times;
        r1cs_gg_ppzksnark_proof<ppT> proof;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            proof = r1cs_gg_ppzksnark_prover<ppT>(
                kp.pk, assign.first, assign.second);
            t.stop();
            prove_times.push_back(t.elapsed_ms());
        }
        auto prove_stats = bench::compute_stats(prove_times);

        /* Verify timing */
        std::vector<double> verify_times;
        bool verified = true;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            const bool rep_verified = r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(
                kp.vk, assign.first, proof);
            verified = verified && rep_verified;
            t.stop();
            verify_times.push_back(t.elapsed_ms());
        }
        auto verify_stats = bench::compute_stats(verify_times);
        if (!verified) {
            fprintf(stderr, "Groth16 verification failed at step %zu.\n", j);
            fclose(csv);
            exit(1);
        }

        size_t proof_bytes = (proof.size_in_bits() + 7) / 8;

        printf("  %-6zu  %-10zu  %-12.1f  %-12.2f  %-12.2f  %-10s\n",
            j, num_constraints, setup_ms, prove_stats.median, verify_stats.median,
            verified ? "PASS" : "FAIL");

        fprintf(csv, "%s,%zu,%zu,%.2f,%.2f,%.2f,%zu,%zu,%zu\n",
            cfg.circuit.c_str(), cfg.n, j,
            setup_ms, prove_stats.median, verify_stats.median,
            g16_crs_g1, g16_crs_g2, proof_bytes);

        write_groth16_mode_row(cfg, "ondemand", j, setup_ms, prove_stats,
            verify_stats, g16_crs_g1, g16_crs_g2, proof_bytes,
            num_constraints, PJ.num_public);
    }

    fclose(csv);
    printf("\n  Groth16 results written to %s\n", g16_path.c_str());
}


/* ======================================================================== */
/* Groth16 fixed-CRS Benchmark (work item B)                                 */
/* ======================================================================== */

void bench_groth16_fixedcrs(const BenchConfig &cfg)
{
    printf("\n================================================================\n");
    printf("Groth16 fixed-CRS Benchmark: circuit=%s, n=%zu, B=%zu\n",
        cfg.circuit.c_str(), cfg.n, cfg.B);
    printf("================================================================\n");

    auto st = create_circuit(cfg.circuit, cfg.n);

    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    create_trace(cfg.circuit, cfg.n, cfg.B, states, transitions, witnesses);

    /* Composed C_B with every reported state s_1..s_B public */
    auto PB = build_composed_public_states(st, cfg.B, ExposeStates::All);

    bench::BenchTimer setup_timer;
    setup_timer.start();
    auto kp = r1cs_gg_ppzksnark_generator<ppT>(PB.cs);
    setup_timer.stop();
    const double setup_ms = setup_timer.elapsed_ms();

    const size_t g16_crs_g1 = kp.pk.G1_size();
    const size_t g16_crs_g2 = kp.pk.G2_size();

    printf("  [fixedcrs] C_B setup: %.2f ms, %zu constraints\n",
        setup_ms, PB.cs.num_constraints());

    auto measured = get_measured_steps(cfg.B);

    printf("\n  %-6s  %-12s  %-12s  %-10s\n",
        "Step", "Prove (ms)", "Verify (ms)", "Verified");
    printf("  %-6s  %-12s  %-12s  %-10s\n",
        "------", "------------", "------------", "----------");

    for (size_t j : measured)
    {
        auto assign = build_padded_composed_assignment(
            st, j, cfg.B, PB.perm, PB.num_public, states, transitions, witnesses);

        if (!PB.cs.is_satisfied(assign.first, assign.second)) {
            fprintf(stderr, "fixedcrs preflight failed at step %zu\n", j);
            abort();
        }

        std::vector<double> prove_times;
        r1cs_gg_ppzksnark_proof<ppT> proof;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            proof = r1cs_gg_ppzksnark_prover<ppT>(kp.pk, assign.first, assign.second);
            t.stop();
            prove_times.push_back(t.elapsed_ms());
        }
        auto prove_stats = bench::compute_stats(prove_times);

        std::vector<double> verify_times;
        bool verified = true;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            const bool rep_verified = r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(
                kp.vk, assign.first, proof);
            t.stop();
            verified = verified && rep_verified;
            verify_times.push_back(t.elapsed_ms());
        }
        auto verify_stats = bench::compute_stats(verify_times);
        if (!verified) {
            fprintf(stderr, "Groth16 fixed-CRS verification failed at step %zu.\n", j);
            abort();
        }

        const size_t proof_bytes = (proof.size_in_bits() + 7) / 8;

        printf("  %-6zu  %-12.2f  %-12.2f  %-10s\n",
            j, prove_stats.median, verify_stats.median, verified ? "PASS" : "FAIL");

        write_groth16_mode_row(cfg, "fixedcrs", j, setup_ms, prove_stats,
            verify_stats, g16_crs_g1, g16_crs_g2, proof_bytes,
            PB.cs.num_constraints(), PB.num_public);
    }

    printf("\n  Groth16 fixed-CRS results written to %s/groth16_modes_results.csv\n",
        cfg.output_dir.c_str());
}


/* ======================================================================== */
/* Groth16 single-step chaining Benchmark (work item A)                      */
/* ======================================================================== */

void bench_groth16_singlestep(const BenchConfig &cfg)
{
    printf("\n================================================================\n");
    printf("Groth16 single-step Benchmark: circuit=%s, n=%zu, B=%zu\n",
        cfg.circuit.c_str(), cfg.n, cfg.B);
    printf("================================================================\n");

    auto st = create_circuit(cfg.circuit, cfg.n);

    std::vector<std::vector<FieldT> > states, transitions, witnesses;
    create_trace(cfg.circuit, cfg.n, cfg.B, states, transitions, witnesses);

    auto cs = make_public_output_circuit(st);

    bench::BenchTimer setup_timer;
    setup_timer.start();
    auto kp = r1cs_gg_ppzksnark_generator<ppT>(cs);
    setup_timer.stop();
    const double setup_ms = setup_timer.elapsed_ms();

    const size_t g16_crs_g1 = kp.pk.G1_size();
    const size_t g16_crs_g2 = kp.pk.G2_size();

    printf("  Setup: %.2f ms, %zu constraints, %zu public inputs\n",
        setup_ms, cs.num_constraints(), cs.num_inputs());

    /* Preflight every step of the chain */
    for (size_t j = 1; j <= cfg.B; ++j)
    {
        auto assign = build_single_step_assignment(st, j, states, transitions, witnesses);
        if (!cs.is_satisfied(assign.first, assign.second)) {
            fprintf(stderr, "singlestep preflight failed at step %zu\n", j);
            abort();
        }
    }

    auto measured = get_measured_steps(cfg.B);

    printf("\n  %-6s  %-12s  %-12s  %-10s\n",
        "Step", "Prove (ms)", "Verify (ms)", "Verified");
    printf("  %-6s  %-12s  %-12s  %-10s\n",
        "------", "------------", "------------", "----------");

    for (size_t j : measured)
    {
        auto assign = build_single_step_assignment(st, j, states, transitions, witnesses);

        std::vector<double> prove_times;
        r1cs_gg_ppzksnark_proof<ppT> proof;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            proof = r1cs_gg_ppzksnark_prover<ppT>(kp.pk, assign.first, assign.second);
            t.stop();
            prove_times.push_back(t.elapsed_ms());
        }
        auto prove_stats = bench::compute_stats(prove_times);

        std::vector<double> verify_times;
        bool verified = true;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            const bool rep_verified = r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(
                kp.vk, assign.first, proof);
            t.stop();
            verified = verified && rep_verified;
            verify_times.push_back(t.elapsed_ms());
        }
        auto verify_stats = bench::compute_stats(verify_times);
        if (!verified) {
            fprintf(stderr, "Groth16 single-step verification failed at step %zu.\n", j);
            abort();
        }

        const size_t proof_bytes = (proof.size_in_bits() + 7) / 8;

        printf("  %-6zu  %-12.2f  %-12.2f  %-10s\n",
            j, prove_stats.median, verify_stats.median, verified ? "PASS" : "FAIL");

        write_groth16_mode_row(cfg, "singlestep", j, setup_ms, prove_stats,
            verify_stats, g16_crs_g1, g16_crs_g2, proof_bytes,
            cs.num_constraints(), cs.num_inputs());
    }

    printf("\n  Groth16 single-step results written to %s/groth16_modes_results.csv\n",
        cfg.output_dir.c_str());
}


/* ======================================================================== */
/* Self-test (spec §10.1)                                                    */
/* ======================================================================== */

/**
 * Correctness self-test of the three Groth16 baseline circuit constructions.
 * For each circuit type at tiny size, B = 3 and j in {1,2,3}:
 *   honest assignment satisfies the circuit and verifies;
 *   a corrupted public state coordinate makes verification fail.
 * Prints SELF-TEST PASS and returns 0, or the first failing case and returns 1.
 */
int run_self_test()
{
    struct Case { const char *circuit; size_t n; };
    const std::vector<Case> cases = {
        {"mimc", 6}, {"hadamard", 4}, {"scalable", 8},
        {"pagerank", 4}, {"sensor_fusion", 4}
    };
    const size_t B = 3;

    for (const Case &tc : cases)
    {
        printf("  [self-test] circuit=%s n=%zu B=%zu\n", tc.circuit, tc.n, B);

        auto st = create_circuit(tc.circuit, tc.n);
        const size_t ss = st.state_size;
        const size_t ts = st.transition_size;

        std::vector<std::vector<FieldT> > states, transitions, witnesses;
        create_trace(tc.circuit, tc.n, B, states, transitions, witnesses);

        /* --- 1. Single-step, s_out public --- */
        auto cs_ss = make_public_output_circuit(st);
        auto kp_ss = r1cs_gg_ppzksnark_generator<ppT>(cs_ss);
        for (size_t j = 1; j <= B; ++j)
        {
            auto assign = build_single_step_assignment(st, j, states, transitions, witnesses);
            if (!cs_ss.is_satisfied(assign.first, assign.second)) {
                fprintf(stderr, "SELF-TEST FAIL: singlestep is_satisfied, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
            auto proof = r1cs_gg_ppzksnark_prover<ppT>(kp_ss.pk, assign.first, assign.second);
            if (!r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(kp_ss.vk, assign.first, proof)) {
                fprintf(stderr, "SELF-TEST FAIL: singlestep honest verify, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
            auto bad = assign.first;
            bad.back() += FieldT::one();
            if (r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(kp_ss.vk, bad, proof)) {
                fprintf(stderr, "SELF-TEST FAIL: singlestep corrupted verify accepted, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
        }

        /* --- 2. On-demand C_j, s_j public --- */
        for (size_t j = 1; j <= B; ++j)
        {
            auto PJ = build_composed_public_states(st, j, ExposeStates::FinalOnly);
            auto assign0 = build_assignment_for_step(st, j, states, transitions, witnesses);
            auto assign = permute_assignment(assign0.first, assign0.second,
                                             PJ.perm, PJ.num_public);
            if (!PJ.cs.is_satisfied(assign.first, assign.second)) {
                fprintf(stderr, "SELF-TEST FAIL: ondemand is_satisfied, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
            auto kp_j = r1cs_gg_ppzksnark_generator<ppT>(PJ.cs);
            auto proof = r1cs_gg_ppzksnark_prover<ppT>(kp_j.pk, assign.first, assign.second);
            if (!r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(kp_j.vk, assign.first, proof)) {
                fprintf(stderr, "SELF-TEST FAIL: ondemand honest verify, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
            auto bad = assign.first;
            bad.back() += FieldT::one();
            if (r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(kp_j.vk, bad, proof)) {
                fprintf(stderr, "SELF-TEST FAIL: ondemand corrupted verify accepted, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
        }

        /* --- 3. Fixed-CRS C_B, all states public --- */
        auto PB = build_composed_public_states(st, B, ExposeStates::All);
        auto kp_B = r1cs_gg_ppzksnark_generator<ppT>(PB.cs);
        for (size_t j = 1; j <= B; ++j)
        {
            auto assign = build_padded_composed_assignment(
                st, j, B, PB.perm, PB.num_public, states, transitions, witnesses);
            if (!PB.cs.is_satisfied(assign.first, assign.second)) {
                fprintf(stderr, "SELF-TEST FAIL: fixedcrs is_satisfied, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
            auto proof = r1cs_gg_ppzksnark_prover<ppT>(kp_B.pk, assign.first, assign.second);
            if (!r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(kp_B.vk, assign.first, proof)) {
                fprintf(stderr, "SELF-TEST FAIL: fixedcrs honest verify, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
            auto bad = assign.first;
            bad[ss + ts + (j - 1) * ss] += FieldT::one();
            if (r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(kp_B.vk, bad, proof)) {
                fprintf(stderr, "SELF-TEST FAIL: fixedcrs corrupted verify accepted, circuit=%s j=%zu\n", tc.circuit, j);
                return 1;
            }
        }
    }

    printf("SELF-TEST PASS\n");
    return 0;
}


/* ======================================================================== */
/* Command-line parsing                                                      */
/* ======================================================================== */

BenchConfig parse_args(int argc, char *argv[])
{
    BenchConfig cfg;
    cfg.circuit = "mimc";
    cfg.n = 270;
    cfg.B = 16;
    cfg.output_dir = ".";
    cfg.commit = "unknown";
    cfg.reps = 5;

    for (int i = 1; i < argc; ++i)
    {
        if ((strcmp(argv[i], "--circuit") == 0 ||
             strcmp(argv[i], "--n") == 0 ||
             strcmp(argv[i], "--B") == 0 ||
             strcmp(argv[i], "--output-dir") == 0 ||
             strcmp(argv[i], "--commit") == 0 ||
             strcmp(argv[i], "--reps") == 0 ||
             strcmp(argv[i], "--scheme") == 0) &&
            (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0)) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            exit(1);
        }
        if (strcmp(argv[i], "--circuit") == 0 && i + 1 < argc)
            cfg.circuit = argv[++i];
        else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc)
            cfg.n = (size_t)atol(argv[++i]);
        else if (strcmp(argv[i], "--B") == 0 && i + 1 < argc)
            cfg.B = (size_t)atol(argv[++i]);
        else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc)
            cfg.output_dir = argv[++i];
        else if (strcmp(argv[i], "--commit") == 0 && i + 1 < argc)
            cfg.commit = argv[++i];
        else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc)
            cfg.reps = (size_t)atol(argv[++i]);
        else if (strcmp(argv[i], "--scheme") == 0 && i + 1 < argc)
            cfg.scheme = argv[++i];
        else if (strcmp(argv[i], "--verify-measured-only") == 0)
            cfg.verify_measured_only = true;
        else if (strcmp(argv[i], "--self-test") == 0)
            cfg.self_test = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --circuit {mimc|hadamard|scalable|pagerank|sensor_fusion} Circuit type (default: mimc)\n");
            printf("  --n {size}                        Constraints per step (default: 270)\n");
            printf("                                    For pagerank: --n is number of nodes (constraints = 2*n)\n");
            printf("                                    For sensor_fusion: --n is number of sensors K (constraints = K+1)\n");
            printf("  --B {bound}                       Max compositions (default: 16)\n");
            printf("  --output-dir {path}               CSV output directory (default: .)\n");
            printf("  --commit {hash}                   Build commit (default: unknown)\n");
            printf("  --reps {count}                    Timing repetitions (default: 5)\n");
            printf("  --scheme {uvc|groth16|groth16-fixedcrs|groth16-singlestep|groth16-all|both|all}\n");
            printf("                                    Scheme(s) to benchmark (default: both)\n");
            printf("  --verify-measured-only             Pre-check only measured UVC proof steps\n");
            printf("  --self-test                        Run the baseline-circuit correctness self-test and exit\n");
            exit(0);
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            exit(1);
        }
    }

    /* Normalize circuit alias */
    if (cfg.circuit == "matmul")
        cfg.circuit = "hadamard";
    if (cfg.scheme != "uvc" && cfg.scheme != "groth16" &&
        cfg.scheme != "groth16-fixedcrs" && cfg.scheme != "groth16-singlestep" &&
        cfg.scheme != "groth16-all" && cfg.scheme != "both" && cfg.scheme != "all") {
        fprintf(stderr, "Invalid --scheme value: %s\n", cfg.scheme.c_str());
        exit(1);
    }

    return cfg;
}


/* ======================================================================== */
/* Main                                                                      */
/* ======================================================================== */

int main(int argc, char *argv[])
{
    ppT::init_public_params();
    libff::inhibit_profiling_info = true;
    libff::inhibit_profiling_counters = true;

#ifdef MULTICORE
    printf("  OpenMP max threads: %d\n", omp_get_max_threads());
#else
    printf("  OpenMP max threads: 1 (MULTICORE off)\n");
#endif

    BenchConfig cfg = parse_args(argc, argv);

    if (cfg.self_test) {
        return run_self_test();
    }

    printf("================================================================\n");
    printf("Comparative Benchmark: UVC vs Groth16\n");
    printf("================================================================\n");
    printf("  Circuit: %s, n=%zu, B=%zu, reps=%zu\n",
        cfg.circuit.c_str(), cfg.n, cfg.B, cfg.reps);
    printf("  Output: %s/\n", cfg.output_dir.c_str());

    if (cfg.scheme == "uvc" || cfg.scheme == "both" || cfg.scheme == "all") {
        bench_uvc(cfg);
    }
    if (cfg.scheme == "groth16" || cfg.scheme == "both" ||
        cfg.scheme == "groth16-all" || cfg.scheme == "all") {
        bench_groth16_ondemand(cfg);
    }
    if (cfg.scheme == "groth16-fixedcrs" ||
        cfg.scheme == "groth16-all" || cfg.scheme == "all") {
        bench_groth16_fixedcrs(cfg);
    }
    if (cfg.scheme == "groth16-singlestep" ||
        cfg.scheme == "groth16-all" || cfg.scheme == "all") {
        bench_groth16_singlestep(cfg);
    }

    printf("\n================================================================\n");
    printf("All benchmarks complete.\n");
    printf("================================================================\n");

    return 0;
}
