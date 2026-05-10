//! Weighted multi-sensor fusion (EMA) circuit for Nova benchmarks.
//!
//! Per step: s_out = sum_{k=1}^{K} w_k * x_k + beta * s_in
//! where w_k are fixed inverse-variance weights (var_k = k+1, normalized),
//! x_k are K sensor readings, s_in is the previous fused estimate, and
//! beta = 1/10 is the EMA decay parameter.
//!
//! Uses 1 constraint per step (natural minimum for Nova):
//!   (sum w_k * x_k + beta * z[0]) * ONE = s_out
//!
//! Weight computation matches C++ sensor_fusion_circuit.hpp exactly:
//!   variance_k = k + 2 (0-indexed), raw_k = 1/variance_k, normalized.

use ff::PrimeField;
use nova_snark::frontend::{num::AllocatedNum, ConstraintSystem, SynthesisError};
use nova_snark::traits::circuit::StepCircuit;

/// Compute normalized inverse-variance weights (matches C++ compute_sensor_weights).
fn compute_weights<F: PrimeField>(num_sensors: usize) -> Vec<F> {
    let mut raw = Vec::with_capacity(num_sensors);
    let mut sum = F::ZERO;
    for k in 0..num_sensors {
        let var_k = F::from((k + 2) as u64);
        let inv: F = Option::from(var_k.invert()).expect("variance is non-zero");
        raw.push(inv);
        sum += inv;
    }
    let sum_inv: F = Option::from(sum.invert()).expect("sum is non-zero");
    raw.iter().map(|r| *r * sum_inv).collect()
}

#[derive(Clone, Debug)]
pub struct SensorFusionCircuit<F: PrimeField> {
    pub num_sensors: usize,
    weights: Vec<F>,
    beta: F,
    /// Per-step sensor readings (None during setup, Some during proving)
    readings: Option<Vec<F>>,
}

impl<F: PrimeField> SensorFusionCircuit<F> {
    /// Create circuit for setup (no per-step input).
    pub fn new(num_sensors: usize) -> Self {
        let weights = compute_weights::<F>(num_sensors);
        let beta: F = Option::from(F::from(10u64).invert()).expect("10 is non-zero");
        Self {
            num_sensors,
            weights,
            beta,
            readings: None,
        }
    }

    /// Create circuit with per-step sensor readings.
    pub fn with_readings(num_sensors: usize, readings: Vec<F>) -> Self {
        assert_eq!(readings.len(), num_sensors);
        let weights = compute_weights::<F>(num_sensors);
        let beta: F = Option::from(F::from(10u64).invert()).expect("10 is non-zero");
        Self {
            num_sensors,
            weights,
            beta,
            readings: Some(readings),
        }
    }
}

impl<F: PrimeField> StepCircuit<F> for SensorFusionCircuit<F> {
    fn arity(&self) -> usize {
        1 // single scalar state: the fused value
    }

    fn synthesize<CS: ConstraintSystem<F>>(
        &self,
        cs: &mut CS,
        z: &[AllocatedNum<F>],
    ) -> Result<Vec<AllocatedNum<F>>, SynthesisError> {
        assert_eq!(z.len(), 1);
        let k = self.num_sensors;

        // Allocate sensor readings x_1..x_K
        let mut x_vars = Vec::with_capacity(k);
        for i in 0..k {
            let x_i = AllocatedNum::alloc(cs.namespace(|| format!("x_{}", i + 1)), || {
                self.readings
                    .as_ref()
                    .map(|r| r[i])
                    .ok_or(SynthesisError::AssignmentMissing)
            })?;
            x_vars.push(x_i);
        }

        // Witness: s_out = sum(w_k * x_k) + beta * s_in
        let s_out = AllocatedNum::alloc(cs.namespace(|| "s_out"), || {
            let s_in = z[0].get_value().ok_or(SynthesisError::AssignmentMissing)?;
            let mut acc = self.beta * s_in;
            for i in 0..k {
                let x_val = x_vars[i]
                    .get_value()
                    .ok_or(SynthesisError::AssignmentMissing)?;
                acc += self.weights[i] * x_val;
            }
            Ok(acc)
        })?;

        // 1 constraint: (sum w_k * x_k + beta * z[0]) * ONE = s_out
        //   A = sum w_k * x_k + beta * z[0]   (multi-term LC)
        //   B = ONE
        //   C = s_out
        cs.enforce(
            || "sensor_fusion",
            |mut lc| {
                for i in 0..k {
                    lc = lc + (self.weights[i], x_vars[i].get_variable());
                }
                lc = lc + (self.beta, z[0].get_variable());
                lc
            },
            |lc| lc + CS::one(),
            |lc| lc + s_out.get_variable(),
        );

        Ok(vec![s_out])
    }
}
