//! Nova folding-scheme benchmarks for comparison with UVC.
//!
//! Usage:
//!   cargo run --release -- --circuit {mimc|hadamard|scalable|pagerank} --n {size} --steps {count}

mod circuits;

use clap::Parser;
use ff::Field;
use nova_snark::{
    nova::{CompressedSNARK, PublicParams, RecursiveSNARK},
    provider::{Bn256EngineKZG, GrumpkinEngine},
    traits::Engine,
};
use std::time::Instant;

use circuits::hadamard::HadamardCircuit;
use circuits::mimc::MiMCCircuit;
use circuits::pagerank::PageRankCircuit;
use circuits::scalable::ScalableCircuit;
use circuits::sensor_fusion::SensorFusionCircuit;

type E1 = Bn256EngineKZG;
type E2 = GrumpkinEngine;
type EE1 = nova_snark::provider::hyperkzg::EvaluationEngine<E1>;
type EE2 = nova_snark::provider::ipa_pc::EvaluationEngine<E2>;
type S1 = nova_snark::spartan::snark::RelaxedR1CSSNARK<E1, EE1>;
type S2 = nova_snark::spartan::snark::RelaxedR1CSSNARK<E2, EE2>;

type F1 = <E1 as Engine>::Scalar;

#[derive(Parser, Debug)]
#[command(name = "nova-bench")]
struct Args {
    #[arg(long, default_value = "mimc")]
    circuit: String,

    #[arg(long, default_value = "270")]
    n: usize,

    #[arg(long, default_value = "16")]
    steps: usize,

    #[arg(long, default_value = ".")]
    output_dir: String,

    #[arg(long, default_value = "1")]
    runs: usize,

    #[arg(long, default_value_t = false)]
    warmup: bool,

    #[arg(long, default_value_t = false)]
    print_config: bool,
}

const NOVA_SNARK_VERSION: &str = "0.58.0"; // keep in sync with Cargo.lock

fn print_config() {
    eprintln!("nova_bench_config primary_snark={}", std::any::type_name::<S1>());
    eprintln!("nova_bench_config secondary_snark={}", std::any::type_name::<S2>());
    eprintln!("nova_bench_config rayon_threads={}", rayon::current_num_threads());
    eprintln!("nova_bench_config nova_snark_version={}", NOVA_SNARK_VERSION);
}

#[cfg(test)]
mod tests {
    #[test]
    fn cargo_lock_pins_nova_snark_version() {
        let lock = include_str!("../Cargo.lock");
        assert!(
            lock.contains("name = \"nova-snark\"\nversion = \"0.58.0\""),
            "Cargo.lock does not pin nova-snark 0.58.0"
        );
    }
}

struct BenchResult {
    circuit: String,
    n: usize,
    num_steps: usize,
    setup_ms: f64,
    fold_times_ms: Vec<f64>,
    total_fold_ms: f64,
    compress_ms: f64,
    verify_ms: f64,
    compressed_proof_size: usize,
}

fn get_measured_steps(max_steps: usize) -> Vec<usize> {
    let mut steps = Vec::new();
    let mut s = 1;
    while s <= max_steps {
        steps.push(s);
        s *= 2;
    }
    if *steps.last().unwrap_or(&0) != max_steps {
        steps.push(max_steps);
    }
    steps
}

fn write_csv(result: &BenchResult, output_dir: &str, run_id: usize, warmup: bool) {
    let path = format!("{}/nova_results.csv", output_dir);
    let write_header = !std::path::Path::new(&path).exists();
    let file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .expect("Cannot open CSV file");
    let mut wtr = csv::Writer::from_writer(file);

    if write_header {
        wtr.write_record([
            "circuit",
            "n",
            "step",
            "run_id",
            "warmup",
            "setup_ms",
            "fold_ms",
            "total_fold_ms",
            "compress_ms",
            "verify_ms",
            "proof_bytes",
        ])
        .unwrap();
    }

    let measured = get_measured_steps(result.num_steps);
    for &s in &measured {
        let fold_time = if s > 0 && s <= result.fold_times_ms.len() {
            result.fold_times_ms[s - 1]
        } else {
            0.0
        };
        let cumulative_fold: f64 = result.fold_times_ms[..s.min(result.fold_times_ms.len())]
            .iter()
            .sum();

        wtr.write_record([
            &result.circuit,
            &result.n.to_string(),
            &s.to_string(),
            &run_id.to_string(),
            &(if warmup { "1" } else { "0" }).to_string(),
            &format!("{:.2}", result.setup_ms),
            &format!("{:.2}", fold_time),
            &format!("{:.2}", cumulative_fold),
            &format!("{:.2}", result.compress_ms),
            &format!("{:.2}", result.verify_ms),
            &result.compressed_proof_size.to_string(),
        ])
        .unwrap();
    }
    wtr.flush().unwrap();
    eprintln!("  Nova results written to {}", path);
}

