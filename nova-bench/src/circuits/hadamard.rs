//! Hadamard (element-wise) product circuit for Nova benchmarks.

use ff::PrimeField;
use nova_snark::frontend::{num::AllocatedNum, ConstraintSystem, SynthesisError};
use nova_snark::traits::circuit::StepCircuit;

#[derive(Clone, Debug)]
pub struct HadamardCircuit<F: PrimeField> {
    pub dim: usize,
    pub t: Option<Vec<F>>,
}

impl<F: PrimeField> HadamardCircuit<F> {
    pub fn new(dim: usize) -> Self {
        Self { dim, t: None }
    }

    pub fn with_input(dim: usize, t: Vec<F>) -> Self {
        Self { dim, t: Some(t) }
    }
}

impl<F: PrimeField> StepCircuit<F> for HadamardCircuit<F> {
    fn arity(&self) -> usize {
        self.dim
    }

    fn synthesize<CS: ConstraintSystem<F>>(
        &self,
        cs: &mut CS,
        z: &[AllocatedNum<F>],
    ) -> Result<Vec<AllocatedNum<F>>, SynthesisError> {
        assert_eq!(z.len(), self.dim);
        let mut outputs = Vec::with_capacity(self.dim);

        for i in 0..self.dim {
            let t_i = AllocatedNum::alloc(cs.namespace(|| format!("t_{}", i)), || {
                self.t
                    .as_ref()
                    .and_then(|v| v.get(i).copied())
                    .ok_or(SynthesisError::AssignmentMissing)
            })?;

            let s_out_i = AllocatedNum::alloc(cs.namespace(|| format!("s_out_{}", i)), || {
                let s = z[i].get_value().ok_or(SynthesisError::AssignmentMissing)?;
                let t = t_i.get_value().ok_or(SynthesisError::AssignmentMissing)?;
                Ok(s * t)
            })?;

            cs.enforce(
                || format!("mul_{}", i),
                |lc| lc + z[i].get_variable(),
                |lc| lc + t_i.get_variable(),
                |lc| lc + s_out_i.get_variable(),
            );

            outputs.push(s_out_i);
        }

        Ok(outputs)
    }
}
