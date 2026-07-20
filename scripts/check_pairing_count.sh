#!/usr/bin/env bash
set -euo pipefail

source_file="src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/r1cs_uvc_ppzksnark.tcc"
if [ ! -f "$source_file" ]; then
    echo "uvc pairing-count gate: source file was not found" >&2
    exit 1
fi
last_verifier_line=$(awk '/bool r1cs_uvc_ppzksnark_verifier\(/ { line = NR } END { if (line) print line }' "$source_file")

if [ -z "$last_verifier_line" ]; then
    echo "uvc pairing-count gate: last verifier declaration was not found" >&2
    exit 1
fi

if ! body=$(awk -v start="$last_verifier_line" '
    NR < start { next }
    {
        for (i = 1; i <= length($0); ++i) {
            c = substr($0, i, 1)
            if (!opened) {
                if (c == "{") {
                    opened = 1
                    depth = 1
                    printf "%s", c
                }
                continue
            }

            printf "%s", c
            if (c == "{") {
                ++depth
            } else if (c == "}") {
                --depth
                if (depth == 0) {
                    printf "\n"
                    exit
                }
            }
        }
        if (opened) printf "\n"
    }
    END {
        if (!opened || depth != 0) exit 1
    }
' "$source_file"); then
    echo "uvc pairing-count gate: last verifier body has unbalanced braces" >&2
    exit 1
fi

count_literal() {
    awk -v token="$1" '
        {
            line = $0
            while ((pos = index(line, token)) != 0) {
                ++count
                line = substr(line, pos + length(token))
            }
        }
        END { print count + 0 }
    ' <<<"$body"
}

count_eta() {
    awk '
        {
            line = $0
            while (match(line, /(^|[^[:alnum:]_])eta([^[:alnum:]_]|$)/)) {
                ++count
                line = substr(line, RSTART + RLENGTH)
            }
        }
        END { print count + 0 }
    ' <<<"$body"
}

require_count() {
    local token="$1" expected="$2" actual="$3"
    if [ "$actual" -ne "$expected" ]; then
        echo "uvc pairing-count gate: expected $expected occurrences of $token, found $actual" >&2
        exit 1
    fi
}

require_present() {
    local token="$1" actual="$2"
    if [ "$actual" -eq 0 ]; then
        echo "uvc pairing-count gate: required token $token was not found" >&2
        exit 1
    fi
}

require_count 'ppT::miller_loop(' 1 "$(count_literal 'ppT::miller_loop(')"
require_count 'ppT::double_miller_loop(' 1 "$(count_literal 'ppT::double_miller_loop(')"
require_count 'ppT::final_exponentiation(' 1 "$(count_literal 'ppT::final_exponentiation(')"
require_count 'eta token' 0 "$(count_eta)"
require_count 'reduced_pairing' 0 "$(count_literal 'reduced_pairing')"
require_count '::pairing' 0 "$(count_literal '::pairing')"
require_present 'alpha_g1_beta_g2' "$(count_literal 'alpha_g1_beta_g2')"
