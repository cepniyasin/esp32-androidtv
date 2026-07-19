#!/usr/bin/env bash
# Build and run host-side unit tests (framing now; pairing secret later).
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p build
gcc -Wall -Wextra -Werror -O1 \
    -I components/nanopb -I main -I main/proto_gen -I test \
    main/proto_frame.c \
    main/proto_gen/polo.pb.c \
    components/nanopb/pb_common.c components/nanopb/pb_encode.c components/nanopb/pb_decode.c \
    test/test_frame.c \
    -o build/test_frame
./build/test_frame