fn bench_mimc(n: usize, num_steps: usize) -> BenchResult {
    let rounds = n / 3;
    eprintln!("  MiMC circuit: {} rounds, {} constraints/step", rounds, n);

    let circuit = MiMCCircuit::<F1>::new(rounds);

    eprintln!("  [Setup] Computing public parameters...");
    let start = Instant::now();
    let pp = PublicParams::<E1, E2, _>::setup(&circuit, &|_| 0, &|_| 0)
        .expect("PublicParams setup failed");
    let setup_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Setup: {:.1} ms", setup_ms);

    let z0 = vec![F1::from(2u64)];

    eprintln!("  [Fold] Running {} steps...", num_steps);
    let mut fold_times = Vec::with_capacity(num_steps);

    // Step 0: create recursive SNARK
    let t_val = F1::from(4u64);
    let c_step = MiMCCircuit::with_input(rounds, t_val);
    let start = Instant::now();
    let mut recursive_snark =
        RecursiveSNARK::new(&pp, &c_step, &z0).expect("RecursiveSNARK::new failed");
    recursive_snark
        .prove_step(&pp, &c_step)
        .expect("prove_step failed");
    let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
    fold_times.push(fold_ms);
    eprintln!("    Step 1: {:.2} ms", fold_ms);

    // Steps 2..num_steps
    for i in 1..num_steps {
        let t_val = F1::from((4 + i) as u64);
        let c_step = MiMCCircuit::with_input(rounds, t_val);

        let start = Instant::now();
        recursive_snark
            .prove_step(&pp, &c_step)
            .expect("prove_step failed");
        let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
        fold_times.push(fold_ms);

        if (i + 1) % 10 == 0 || i + 1 == num_steps {
            eprintln!("    Step {}: {:.2} ms", i + 1, fold_ms);
        }
    }

    let total_fold: f64 = fold_times.iter().sum();

    recursive_snark
        .verify(&pp, num_steps, &z0)
        .expect("RecursiveSNARK verification failed");
    eprintln!("  RecursiveSNARK verified OK");

    // Compress
    eprintln!("  [Compress] Generating compressed SNARK...");
    let (pk, vk) =
        CompressedSNARK::<E1, E2, _, S1, S2>::setup(&pp).expect("CompressedSNARK setup failed");

    let start = Instant::now();
    let compressed =
        CompressedSNARK::prove(&pp, &pk, &recursive_snark).expect("compress failed");
    let compress_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Compress: {:.1} ms", compress_ms);

    let start = Instant::now();
    compressed
        .verify(&vk, num_steps, &z0)
        .expect("compressed verify failed");
    let verify_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Verify: {:.2} ms", verify_ms);

    let compressed_size = bincode::serialize(&compressed).map(|v| v.len()).unwrap_or(0);

    BenchResult {
        circuit: "mimc".to_string(),
        n,
        num_steps,
        setup_ms,
        fold_times_ms: fold_times,
        total_fold_ms: total_fold,
        compress_ms,
        verify_ms,
        compressed_proof_size: compressed_size,
    }
}

