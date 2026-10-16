# Tensor G1 Mali-G78 research ledger

This document records reproducible facts needed to support the Mali-G78 in
Google Tensor G1 with an open Mesa driver. It is an engineering ledger, not a
claim of conformance or support for every Mali-G78 implementation.

The maintained work order and release gates are in the
[Tensor G1 driver roadmap](tensor-g1-gpu-roadmap.md). This ledger records
evidence; the roadmap decides what that evidence is sufficient to claim.

## Evidence rules

Each finding must be marked as one of:

- **Observed:** measured on identified hardware with a reproducible probe.
- **Public reference:** stated by a public specification or source repository.
- **Inferred:** the smallest explanation consistent with current observations.
- **Unknown:** not yet established.

Keep observation and implementation separate. Record the device, software
build, probe source revision, driver digest, result, and failure boundary. A
result from one Pixel build must not silently become a general Valhall rule.

## Provenance boundary

This project follows a conservative clean-room boundary inspired by the
[Asahi Linux copyright and reverse-engineering policy](https://asahilinux.org/copyright/):

- Use public specifications, public source code, and behavior measured on
  hardware we own or are authorized to test.
- Describe observed interfaces and behavior in original words. Do not paste
  proprietary disassembly, decompiler output, shader compiler sequences,
  firmware, encryption keys, or vendor binaries into the repository.
- Do not be a biblioklept: code taken from compatible open-source projects
  must retain its copyright, license, author, repository, and commit reference.
- Keep hashes and reproduction commands for locally inspected artifacts rather
  than redistributing those artifacts.
- Use Ghidra only as private supporting evidence when public sources and black-
  box probes cannot answer a focused question. Publish only independently
  corroborated interface facts, never recovered implementation text.
- Quarantine material with uncertain origin or license until its provenance is
  resolved.

These rules reduce provenance and copyright risk; they are not legal advice.

## Target snapshot

Observed on 2026-10-14:

| Field | Value |
|---|---|
| Device | Google Pixel 6a (`bluejay`) |
| Android build | `google/bluejay/bluejay:17/CP3A.260905.009/16091614:user/release-keys` |
| Kernel | `6.1.162-android14-11-g2ec90535fa34-ab15810641` |
| Stock Vulkan device | `Mali-G78`, API 1.4.305, driver 54.3.0 |
| Experimental device | `Mali-G78 MC20`, API 1.3.363, Mesa 26.2.99 |
| Experimental ICD SHA-256 | `8ad4abe703ebe0a31df4980c441b6cc797ef2c5a3e03d72c6638316a47971850` |
| Kernel interface | `/dev/mali0`, proprietary kbase Job Manager path |

The ordinary ADB shell could not read the Mali sysfs identity or frequency
files on this build. Their contents remain unknown rather than inferred.

## Current driver boundary

```text
Vulkan application
  -> Android Vulkan loader and HAL
  -> Mesa PanVK for Valhall v9
  -> kbase Job Manager userspace ABI
  -> /dev/mali0
  -> Mali-G78

Android presentation
  -> VK_KHR_android_surface
  -> ANativeWindow / gralloc buffer
  -> Android compositor
```

The public references for these boundaries are the
[AOSP Vulkan HAL interface](https://android.googlesource.com/platform/frameworks/native/+/324b700b4b0669683efff3d696324dc0d4e24ec5/vulkan/include/hardware/hwvulkan.h),
Google's public
[Bluejay kbase kernel tree](https://android.googlesource.com/kernel/google-modules/gpu/+/refs/heads/android-gs-bluejay-6.1-android15-qpr2/),
and Arm's public
[Valhall architecture material](https://developer.arm.com/Downloads/-/Valhall%20Mali%204th%20Gen%20GPU%20Architecture).

## Validation ledger

Probe source:
[`RandomCoderOrg/AdrenoToolsTest` commit `8418775`](https://github.com/RandomCoderOrg/AdrenoToolsTest/commit/841877521a8dea463f6eac1b6f22dcab1368cb2b).

| Capability | Result | Evidence |
|---|---|---|
| Instance and device creation | Pass | Stock and PanVK enumerate independently in one process |
| Host-visible allocation and binding | Pass | Two 4 KiB buffers bind and map |
| Non-coherent cache maintenance | Pass | Memory flags `0xb`; explicit flush and invalidate |
| Transfer submission | Pass | 4 KiB copy hash `78b772454f481ffe` matches stock |
| Compute pipeline creation | Pass | Vulkan 1.0 compute pipeline with one storage buffer |
| Compute dispatch | Pass | 1,024 invocations; hash `c3e210fc6f652c42` matches stock |
| Compute-to-host visibility | Pass | Compute-to-host buffer barrier followed by invalidate |
| Optimal RGBA8 image transfer | Pass | 31x29x2, row length 32, image height 30; exact 7,680-byte hash `a33ce690b1b676d8` matches stock |
| Nearest fragment texture sampling | Pass | Source layer 1 to 31x29 output; padded readback hash `4eb3b8d769b7221b` matches stock |
| Indexed drawing | Pass | Indexed draw hash `ad3634ff7218d241` matches stock |
| Additive blending | Pass | Additive blend hash `fc279fbf3be0e4d0` matches stock |
| Depth testing | Pass | Depth-test hash `42cffa14ad89a89f` matches stock |
| Android triangle presentation | Pass | Four-image swapchain and first presented frame observed |
| Vulkan conformance | Unknown | Focused dEQP coverage has not run on this branch |
| Performance and power behavior | Unknown | Correctness gates take priority |

**Observed:** the buffer, compute, odd-layout image-transfer, and layer-1
texture-sampling probes passed on three completed cold executions using the
device build and driver digest in the target snapshot. Image transfer used
distinct `0xa6` upload and `0x5a` readback padding sentinels and compared the
full 7,680-byte layout. Texture sampling preserved its padded readback and
`0x3c` guard region. The legacy buffer and compute hashes were unchanged, the
first frame presented, and captured fault scans were clean. Truncated launches
that were stopped while copying the 126 MiB test ICD are not counted.

**Observed:** indexed drawing, additive blending, and depth testing matched the
stock driver on three of three cold executions on
`google/bluejay/bluejay:17/CP3A.260905.009/16091614:user/release-keys` using the
driver digest in the target snapshot. Prior hashes were unchanged, the probe
reported ready and presented its first frame in every run, and captured fault
scans were clean.

**Observed:** PanVK did not diverge from the stock driver in the image or draw-
state probes, so no driver patch was needed for these checkpoints.

**Observed:** on the same Pixel 6a snapshot, the focused Android Vulkan CTS
harness passed `alpha_to_one` with 1, 4, and 8 samples and
`clip_distance.vert.1` through `.8`. Separate boundary runs passed explicit
fragment reads for clip-distance counts 1, 5, and 8. The repeated 11-case run
used APK SHA-256
`531cd0e80214b4202530ac31466c5d18fe6c02097f626ae027a4bf048800f05d`
and case-list SHA-256
`cb6b7064965dae5f0b3f27449e06dfb5e17833b66cf0daad9c3dc516166bd291`.
These focused passes support the current feature checkpoint; they do not replace
complete clipping and multisample CTS coverage.

The cache operations follow the requirements in the
[Vulkan specification](https://registry.khronos.org/vulkan/specs/latest-ratified/pdf/vkspec.pdf):
host writes to non-coherent memory are flushed before device access, and device
writes are invalidated before host comparison.

## PRoot X11 presentation boundary

Observed on 2026-10-17 using Pixel 6a build
`google/bluejay/bluejay:17/CP3A.261005.002.A1/16269273:user/release-keys`,
kernel `6.1.162-android14-11-g9dd05ae3a1de-ab16261587`, and frozen PanVK ICD
SHA-256
`a29c14232a3b8b0c1e3eb5ed67bd6b986f55aac30904d657889b8f3b0ca761cd`:

- `/dev/dma_heap/system` was readable but not writable by the application
  domain (`cr--r--r--`, owner `system:system`). Opening it with `O_RDWR`
  failed before the allocation ioctl; `O_RDONLY` allowed the ioctl to return
  DMA-BUF file descriptors with the requested access mode.
- Matched offscreen triangle runs ruled out a universal JM throughput limit.
  Stock measured 694.028, 548.536, and 542.463 frames/s; PanVK measured
  1163.697, 1902.255, and 1750.726 frames/s after an identical warm-up, with
  identical shaders and output hash.
- A four-way 640 by 480 XCB probe isolated two independent requirements. The
  unmodified path used shared-memory presentation and measured 266 frames/s.
  DMA-heap access alone created four heap buffers and four kbase imports but
  failed the redundant implicit-sync import with `EINVAL`. Suppressing only
  that import retained the shared-memory path at 259 frames/s. Combining both
  behaviors used four heap buffers and four kbase imports and measured 1305
  frames/s.
- The same behavioral combination completed all seven one-second fullscreen
  `vkmark` scenes at aggregate score 982, compared with 135 on the shared-
  memory fallback. These short stages were boundary probes, not production
  benchmark samples.

**Inferred:** the dominant regression in this configuration is the X11 shared-
memory fallback plus an incompatible redundant sync-file import, not raw JM
submission throughput. The WSI already waits for the per-image fence before
queueing presentation when `wait_present_before_queue` is enabled, so creating
another implicit-sync import is unnecessary on that path.

**Limit:** the four-way comparison used temporary preload behavior against the
frozen ICD. The source changes have not yet been rebuilt into that exact driver.
They remain a research checkpoint until the rebuilt binary passes repeated
resize, move, unmap, image-reuse, present-mode, and lifecycle tests without a
fault, stale frame, or early presentation.

## Next probes

1. Rebuild and repeat the four-way presentation control with the source patch.
2. Stress Android and X11 surface recreation, image reuse, resize, and lifecycle
   independently of shader correctness.
3. Run focused dEQP-VK API, memory, compute, synchronization, and draw groups.
4. Add performance measurements only after the corresponding correctness gate
   is stable.

Every failed probe should first identify the boundary that diverged from stock.
Driver changes follow only after the failure is reproducible and narrower
instrumentation cannot explain it.
