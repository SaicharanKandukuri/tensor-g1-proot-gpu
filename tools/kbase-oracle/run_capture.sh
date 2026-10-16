#!/bin/sh

set -eu

if [ "$#" -lt 4 ] || [ "$3" != "--" ]; then
    echo "usage: $0 <capture-library> <label> -- <command> [args...]" >&2
    exit 2
fi

library=$1
label=$2
shift 3
output=${KBASE_ORACLE_TRACE:-"$PWD/kbase-$label.jsonl"}
rm -f "$output"
export KBASE_ORACLE_TRACE=$output
export KBASE_ORACLE_LABEL=$label
export LD_PRELOAD="$library${LD_PRELOAD:+:$LD_PRELOAD}"
"$@"
if [ ! -s "$output" ]; then
    echo "no trace produced" >&2
    exit 1
fi
if ! grep -q '"name":"JOB_SUBMIT"' "$output"; then
    echo "trace contains no intercepted JOB_SUBMIT; the process may bypass libc ioctl" >&2
    exit 1
fi
echo "$output"