fn bench_hadamard(n: usize, num_steps: usize) -> BenchResult {
    let dim = n;
    eprintln!("  Hadamard circuit: dim={}", dim);

    let circuit = HadamardCircuit::<F1>::new(dim);

    eprintln!("  [Setup] Computing public parameters...");
    let start = Instant::now();
    let pp = PublicParams::<E1, E2, _>::setup(&circuit, &|_| 0, &|_| 0)
        .expect("PublicParams setup failed");
    let setup_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Setup: {:.1} ms", setup_ms);

    let z0: Vec<F1> = (0..dim).map(|i| F1::from((2 + i) as u64)).collect();

    eprintln!("  [Fold] Running {} steps...", num_steps);
    let mut fold_times = Vec::with_capacity(num_steps);

    let t_vals: Vec<F1> = (0..dim).map(|j| F1::from((4 + j) as u64)).collect();
    let c_step = HadamardCircuit::with_input(dim, t_vals);
    let start = Instant::now();
    let mut recursive_snark =
        RecursiveSNARK::new(&pp, &c_step, &z0).expect("RecursiveSNARK::new failed");
    recursive_snark
        .prove_step(&pp, &c_step)
        .expect("prove_step failed");
    let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
    fold_times.push(fold_ms);
    eprintln!("    Step 1: {:.2} ms", fold_ms);

    for i in 1..num_steps {
        let t_vals: Vec<F1> = (0..dim).map(|j| F1::from((3 + i + 1 + j) as u64)).collect();
        let c_step = HadamardCircuit::with_input(dim, t_vals);

        let start = Instant::now();
        recursive_snark
            .prove_step(&pp, &c_step)
            .expect("prove_step failed");
        let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
        fold_times.push(fold_ms);

        if (i + 1) % 10 == 0 || i + 1 == num_steps {
            eprintln!("    Step {}: {:.2} ms", i + 1, fold_ms);
        }
    }

    let total_fold: f64 = fold_times.iter().sum();

    recursive_snark
        .verify(&pp, num_steps, &z0)
        .expect("RecursiveSNARK verification failed");
    eprintln!("  RecursiveSNARK verified OK");

    eprintln!("  [Compress] Generating compressed SNARK...");
    let (pk, vk) =
        CompressedSNARK::<E1, E2, _, S1, S2>::setup(&pp).expect("setup failed");

    let start = Instant::now();
    let compressed =
        CompressedSNARK::prove(&pp, &pk, &recursive_snark).expect("compress failed");
    let compress_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Compress: {:.1} ms", compress_ms);

    let start = Instant::now();
    compressed
        .verify(&vk, num_steps, &z0)
        .expect("compressed verify failed");
    let verify_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Verify: {:.2} ms", verify_ms);

    let compressed_size = bincode::serialize(&compressed).map(|v| v.len()).unwrap_or(0);

    BenchResult {
        circuit: "hadamard".to_string(),
        n,
        num_steps,
        setup_ms,
        fold_times_ms: fold_times,
        total_fold_ms: total_fold,
        compress_ms,
        verify_ms,
        compressed_proof_size: compressed_size,
    }
}

