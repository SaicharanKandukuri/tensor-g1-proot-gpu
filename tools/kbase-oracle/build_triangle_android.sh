#!/bin/sh

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 <android-ndk> [output-directory]" >&2
    exit 2
fi

ndk=$1
output=${2:-.}
case "$(uname -s)" in
    Darwin) host=darwin-x86_64 ;;
    Linux) host=linux-x86_64 ;;
    *) echo "unsupported build host" >&2; exit 2 ;;
esac

compiler="$ndk/toolchains/llvm/prebuilt/$host/bin/aarch64-linux-android26-clang"
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output"
glslangValidator -V "$script_dir/triangle.vert" -o "$output/triangle.vert.spv"
glslangValidator -V "$script_dir/triangle.frag" -o "$output/triangle.frag.spv"
"$compiler" -std=c11 -Wall -Wextra -Werror "$script_dir/triangle_probe.c" \
    -lvulkan -o "$output/triangle_probe"
