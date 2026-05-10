//! Configurable-size circuit using chained squarings for Nova benchmarks.

use ff::PrimeField;
use nova_snark::frontend::{num::AllocatedNum, ConstraintSystem, SynthesisError};
use nova_snark::traits::circuit::StepCircuit;

#[derive(Clone, Debug)]
pub struct ScalableCircuit<F: PrimeField> {
    pub target_constraints: usize,
    pub t: Option<F>,
}

impl<F: PrimeField> ScalableCircuit<F> {
    pub fn new(target_constraints: usize) -> Self {
        Self {
            target_constraints,
            t: None,
        }
    }

    pub fn with_input(target_constraints: usize, t: F) -> Self {
        Self {
            target_constraints,
            t: Some(t),
        }
    }
}

impl<F: PrimeField> StepCircuit<F> for ScalableCircuit<F> {
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

        // Constraint 1: s_in * t = w_0
        let mut prev = AllocatedNum::alloc(cs.namespace(|| "w_0"), || {
            let s = s_in.get_value().ok_or(SynthesisError::AssignmentMissing)?;
            let tv = t.get_value().ok_or(SynthesisError::AssignmentMissing)?;
            Ok(s * tv)
        })?;
        cs.enforce(
            || "mix",
            |lc| lc + s_in.get_variable(),
            |lc| lc + t.get_variable(),
            |lc| lc + prev.get_variable(),
        );

        // Constraints 2..target: repeated squaring
        for c in 1..self.target_constraints {
            let next = AllocatedNum::alloc(cs.namespace(|| format!("w_{}", c)), || {
                let p = prev.get_value().ok_or(SynthesisError::AssignmentMissing)?;
                Ok(p * p)
            })?;
            cs.enforce(
                || format!("sq_{}", c),
                |lc| lc + prev.get_variable(),
                |lc| lc + prev.get_variable(),
                |lc| lc + next.get_variable(),
            );
            prev = next;
        }

        Ok(vec![prev])
    }
}