fn bench_scalable(n: usize, num_steps: usize) -> BenchResult {
    eprintln!("  Scalable circuit: {} constraints/step", n);

    let circuit = ScalableCircuit::<F1>::new(n);

    eprintln!("  [Setup] Computing public parameters...");
    let start = Instant::now();
    let pp = PublicParams::<E1, E2, _>::setup(&circuit, &|_| 0, &|_| 0)
        .expect("PublicParams setup failed");
    let setup_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Setup: {:.1} ms", setup_ms);

    let z0 = vec![F1::from(2u64)];

    eprintln!("  [Fold] Running {} steps...", num_steps);
    let mut fold_times = Vec::with_capacity(num_steps);

    let t_val = F1::from(4u64);
    let c_step = ScalableCircuit::with_input(n, t_val);
    let start = Instant::now();
    let mut recursive_snark =
        RecursiveSNARK::new(&pp, &c_step, &z0).expect("RecursiveSNARK::new failed");
    recursive_snark
        .prove_step(&pp, &c_step)
        .expect("prove_step failed");
    let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
    fold_times.push(fold_ms);
    eprintln!("    Step 1: {:.2} ms", fold_ms);

    for i in 1..num_steps {
        let t_val = F1::from((4 + i) as u64);
        let c_step = ScalableCircuit::with_input(n, t_val);

        let start = Instant::now();
        recursive_snark
            .prove_step(&pp, &c_step)
            .expect("prove_step failed");
        let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
        fold_times.push(fold_ms);

        if (i + 1) % 10 == 0 || i + 1 == num_steps {
            eprintln!("    Step {}: {:.2} ms", i + 1, fold_ms);
        }
    }

    let total_fold: f64 = fold_times.iter().sum();

    recursive_snark
        .verify(&pp, num_steps, &z0)
        .expect("RecursiveSNARK verification failed");
    eprintln!("  RecursiveSNARK verified OK");

    eprintln!("  [Compress] Generating compressed SNARK...");
    let (pk, vk) =
        CompressedSNARK::<E1, E2, _, S1, S2>::setup(&pp).expect("setup failed");

    let start = Instant::now();
    let compressed =
        CompressedSNARK::prove(&pp, &pk, &recursive_snark).expect("compress failed");
    let compress_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Compress: {:.1} ms", compress_ms);

    let start = Instant::now();
    compressed
        .verify(&vk, num_steps, &z0)
        .expect("compressed verify failed");
    let verify_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Verify: {:.2} ms", verify_ms);

    let compressed_size = bincode::serialize(&compressed).map(|v| v.len()).unwrap_or(0);

    BenchResult {
        circuit: "scalable".to_string(),
        n,
        num_steps,
        setup_ms,
        fold_times_ms: fold_times,
        total_fold_ms: total_fold,
        compress_ms,
        verify_ms,
        compressed_proof_size: compressed_size,
    }
}

fn bench_pagerank(n: usize, num_steps: usize) -> BenchResult {
    let num_nodes = n;
    eprintln!(
        "  PageRank circuit: {} nodes, {} constraints/step",
        num_nodes, num_nodes
    );

    let circuit = PageRankCircuit::<F1>::new(num_nodes);

    eprintln!("  [Setup] Computing public parameters...");
    let start = Instant::now();
    let pp = PublicParams::<E1, E2, _>::setup(&circuit, &|_| 0, &|_| 0)
        .expect("PublicParams setup failed");
    let setup_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Setup: {:.1} ms", setup_ms);

    // z0 = uniform distribution: each element = 1/N
    let inv_n: F1 = Option::from(F1::from(num_nodes as u64).invert()).expect("N is non-zero");
    let z0: Vec<F1> = vec![inv_n; num_nodes];

    eprintln!("  [Fold] Running {} steps...", num_steps);
    let mut fold_times = Vec::with_capacity(num_steps);

    // Step 1: create recursive SNARK
    let start = Instant::now();
    let mut recursive_snark =
        RecursiveSNARK::new(&pp, &circuit, &z0).expect("RecursiveSNARK::new failed");
    recursive_snark
        .prove_step(&pp, &circuit)
        .expect("prove_step failed");
    let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
    fold_times.push(fold_ms);
    eprintln!("    Step 1: {:.2} ms", fold_ms);

    // Steps 2..num_steps
    for i in 1..num_steps {
        let start = Instant::now();
        recursive_snark
            .prove_step(&pp, &circuit)
            .expect("prove_step failed");
        let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
        fold_times.push(fold_ms);

        if (i + 1) % 10 == 0 || i + 1 == num_steps {
            eprintln!("    Step {}: {:.2} ms", i + 1, fold_ms);
        }
    }

    let total_fold: f64 = fold_times.iter().sum();

    recursive_snark
        .verify(&pp, num_steps, &z0)
        .expect("RecursiveSNARK verification failed");
    eprintln!("  RecursiveSNARK verified OK");

    eprintln!("  [Compress] Generating compressed SNARK...");
    let (pk, vk) =
        CompressedSNARK::<E1, E2, _, S1, S2>::setup(&pp).expect("setup failed");

    let start = Instant::now();
    let compressed =
        CompressedSNARK::prove(&pp, &pk, &recursive_snark).expect("compress failed");
    let compress_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Compress: {:.1} ms", compress_ms);

    let start = Instant::now();
    compressed
        .verify(&vk, num_steps, &z0)
        .expect("compressed verify failed");
    let verify_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Verify: {:.2} ms", verify_ms);

    let compressed_size = bincode::serialize(&compressed).map(|v| v.len()).unwrap_or(0);

    BenchResult {
        circuit: "pagerank".to_string(),
        n,
        num_steps,
        setup_ms,
        fold_times_ms: fold_times,
        total_fold_ms: total_fold,
        compress_ms,
        verify_ms,
        compressed_proof_size: compressed_size,
    }
}

