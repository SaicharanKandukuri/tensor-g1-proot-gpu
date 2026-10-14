# Tensor G1 Mali-G78 research ledger

This document records reproducible facts needed to support the Mali-G78 in
Google Tensor G1 with an open Mesa driver. It is an engineering ledger, not a
claim of conformance or support for every Mali-G78 implementation.

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
[`RandomCoderOrg/AdrenoToolsTest` commit `7106c2e`](https://github.com/RandomCoderOrg/AdrenoToolsTest/commit/7106c2e0f5dcdbb049ec0e0e0d5a0c2e1ce77145).

| Capability | Result | Evidence |
|---|---|---|
| Instance and device creation | Pass | Stock and PanVK enumerate independently in one process |
| Host-visible allocation and binding | Pass | Two 4 KiB buffers bind and map |
| Non-coherent cache maintenance | Pass | Memory flags `0xb`; explicit flush and invalidate |
| Transfer submission | Pass | 4 KiB copy hash `78b772454f481ffe` matches stock |
| Compute pipeline creation | Pass | Vulkan 1.0 compute pipeline with one storage buffer |
| Compute dispatch | Pass | 1,024 invocations; hash `c3e210fc6f652c42` matches stock |
| Compute-to-host visibility | Pass | Compute-to-host buffer barrier followed by invalidate |
| Android triangle presentation | Pass | Four-image swapchain and first presented frame observed |
| Sampled images and layout transitions | Unknown | Next deterministic probe |
| Depth, blending, and indexed drawing | Unknown | Not yet isolated |
| Vulkan conformance | Unknown | Focused dEQP coverage has not run on this branch |
| Performance and power behavior | Unknown | Correctness gates take priority |

The transfer and compute results passed on three completed cold executions.
Truncated launches that were stopped while copying the 126 MiB test ICD are not
counted.

The cache operations follow the requirements in the
[Vulkan specification](https://registry.khronos.org/vulkan/specs/latest-ratified/pdf/vkspec.pdf):
host writes to non-coherent memory are flushed before device access, and device
writes are invalidated before host comparison.

## Next probes

1. Upload a deterministic texture, sample it, render offscreen, and read it
   back.
2. Isolate depth testing, blending, indexed drawing, and image transitions.
3. Stress Android surface recreation and swapchain lifecycle independently of
   shader correctness.
4. Run focused dEQP-VK API, memory, compute, synchronization, and draw groups.
5. Add performance measurements only after the corresponding correctness gate
   is stable.

Every failed probe should first identify the boundary that diverged from stock.
Driver changes follow only after the failure is reproducible and narrower
instrumentation cannot explain it.

