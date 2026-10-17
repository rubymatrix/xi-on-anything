#!/bin/sh
# Every shader key pair the game has used, as WGSL (tests/wgsl_corpus.c), checked with naga:
#   tools/web/wgsl_check.sh [pipelines.v1] [naga]
# The keys come from the Metal back end's pipeline cache (~/Library/Caches/FFXI/pipelines.v1); naga is
# the WGSL validator from `cargo install naga-cli`.
set -e
cd "$(dirname "$0")/../.."
CACHE=${1:-$HOME/Library/Caches/FFXI/pipelines.v1}
NAGA=${2:-naga}
mkdir -p build
clang -O1 -I runtime -I runtime/portable tests/wgsl_corpus.c runtime/portable/gfx_msl.c runtime/portable/gfx_msl_shaders.c \
    -o build/wgsl_corpus
rm -rf build/wgsl
build/wgsl_corpus "$CACHE" build/wgsl
ok=0; bad=0
for f in build/wgsl/*.wgsl; do
    if "$NAGA" "$f" >/dev/null 2>build/wgsl_last_error.txt; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "$f:"; head -8 build/wgsl_last_error.txt; fi
done
echo "naga: $ok valid, $bad invalid"
[ "$bad" = 0 ]