fn bench_sensor_fusion(n: usize, num_steps: usize) -> BenchResult {
    let num_sensors = n;
    eprintln!(
        "  Sensor fusion circuit: {} sensors, 1 constraint/step",
        num_sensors
    );

    let circuit = SensorFusionCircuit::<F1>::new(num_sensors);

    eprintln!("  [Setup] Computing public parameters...");
    let start = Instant::now();
    let pp = PublicParams::<E1, E2, _>::setup(&circuit, &|_| 0, &|_| 0)
        .expect("PublicParams setup failed");
    let setup_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Setup: {:.1} ms", setup_ms);

    // z0 = [0] (initial fused value = 0)
    let z0 = vec![F1::ZERO];

    eprintln!("  [Fold] Running {} steps...", num_steps);
    let mut fold_times = Vec::with_capacity(num_steps);

    // Step 1: create recursive SNARK with first set of readings
    // Readings match C++ trace: x_{t,k} = (step-1)*K + k + 1
    let readings: Vec<F1> = (0..num_sensors)
        .map(|k| F1::from((k + 1) as u64))
        .collect();
    let c_step = SensorFusionCircuit::with_readings(num_sensors, readings);
    let start = Instant::now();
    let mut recursive_snark =
        RecursiveSNARK::new(&pp, &c_step, &z0).expect("RecursiveSNARK::new failed");
    recursive_snark
        .prove_step(&pp, &c_step)
        .expect("prove_step failed");
    let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
    fold_times.push(fold_ms);
    eprintln!("    Step 1: {:.2} ms", fold_ms);

    // Steps 2..num_steps
    for i in 1..num_steps {
        let readings: Vec<F1> = (0..num_sensors)
            .map(|k| F1::from((i * num_sensors + k + 1) as u64))
            .collect();
        let c_step = SensorFusionCircuit::with_readings(num_sensors, readings);

        let start = Instant::now();
        recursive_snark
            .prove_step(&pp, &c_step)
            .expect("prove_step failed");
        let fold_ms = start.elapsed().as_secs_f64() * 1000.0;
        fold_times.push(fold_ms);

        if (i + 1) % 10 == 0 || i + 1 == num_steps {
            eprintln!("    Step {}: {:.2} ms", i + 1, fold_ms);
        }
    }

    let total_fold: f64 = fold_times.iter().sum();

    recursive_snark
        .verify(&pp, num_steps, &z0)
        .expect("RecursiveSNARK verification failed");
    eprintln!("  RecursiveSNARK verified OK");

    eprintln!("  [Compress] Generating compressed SNARK...");
    let (pk, vk) =
        CompressedSNARK::<E1, E2, _, S1, S2>::setup(&pp).expect("setup failed");

    let start = Instant::now();
    let compressed =
        CompressedSNARK::prove(&pp, &pk, &recursive_snark).expect("compress failed");
    let compress_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Compress: {:.1} ms", compress_ms);

    let start = Instant::now();
    compressed
        .verify(&vk, num_steps, &z0)
        .expect("compressed verify failed");
    let verify_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!("  Verify: {:.2} ms", verify_ms);

    let compressed_size = bincode::serialize(&compressed).map(|v| v.len()).unwrap_or(0);

    BenchResult {
        circuit: "sensor_fusion".to_string(),
        n,
        num_steps,
        setup_ms,
        fold_times_ms: fold_times,
        total_fold_ms: total_fold,
        compress_ms,
        verify_ms,
        compressed_proof_size: compressed_size,
    }
}

