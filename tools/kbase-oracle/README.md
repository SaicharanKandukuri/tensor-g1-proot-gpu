# Kbase oracle capture

This tool records the public Kbase userspace contract observed from a process
that opens `/dev/mali*`. It is a clean-room measurement aid, not a vendor
decompiler. Keep the raw trace private when provenance is uncertain, and do not
be a biblioklept: implement conclusions independently from public interfaces and
reproducible behavior.

The preload library records intercepted Kbase ioctls, allocations, CPU/GPU
mappings, JM atoms, completion events, and mapped v9 job chains. The ptrace
runner records the same public syscall contract when a vendor library bypasses
libc interposition. Runtime addresses are removed by the normalizer. The
comparison tool checks job and dependency structure while ignoring addresses,
allocation numbering, timestamps, and snapshot hashes.

## Build

For a native Android/Termux process:

```sh
tools/kbase-oracle/build_android.sh "$ANDROID_NDK_HOME" \
  /tmp/libkbase_oracle_capture.so
```

The Android build also writes `kbase_oracle_ptrace` beside the requested shared
library.

For an arm64 glibc guest, build from the repository inside that guest:

```sh
cc -shared -fPIC -std=c11 -Wall -Wextra -Werror -pthread \
  tools/kbase-oracle/kbase_oracle_capture.c \
  -o /tmp/libkbase_oracle_capture.so
```

### GLES and GLX probes

Build the deterministic GLES2 pbuffer probe with the Android NDK:

```sh
tools/kbase-oracle/build_gles_oracle_android.sh \
  "$ANDROID_NDK_HOME" /tmp/kbase-gles

/tmp/kbase-gles/gles_oracle clear
/tmp/kbase-gles/gles_oracle constant
/tmp/kbase-gles/gles_oracle varying
```

Each case reports the EGL platform, GL implementation, sample pixels, and an
RGBA8 readback hash. Compare the same case between two drivers; the probe is
not a benchmark.

Build the staged GLX drawable-lifecycle probe inside an arm64 glibc guest:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  tools/kbase-oracle/glx_stage_probe.c -lGL -lX11 \
  -o /tmp/glx_stage_probe

DISPLAY=:0 PAN_EXPERIMENTAL_KBASE_GL=1 /tmp/glx_stage_probe
```

The GLX probe names context, draw, finish, and swap boundaries, then performs
120 move-and-resize cycles so a failure can be assigned to rendering,
completion, presentation, or drawable reallocation.

## Capture

Run one deterministic workload per process. The stock and PanVK runs must use
the same probe, dimensions, shaders, resources, and synchronization:

```sh
tools/kbase-oracle/run_capture.sh \
  /tmp/libkbase_oracle_capture.so vendor-triangle -- ./triangle-probe

tools/kbase-oracle/run_capture.sh \
  /tmp/libkbase_oracle_capture.so panvk-triangle -- ./triangle-probe
```

Use the syscall runner when the stock Android driver issues direct syscalls:

```sh
./kbase_oracle_ptrace vendor-triangle.jsonl vendor-triangle -- \
  ./triangle-probe triangle.vert.spv triangle.frag.spv
```

`KBASE_ORACLE_SNAPSHOT_BYTES` controls the mapped bytes retained for each job
descriptor and defaults to 384. The runner fails when it cannot observe a
`JOB_SUBMIT`; that usually means a linker namespace or direct syscall bypassed
libc interposition. Do not treat an empty trace as driver evidence.

## Normalize and compare

```sh
python3 tools/kbase-oracle/normalize_trace.py \
  kbase-vendor-triangle.jsonl -o vendor-triangle.json
python3 tools/kbase-oracle/normalize_trace.py \
  kbase-panvk-triangle.jsonl -o panvk-triangle.json
python3 tools/kbase-oracle/compare_traces.py \
  vendor-triangle.json panvk-triangle.json -o triangle-diff.json
```

The first checkpoint passes only when both traces contain the expected Kbase
handshake, `JOB_SUBMIT`, and completion events, and both probes produce the same
readback hash. A descriptor may be marked unmapped when Android denies external
reads of a Kbase `VM_IO` mapping; that is a capture boundary, not a driver
difference. Snapshot bytes remain in the raw JSONL; add `--include-bytes` only
for private inspection.

## First validated baseline

On 2026-10-17, the same 64 by 64 offscreen triangle produced
`48e6dcff98d52603` through both the Pixel 6a stock Mali-G78 driver and the
PanVK JM build. The stock path submitted seven atoms and returned seven `DONE`
events. PanVK submitted three atoms and suppressed successful intermediate
events, so only its last atom returned `DONE`.

The stock graphics atom used `compute_shader`, `tiler`, `cache_flush`, and
`coherent_group` requirements. PanVK used `compute_shader`, `tiler`, and
`value_writeback`, with `event_only_on_failure` on intermediate atoms. This is
an observed scheduling difference, not proof that any one flag should be copied.
The next failing workload must identify a specific completion fault before a
driver change is justified.

The current PanVK build also completed one-second 640 by 480 XCB runs of all
seven `vkmark` scenes: vertex, texture, shading, effect2d, desktop, cube, and
clear. Every completion recorded during those runs was `DONE`. These are
stability probes, not performance comparisons with the stock driver.

## Current boundary

The tracer walks `next` links in job headers and captures a bounded descriptor
window. Android returned `EFAULT` from `process_vm_readv` and `EIO` from
`PTRACE_PEEKDATA` for the stock driver's Kbase job pages, while the in-process
PanVK tracer could read its own pages. Therefore stock descriptor payloads are
not compared yet. Add an in-process capture assistant or a narrowly targeted
Ghidra observation only after a failing atom and descriptor family are known.

Core-requirement and completion names come from Arm's public GPL syscall-note
JM UAPI imported into Mesa by commit `54377feea905ee68816e3d029d496cdb1083a444`.
No decompiler output or proprietary code is copied into this tool.
