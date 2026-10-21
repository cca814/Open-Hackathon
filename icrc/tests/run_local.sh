#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build-local
cc=${CC:-cc}
flags='-std=c11 -Wall -Wextra -Werror -pedantic -g'
# Keep the ctypes oracle library unsanitized; run C ownership tests separately
# with sanitizers independently so Python does not need an injected ASan runtime.
$cc $flags -fPIC -shared -Iinclude src/roce.c src/icrc.c src/groups.c -o build-local/libnode_b_packet.so
$cc $flags -Iinclude src/groups.c tools/groups_check.c -o build-local/groups_check
$cc $flags -Iinclude src/worker.c src/groups.c src/roce.c src/icrc.c tests/test_worker.c -o build-local/test_worker
$cc $flags -Iinclude src/worker.c src/groups.c src/roce.c src/icrc.c tests/test_fanout.c -o build-local/test_fanout
./build-local/test_worker
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 tests/oracle_packets.py build-local/libnode_b_packet.so build-local/groups_check build-local/test_fanout
$cc $flags -Iinclude src/roce.c src/icrc.c tests/test_roce_bounds.c -o build-local/test_roce_bounds
./build-local/test_roce_bounds
