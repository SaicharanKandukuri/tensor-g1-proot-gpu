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
"$compiler" -std=c11 -Wall -Wextra -Werror "$script_dir/gles_oracle.c" \
    -lEGL -lGLESv2 -o "$output/gles_oracle"
