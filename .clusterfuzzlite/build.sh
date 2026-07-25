#!/bin/bash -eu
# Build shibadb-c fuzz targets under the OSS-Fuzz / ClusterFuzzLite
# toolchain. The harness supplies $CC, $CFLAGS, $CXX, $CXXFLAGS with the
# right sanitizer + coverage instrumentation, and $LIB_FUZZING_ENGINE
# is the object to link against for the fuzzing engine (libFuzzer,
# AFL++, Honggfuzz, or Centipede). We forward all of these to CMake.

cd "$SRC/shibadb-c"

# The library sources must inherit the harness flags so their coverage
# and sanitizer instrumentation matches the fuzz driver. CMake picks up
# $CC/$CFLAGS/$CXX/$CXXFLAGS from the environment.
#
# We disable the local -fsanitize=fuzzer,address,undefined branch by
# setting SDB_FUZZ_LINK_FLAGS to the harness-provided engine object.
cmake -S . -B build-oss-fuzz -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDB_BUILD_TESTS=OFF \
    -DSDB_ENABLE_FUZZERS=ON \
    -DSDB_FUZZ_LINK_FLAGS="$LIB_FUZZING_ENGINE"

cmake --build build-oss-fuzz --parallel "$(nproc)"

# Copy fuzz target binaries into $OUT and pair each with its seed corpus
# (if the corpus directory exists) as required by OSS-Fuzz.
for target in fuzz_superblock fuzz_page fuzz_btree_page fuzz_xchacha20poly1305; do
    cp "build-oss-fuzz/${target}" "$OUT/${target}"
    corpus_dir="fuzz/corpus-${target#fuzz_}"
    if [ -d "$corpus_dir" ]; then
        (cd "$corpus_dir" && zip -qr "$OUT/${target}_seed_corpus.zip" .)
    fi
done
