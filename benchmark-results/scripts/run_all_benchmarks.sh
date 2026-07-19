#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────
# run_all_benchmarks.sh — thin delegation wrapper (retired legacy runner)
#
# The full paper-grade benchmark lifecycle (pinned CMake configuration,
# canonical uvc_results.csv contract, coordinate manifest, fatal
# verify_completeness gate, provenance run manifest, and read-only
# sealing) lives in the canonical runner:
#
#     scripts/run_benchmarks.sh
#
# This wrapper exists only so historical invocations keep working; it
# forwards every argument to the canonical runner. The old standalone
# 18-configuration matrix measured here diverged from the strict
# verifier and lacked the canonical lifecycle guarantees, so it was
# retired in favor of delegation.
# ──────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CANONICAL="$PROJECT_DIR/scripts/run_benchmarks.sh"

if [[ ! -x "$CANONICAL" ]]; then
    echo "ERROR: canonical runner not found at $CANONICAL" >&2
    exit 1
fi

echo "run_all_benchmarks.sh is a delegation wrapper; running the canonical runner:"
echo "  $CANONICAL $*"
exec "$CANONICAL" "$@"
