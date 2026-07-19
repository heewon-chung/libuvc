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
    size_t reps;            /* timing repetitions */
    bool verify_measured_only = false; /* pre-check only measured steps */
};

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
    std::string uvc_path = cfg.output_dir + "/uvc_bound_results.csv";
    FILE *csv_check = fopen(uvc_path.c_str(), "r");
    bool write_header = (csv_check == nullptr);
    if (csv_check) fclose(csv_check);

    FILE *csv = fopen(uvc_path.c_str(), "a");
    if (!csv) { fprintf(stderr, "Cannot open %s\n", uvc_path.c_str()); return; }
    if (write_header) {
        fprintf(csv, "scheme,circuit,n,B,step,setup_ms,prove_ms,verify_ms,crs_g1_published,crs_g2,vk_st_abc_g1,proof_bytes,proof_bytes_compressed,peak_mem_mb\n");
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

        const size_t proof_bytes_compressed =
            32 * proofs[j].G1_size() + 64 * proofs[j].G2_size();
        fprintf(csv, "uvc_bound,%s,%zu,%zu,%zu,%.2f,%.2f,%.2f,%zu,%zu,%zu,%zu,%zu,%.1f\n",
            cfg.circuit.c_str(), cfg.n, cfg.B, j,
            setup_ms, prove_stats.median, verify_stats.median,
            crs_g1, crs_g2, kp.vk.st_ABC_g1.size(), proof_bytes,
            proof_bytes_compressed, peak_mem_mb);
    }

    fclose(csv);
    printf("\n  UVC results written to %s\n", uvc_path.c_str());
}


/* ======================================================================== */
/* Groth16 Re-prove Benchmark                                                */
/* ======================================================================== */

void bench_groth16(const BenchConfig &cfg)
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

        /* Build composed circuit C_j */
        auto cs_j = build_composed_constraint_system(st, j);
        size_t num_constraints = cs_j.num_constraints();

        /* Build assignment */
        auto assign = build_assignment_for_step(st, j, states, transitions, witnesses);

        /* Setup (1 run — expensive for large j) */
        bench::BenchTimer setup_timer;
        setup_timer.start();
        auto kp = r1cs_gg_ppzksnark_generator<ppT>(cs_j);
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
        bool verified = false;
        for (size_t r = 0; r < cfg.reps; ++r)
        {
            bench::BenchTimer t;
            t.start();
            verified = r1cs_gg_ppzksnark_verifier_strong_IC<ppT>(
                kp.vk, assign.first, proof);
            t.stop();
            verify_times.push_back(t.elapsed_ms());
        }
        auto verify_stats = bench::compute_stats(verify_times);

        size_t proof_bytes = (proof.size_in_bits() + 7) / 8;

        printf("  %-6zu  %-10zu  %-12.1f  %-12.2f  %-12.2f  %-10s\n",
            j, num_constraints, setup_ms, prove_stats.median, verify_stats.median,
            verified ? "PASS" : "FAIL");

        fprintf(csv, "%s,%zu,%zu,%.2f,%.2f,%.2f,%zu,%zu,%zu\n",
            cfg.circuit.c_str(), cfg.n, j,
            setup_ms, prove_stats.median, verify_stats.median,
            g16_crs_g1, g16_crs_g2, proof_bytes);
    }

    fclose(csv);
    printf("\n  Groth16 results written to %s\n", g16_path.c_str());
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
    cfg.reps = 5;

    for (int i = 1; i < argc; ++i)
    {
        if ((strcmp(argv[i], "--circuit") == 0 ||
             strcmp(argv[i], "--n") == 0 ||
             strcmp(argv[i], "--B") == 0 ||
             strcmp(argv[i], "--output-dir") == 0 ||
             strcmp(argv[i], "--reps") == 0) &&
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
        else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc)
            cfg.reps = (size_t)atol(argv[++i]);
        else if (strcmp(argv[i], "--verify-measured-only") == 0)
            cfg.verify_measured_only = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --circuit {mimc|hadamard|scalable|pagerank|sensor_fusion} Circuit type (default: mimc)\n");
            printf("  --n {size}                        Constraints per step (default: 270)\n");
            printf("                                    For pagerank: --n is number of nodes (constraints = 2*n)\n");
            printf("                                    For sensor_fusion: --n is number of sensors K (constraints = K+1)\n");
            printf("  --B {bound}                       Max compositions (default: 16)\n");
            printf("  --output-dir {path}               CSV output directory (default: .)\n");
            printf("  --reps {count}                    Timing repetitions (default: 5)\n");
            printf("  --verify-measured-only             Pre-check only measured UVC proof steps\n");
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

    BenchConfig cfg = parse_args(argc, argv);

    printf("================================================================\n");
    printf("Comparative Benchmark: UVC vs Groth16\n");
    printf("================================================================\n");
    printf("  Circuit: %s, n=%zu, B=%zu, reps=%zu\n",
        cfg.circuit.c_str(), cfg.n, cfg.B, cfg.reps);
    printf("  Output: %s/\n", cfg.output_dir.c_str());

    bench_uvc(cfg);
    bench_groth16(cfg);

    printf("\n================================================================\n");
    printf("All benchmarks complete.\n");
    printf("================================================================\n");

    return 0;
}
