<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-03-19 | Updated: 2026-03-19 -->

# nova-bench

## Purpose
Rust-based benchmark suite for Microsoft Nova (IVC proof system) used as a comparison baseline against UVC. Nova uses recursive proof composition with folding schemes, which is the approach this research aims to improve upon.

## Key Files

| File | Description |
|------|-------------|
| `Cargo.toml` | Rust project manifest with Nova dependencies |
| `Cargo.lock` | Locked dependency versions |
| `src/` | Rust source implementing benchmark circuits for Nova comparison |

## For AI Agents

### Working In This Directory
- This is a **separate Rust project** -- build with `cargo build --release`
- Used purely for baseline comparison; the main implementation is in C++ under `../src/libsnark/`
- Results from Nova benchmarks are compared against UVC benchmarks in the paper

### Build Commands
```bash
cargo build --release
cargo run --release
```

<!-- MANUAL: -->
