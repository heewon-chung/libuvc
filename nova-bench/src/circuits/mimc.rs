//! MiMC-like hash chain circuit for Nova benchmarks.

use ff::PrimeField;
use nova_snark::frontend::{num::AllocatedNum, ConstraintSystem, SynthesisError};
use nova_snark::traits::circuit::StepCircuit;

#[derive(Clone, Debug)]
pub struct MiMCCircuit<F: PrimeField> {
    pub rounds: usize,
    pub t: Option<F>,
}

impl<F: PrimeField> MiMCCircuit<F> {
    pub fn new(rounds: usize) -> Self {
        Self { rounds, t: None }
    }

    pub fn with_input(rounds: usize, t: F) -> Self {
        Self {
            rounds,
            t: Some(t),
        }
    }
}

impl<F: PrimeField> StepCircuit<F> for MiMCCircuit<F> {
    fn arity(&self) -> usize {
        1
    }

    fn synthesize<CS: ConstraintSystem<F>>(
        &self,
        cs: &mut CS,
        z: &[AllocatedNum<F>],
    ) -> Result<Vec<AllocatedNum<F>>, SynthesisError> {
        assert_eq!(z.len(), 1);
        let s_in = &z[0];

        let t = AllocatedNum::alloc(cs.namespace(|| "t"), || {
            self.t.ok_or(SynthesisError::AssignmentMissing)
        })?;

        let mut prev = s_in.clone();

        for r in 0..self.rounds {
            let w_mix = AllocatedNum::alloc(cs.namespace(|| format!("w_mix_{}", r)), || {
                let p = prev.get_value().ok_or(SynthesisError::AssignmentMissing)?;
                let tv = t.get_value().ok_or(SynthesisError::AssignmentMissing)?;
                Ok(p * tv)
            })?;
            cs.enforce(
                || format!("mix_{}", r),
                |lc| lc + prev.get_variable(),
                |lc| lc + t.get_variable(),
                |lc| lc + w_mix.get_variable(),
            );

            let w_sq = AllocatedNum::alloc(cs.namespace(|| format!("w_sq_{}", r)), || {
                let m = w_mix.get_value().ok_or(SynthesisError::AssignmentMissing)?;
                Ok(m * m)
            })?;
            cs.enforce(
                || format!("sq_{}", r),
                |lc| lc + w_mix.get_variable(),
                |lc| lc + w_mix.get_variable(),
                |lc| lc + w_sq.get_variable(),
            );

            let w_cube = AllocatedNum::alloc(cs.namespace(|| format!("w_cube_{}", r)), || {
                let sq = w_sq.get_value().ok_or(SynthesisError::AssignmentMissing)?;
                let m = w_mix.get_value().ok_or(SynthesisError::AssignmentMissing)?;
                Ok(sq * m)
            })?;
            cs.enforce(
                || format!("cube_{}", r),
                |lc| lc + w_sq.get_variable(),
                |lc| lc + w_mix.get_variable(),
                |lc| lc + w_cube.get_variable(),
            );

            prev = w_cube;
        }

        Ok(vec![prev])
    }
}
