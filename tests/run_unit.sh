#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/usbwarp-tests.XXXXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
cc=${CC:-gcc}
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"$cc" "${flags[@]}" include/verify_headers.c -o "$test_dir/headers"
"$cc" "${flags[@]}" tests/ring_test.c -o "$test_dir/ring"
"$cc" "${flags[@]}" tests/iso_test.c -o "$test_dir/iso"
"$cc" "${flags[@]}" -pthread -Itests/stubs tests/ring_guest_test.c -o "$test_dir/guest"
"$cc" "${flags[@]}" -pthread -Itests/stubs tests/ring_host_test.c -o "$test_dir/host"
"$cc" "${flags[@]}" -Itests/stubs tests/hub_test.c -o "$test_dir/hub"
"$cc" "${flags[@]}" -Itests/stubs tests/transfer_test.c -o "$test_dir/transfer"
"$cc" "${flags[@]}" -pthread -Itests/stubs tests/rx_test.c -o "$test_dir/rx"
"$test_dir/headers"
"$test_dir/ring"
"$test_dir/iso"
"$test_dir/guest"
"$test_dir/host"
"$test_dir/hub"
"$test_dir/transfer"
"$test_dir/rx"
