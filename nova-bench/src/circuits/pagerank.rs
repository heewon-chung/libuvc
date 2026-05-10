//! PageRank (power iteration) circuit for Nova benchmarks.
//!
//! Per step: s_out = d * M * s_in + (1-d)/N
//! where M is a column-stochastic transition matrix for a deterministic graph,
//! d = 85/100 is the damping factor, and N is the number of nodes.
//!
//! Uses N constraints per step (one per node). Nova can use the constant wire
//! in LCs, so the bias addition is absorbed into constraint C without needing
//! a separate multiplication constraint (unlike UVC which needs 2N).
//!
//! Graph construction matches the C++ UVC circuit exactly:
//!   - Cycle: node i -> node (i+1) % N
//!   - Extra: node i -> node (2*i+1) % N (no duplicates)

use ff::PrimeField;
use nova_snark::frontend::{num::AllocatedNum, ConstraintSystem, SynthesisError};
use nova_snark::traits::circuit::StepCircuit;

/// Generate a deterministic directed graph (matches C++ make_deterministic_graph).
fn make_deterministic_graph(n: usize) -> Vec<Vec<usize>> {
    let mut adj = vec![vec![]; n];
    for i in 0..n {
        let cycle_target = (i + 1) % n;
        adj[i].push(cycle_target);

        let extra_target = (2 * i + 1) % n;
        if !adj[i].contains(&extra_target) {
            adj[i].push(extra_target);
        }
    }
    adj
}

/// Build column-stochastic transition matrix as sparse rows.
///
/// M[k][j] = 1/out_degree(j) if edge j->k exists, else 0.
/// Returns M[k] as a list of (j, M[k][j]) for non-zero entries.
fn build_transition_rows<F: PrimeField>(n: usize, adj: &[Vec<usize>]) -> Vec<Vec<(usize, F)>> {
    let mut rows: Vec<Vec<(usize, F)>> = vec![vec![]; n];

    for j in 0..n {
        let deg = adj[j].len() as u64;
        let inv_deg: F = Option::from(F::from(deg).invert()).expect("degree is non-zero");
        for &target in &adj[j] {
            rows[target].push((j, inv_deg));
        }
    }
    rows
}

#[derive(Clone, Debug)]
pub struct PageRankCircuit<F: PrimeField> {
    pub num_nodes: usize,
    /// Sparse transition matrix rows: m_rows[k] = [(j, M[k][j]), ...]
    m_rows: Vec<Vec<(usize, F)>>,
    /// Damping factor d = 85/100
    d: F,
    /// Bias = (1 - d) / N
    bias: F,
}

impl<F: PrimeField> PageRankCircuit<F> {
    pub fn new(num_nodes: usize) -> Self {
        let adj = make_deterministic_graph(num_nodes);
        let m_rows = build_transition_rows::<F>(num_nodes, &adj);

        let hundred_inv: F = Option::from(F::from(100u64).invert()).expect("100 is non-zero");
        let d = F::from(85u64) * hundred_inv;
        let n_inv: F =
            Option::from(F::from(num_nodes as u64).invert()).expect("num_nodes is non-zero");
        let bias = (F::ONE - d) * n_inv;

        Self {
            num_nodes,
            m_rows,
            d,
            bias,
        }
    }
}

impl<F: PrimeField> StepCircuit<F> for PageRankCircuit<F> {
    fn arity(&self) -> usize {
        self.num_nodes
    }

    fn synthesize<CS: ConstraintSystem<F>>(
        &self,
        cs: &mut CS,
        z: &[AllocatedNum<F>],
    ) -> Result<Vec<AllocatedNum<F>>, SynthesisError> {
        assert_eq!(z.len(), self.num_nodes);
        let n = self.num_nodes;
        let mut outputs = Vec::with_capacity(n);

        for k in 0..n {
            // Witness: s_out[k] = d * (sum_j M[k][j] * z[j]) + bias
            let s_out_k =
                AllocatedNum::alloc(cs.namespace(|| format!("s_out_{}", k)), || {
                    let mut mv_k = F::ZERO;
                    for &(j, m_kj) in &self.m_rows[k] {
                        let z_j = z[j].get_value().ok_or(SynthesisError::AssignmentMissing)?;
                        mv_k += m_kj * z_j;
                    }
                    Ok(self.d * mv_k + self.bias)
                })?;

            // Constraint: (sum_j M[k][j] * z[j]) * d = s_out[k] - bias
            //   A = sum_j M[k][j] * z[j]
            //   B = d * ONE
            //   C = s_out[k] - bias * ONE
            cs.enforce(
                || format!("pagerank_{}", k),
                |mut lc| {
                    for &(j, m_kj) in &self.m_rows[k] {
                        lc = lc + (m_kj, z[j].get_variable());
                    }
                    lc
                },
                |lc| lc + (self.d, CS::one()),
                |lc| lc + s_out_k.get_variable() - (self.bias, CS::one()),
            );

            outputs.push(s_out_k);
        }

        Ok(outputs)
    }
}