/// Untimed warm-up: build the same public parameters and step-1 circuit as the
/// matching `bench_*` function, run `RecursiveSNARK::new` + one `prove_step`,
/// then discard everything.
fn warmup_one<C: nova_snark::traits::circuit::StepCircuit<F1>>(
    setup_circuit: &C,
    step_circuit: &C,
    z0: &[F1],
) {
    let pp = PublicParams::<E1, E2, C>::setup(setup_circuit, &|_| 0, &|_| 0)
        .expect("PublicParams setup failed");
    let mut recursive_snark =
        RecursiveSNARK::new(&pp, step_circuit, z0).expect("RecursiveSNARK::new failed");
    recursive_snark
        .prove_step(&pp, step_circuit)
        .expect("prove_step failed");
}

fn warmup_fold(circuit: &str, n: usize) {
    eprintln!("  [Warm-up] Untimed step-1 fold...");
    match circuit {
        "mimc" => {
            let rounds = n / 3;
            warmup_one(
                &MiMCCircuit::<F1>::new(rounds),
                &MiMCCircuit::with_input(rounds, F1::from(4u64)),
                &[F1::from(2u64)],
            );
        }
        "hadamard" | "matmul" => {
            let dim = n;
            let z0: Vec<F1> = (0..dim).map(|i| F1::from((2 + i) as u64)).collect();
            let t_vals: Vec<F1> = (0..dim).map(|j| F1::from((4 + j) as u64)).collect();
            warmup_one(
                &HadamardCircuit::<F1>::new(dim),
                &HadamardCircuit::with_input(dim, t_vals),
                &z0,
            );
        }
        "pagerank" => {
            let num_nodes = n;
            let inv_n: F1 =
                Option::from(F1::from(num_nodes as u64).invert()).expect("N is non-zero");
            let z0: Vec<F1> = vec![inv_n; num_nodes];
            let c = PageRankCircuit::<F1>::new(num_nodes);
            warmup_one(&c, &c, &z0);
        }
        "sensor_fusion" => {
            let num_sensors = n;
            let readings: Vec<F1> = (0..num_sensors).map(|k| F1::from((k + 1) as u64)).collect();
            warmup_one(
                &SensorFusionCircuit::<F1>::new(num_sensors),
                &SensorFusionCircuit::with_readings(num_sensors, readings),
                &[F1::ZERO],
            );
        }
        "scalable" => {
            warmup_one(
                &ScalableCircuit::<F1>::new(n),
                &ScalableCircuit::with_input(n, F1::from(4u64)),
                &[F1::from(2u64)],
            );
        }
        other => {
            eprintln!("Unknown circuit: {}", other);
            std::process::exit(1);
        }
    }
}

fn main() {
    let args = Args::parse();

    if args.print_config {
        print_config();
        return;
    }
    print_config();

    eprintln!("================================================================");
    eprintln!("Nova Folding-Scheme Benchmark");
    eprintln!("================================================================");
    eprintln!(
        "  Circuit: {}, n={}, steps={}",
        args.circuit, args.n, args.steps
    );

    for run_id in 0..args.runs {
        eprintln!("\n  === Run {}/{} ===", run_id + 1, args.runs);

        if args.warmup {
            warmup_fold(&args.circuit, args.n);
        }

        let result = match args.circuit.as_str() {
            "mimc" => bench_mimc(args.n, args.steps),
            "hadamard" | "matmul" => bench_hadamard(args.n, args.steps),
            "pagerank" => bench_pagerank(args.n, args.steps),
            "sensor_fusion" => bench_sensor_fusion(args.n, args.steps),
            "scalable" => bench_scalable(args.n, args.steps),
            other => {
                eprintln!("Unknown circuit: {}", other);
                std::process::exit(1);
            }
        };

        eprintln!("\n  Summary:");
        eprintln!("    Setup:         {:.1} ms", result.setup_ms);
        eprintln!("    Total fold:    {:.1} ms", result.total_fold_ms);
        eprintln!(
            "    Avg fold/step: {:.2} ms",
            result.total_fold_ms / result.num_steps as f64
        );
        eprintln!("    Compress:      {:.1} ms", result.compress_ms);
        eprintln!("    Verify:        {:.2} ms", result.verify_ms);

        write_csv(&result, &args.output_dir, run_id, args.warmup);
    }

    eprintln!("\n================================================================");
    eprintln!("Nova benchmark complete.");
    eprintln!("================================================================");
}
