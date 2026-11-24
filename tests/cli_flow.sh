#!/usr/bin/env bash
set -euo pipefail

binary="$1"
assets="$2"
workdir="$(mktemp -d)"
trap 'rm -rf -- "$workdir"' EXIT
db="$workdir/faces.json"
options=(--models "$assets/models" --db "$db")

"$binary" "${options[@]}" model-check

assert_check() {
    local image="$1"
    local expected="$2"
    local code="$3"
    local output
    local status=0
    output="$("$binary" "${options[@]}" check "$image")" || status=$?
    printf '%s %s\n' "${image##*/}" "$output"
    test "$status" -eq "$code"
    python3 - "$output" "$expected" <<'PY'
import json
import sys

result = json.loads(sys.argv[1])
expected = sys.argv[2]
assert result["decision"] == expected, result
assert result["simulated_unlock"] == (expected == "authorized"), result
if expected in ("authorized", "unknown"):
    assert result["face_count"] == 1, result
    assert (result["similarity"] >= result["threshold"]) == (expected == "authorized"), result
    assert ("name" in result) == (expected == "authorized"), result
PY
}

# 每条命令都是独立进程，验证录入之后能从磁盘重读模板。
assert_check "$assets/samples/known.jpg" no_enrollment 3
"$binary" "${options[@]}" enroll '样例 A' "$assets/samples/enroll.jpg"
assert_check "$assets/samples/known.jpg" authorized 0
assert_check "$assets/samples/unknown.jpg" unknown 3

if "$binary" "${options[@]}" enroll '样例 A' "$assets/samples/unknown.jpg"; then
    echo 'Duplicate enrollment was unexpectedly accepted.' >&2
    exit 1
fi
assert_check "$assets/samples/known.jpg" authorized 0
