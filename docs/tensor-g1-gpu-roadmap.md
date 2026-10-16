# Tensor G1 Mali-G78 driver roadmap

This roadmap defines the gates for turning the experimental Tensor G1 PanVK
path into a robust driver. The [research ledger](tensor-g1-gpu-research.md)
holds observations and immutable test evidence; this document holds work and
exit criteria.

> **Working draft — not for publication:** This roadmap is AI-authored. Per
> `AGENTS.md`, a human maintainer must independently verify it and rewrite or
> explicitly adopt it before any part is published.

## Scope

The target is Google Tensor G1's Mali-G78 using the proprietary kbase Job
Manager (JM) interface and Android presentation. Initial hardware coverage is
the Pixel 6a snapshot recorded in the research ledger. Pixel 6 and Pixel 6 Pro
are required portability targets before release.

This roadmap does not claim support for every Mali-G78, every kbase release,
or the kbase Command Stream Frontend (CSF). The Pixel 7/G710 results in the
[general kbase notes](panvk-kbase.md),
[CSF triangle analysis](kbase-triangle-hang-analysis.md), and
[CSF Minecraft analysis](kbase-minecraft-fault-analysis.md) are design input,
not Tensor G1 proof. OpenGL through Zink and application compatibility are
downstream acceptance tests, not substitutes for Vulkan correctness.

## Independent tracks

Progress in one track does not advance another track automatically.

| Track | Question | Evidence needed |
|---|---|---|
| Hardware and ABI knowledge | Do we understand the exact Tensor G1 GPU and shipping JM contract? | Public kernel or architecture reference plus a target-bound probe |
| Execution correctness | Are memory, submission, synchronization, faults, and lifetimes correct? | Deterministic stress probes with clean kernel and driver fault scans |
| Vulkan conformance | Does every advertised Vulkan behavior match the specification? | Focused dEQP-VK results followed by a maintained must-pass list |
| Android integration | Do HAL loading, gralloc, fences, surfaces, and lifecycle transitions remain correct? | Repeated on-device lifecycle tests with leak and presentation checks |

## Milestone gates

`Pass` always means the evidence names the device fingerprint, kernel, driver
digest, probe revision, repetition count, result, and retained artifact.

The [Vulkan specification](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/)
and applicable
[dEQP-VK](https://github.com/KhronosGroup/VK-GL-CTS/tree/main/external/vulkancts)
tests are the acceptance authorities. Stock-driver behavior is diagnostic
evidence only; it cannot override the specification or turn a shared bug into
a pass.

| Gate | Current state | Exit criteria |
|---|---|---|
| M0: ABI and provenance prerequisite | Partial; the JM submit/event/fence contract is typed, asserted, and target-tested; the full custom-delta and import-provenance ledger remains | Reproduce the build; map every custom delta by subsystem; identify every imported public source by license, repository, path, and commit; bind each used JM ioctl layout to exact public source when available, or to the exact runtime version plus adjacent authoritative public versions, compile-time size/offset/alignment assertions, and a safe on-target contract probe; retain build, driver, kernel, and probe hashes |
| M1: JM asynchronous synchronization and lifetime | Exploratory 10-run stress passed; full gate remains incomplete and gated by M0 | Pass the checkpoint below; propagate rejected work, poll failure, timeout, and context loss without false success; prove atom reuse, queue ordering, fence ownership, allocation, mapping, and process teardown without leaks |
| M2: memory, VA, and cache correctness | Partial, narrow probes only | Pass host-visible and device-local allocation, bind, alias, suballocation, map/unmap, flush/invalidate, non-coherent, image-layout, padded-copy, VA reuse, use-after-free rejection, and bounded out-of-memory tests with exact guards and no stale data or mapping |
| M3: compiler and descriptors | Partial, narrow shaders only | Pass focused SPIR-V/NIR, scalar and vector ALU, control-flow, texture, image, atomic, push-constant, descriptor-type, array, dynamic-offset, update-after-bind where advertised, pipeline-cache, and shader-stage dEQP groups; every compiler workaround has a reduced regression |
| M4: advertised-feature truth | Known blocker: multiview is advertised without target proof | Inventory every core feature, extension, property, limit, format, and external-handle capability; for each advertised item, identify its implementation boundary and passing target test, otherwise disable it; resolve every ledger row before conformance claims |
| M5: focused and full CTS | Not started | Pass the focused dEQP-VK groups from M2-M4, then the complete applicable Vulkan CTS for the declared API/profile on the exact release build; retain the must-pass list and raw results with no unexplained fail, crash, timeout, or quality warning |
| M6: Android WSI and lifecycle | First frame only | Pass at least 100 surface create/destroy cycles and 100 resize, rotate, background/foreground, screen-off/on, and swapchain-recreation transitions; correctly handle out-of-date, suboptimal, and surface-lost results; verify acquire/present fences, formats, colorspaces, transforms, and gralloc layouts |
| M7: reliability and recovery | Unknown | Complete repeated process death, device-loss propagation, allocation-pressure, app restart, suspend/resume, and long-running graphics/compute stress without kernel panic, silent corruption, false success, unbounded resource growth, or unrecoverable display state |
| M8: performance and power | Unknown | Under matched workloads, measure frame pacing, CPU cost, memory bandwidth, DVFS, thermal throttling, idle power, and suspend/resume against stock; explain material regressions and meet recorded release budgets without weakening correctness |
| M9: portability and release | Not started | Pass M0-M8 on Pixel 6a and the applicable gates on Pixel 6 and Pixel 6 Pro across the declared Android/kernel matrix; produce a reproducible package, rollback path, known-limitations list, and evidence bundle |

### Current M0 evidence

- **Observed:** Pixel 6a fingerprint
  `google/bluejay/bluejay:17/CP3A.260905.009/16091614:user/release-keys`,
  kernel `6.1.162-android14-11-g2ec90535fa34-ab15810641`.
- **Observed:** the safe `VERSION_CHECK` handshake negotiated JM uAPI 11.46.
  The probe binary SHA-256 is
  `a17c25be2fd0a9100977461859902fe92bd897a6790c2e3e56d746ee681a8bf7`.
- **Public reference:** the fingerprint's GKI commit is publicly available as
  [`2ec90535fa348d27c0a545b05c3badc7fa8ecf68`](https://android.googlesource.com/kernel/common/+/2ec90535fa348d27c0a545b05c3badc7fa8ecf68/),
  but no exact public Android 17 Bluejay `mali_kbase` module source was found.
- **Public reference:** Google's official Bluejay GPU history brackets the
  runtime version with
  [r49p0 `7ec5b8a0bf8a4327ee92240521cc5f4cee67e5ca`](https://android.googlesource.com/kernel/google-modules/gpu/+/7ec5b8a0bf8a4327ee92240521cc5f4cee67e5ca/)
  at uAPI 11.45 and
  [r50p0 `113ad20905c853a196ba68ac764d882c883ccc06`](https://android.googlesource.com/kernel/google-modules/gpu/+/113ad20905c853a196ba68ac764d882c883ccc06/)
  at uAPI 11.47. The 11.46 change within that bracket removes v2
  `renderpass_id`; `JOB_SUBMIT`, the v3 64-byte layout and offsets, and the
  event-v2 24-byte layout remain stable across the bracket.
- **Bounded conclusion:** the current 64-byte v3 atom layout is publicly
  bracketed. M0 can close without unavailable exact shipping module source once
  build/import provenance and compile-time plus safe runtime contract assertions
  are complete. Scheduler, MMU, cache, failure, and recovery behavior remain
  unbound: M1, M2, and M7 must establish them by black-box evidence, never by
  inference from the version bracket.
- **Implemented:** commit `6e4c02996a0002194959930d66f00676d5525011`
  replaces raw byte-offset JM atoms with the public v3 types and compile-time
  size, alignment, offset, ioctl, and constant assertions. Independent review
  found it byte-equivalent to the old construction and consistent with both
  public bracket endpoints.
- **Implemented:** commit `1562845de8758b57a9674d839afa10c9e07e198b`
  rejects non-4-KiB host page sizes before kbase context creation. The current
  allocator, alias, import, tracking-page, and page-count paths all encode 4 KiB
  units, so accepting another host page size would be unsafe.
- **Implemented:** commit `a7f5d4531882d340ace89d9912618b7394a980e5`
  adds compile-time assertions for the 64-bit common/JM ioctl envelopes,
  structures, used member offsets, and constants reached by the Tensor G1
  kbase backend. It intentionally excludes CSF queue contracts and
  `MEM_ALLOC_EX`, which this target path does not use.
- **Observed:** the post-ABI-gate ARM build SHA-256 is
  `54e9b2cd92be47d2433e1d3e04a356616b44663c6553e6d757f8bc11d0c73514`.
  On the recorded Pixel target it passed the stock and PanVK 4,096-submission
  workloads, the 121-job `119+2` split, 64 same-queue binary-semaphore reuse
  cycles, two concurrent host fence waiters, and all current buffer, compute,
  image, sampling, indexed-draw, blend, and depth probes. The final trace
  reported 4,362 atoms allocated and freed, 4,107 reuses, and zero failed JM
  events.
- **Provenance audit:** the historical squash `ed1979926b0` is not adequately
  described by its `Based-on: 4faddb4` line. Its strongest public source match
  is [`funnymdzz/mesa@6598829`](https://github.com/funnymdzz/mesa/commit/6598829019c0746aa8e473b4ae1c980cbfa6ea4b),
  whose custom delta from merge parent `84f0901` overlaps 59 paths, including
  25 byte-identical files. The squash also adds 39 paths outside that public
  delta, chiefly JM/v9 and Android integration, whose exact authorship cannot
  be recovered from the current history.
- **Release blocker:** imported source attribution, the 39 local-only paths,
  unlicensed newly added files, and conflicting inherited copyright headers
  require human verification. Preserve existing notices until their origins
  are established. The unrelated local CSF UAPI also needs a separate audit;
  it cannot be used as Tensor G1 JM evidence.
- **Remaining:** create a human-verified import/provenance manifest and replace
  or split the historical squash into attributed public import, current-Mesa
  adaptation, and independently traceable local JM/v9 work before declaring M0
  complete or preparing a release.

## Active checkpoint: JM asynchronous synchronization and lifetime (M1)

Do not start a counted M1 run until M0 has completed the JM-specific build and
import provenance and verified every used ioctl structure size, member offset,
alignment, flag, and userspace/kernel bitness assumption with compile-time
assertions and the safe Pixel 6a contract probe. Earlier runs remain exploratory
and cannot satisfy M1.

Use one fixed Pixel 6a target snapshot and one driver/probe pair for the whole
checkpoint. A run is not counted if deployment, setup, or capture is truncated.

1. Complete 10 cold executions, each in a fresh application process.
2. In each execution keep 256 one-time command buffers live and submit them in
   16 fenced waves, for 4,096 command-buffer submissions total. All waves must
   preserve a single global deterministic sequence. Each wave must submit all
   256, attach a fence only to its final queue submission, and never
   use `vkQueueWaitIdle` between submissions or waves. Reuse a wave's command
   buffers only after its fence completes.
3. A counted pass requires trace records of atom allocation and completion,
   reuse across the 255 usable atom numbers, the wrap point, and the live-atom
   high-water mark. Timing, application output, or elapsed work alone is not
   atom-allocation evidence.
4. Separately submit one dependency chain that expands to more than 120 kbase
   jobs. The current implementation is expected to split a 121-job chain as
   119+2, but that split is diagnostic rather than the pass condition: exact
   output and trace telemetry must prove ordered split and completion.
5. Make the final workload write a unique sentinel. Its final Vulkan fence must
   complete within a bounded wait. Read and compare the sentinel only after the
   wait succeeds and the required host invalidation has completed; an immediate
   post-enqueue unsignaled observation is not required.
6. First validate Vulkan binary fences and binary semaphores, including reset,
   reuse, queue ordering, concurrent host waiters, and ownership through every
   success and error path. For each external FD later introduced, record its
   producer, importer, owning closer, duplication, and exact close point.
7. Test external `sync_file` only after a verified producer is available: an
   Android acquire fence or the public `sw_sync` test interface. Name that
   producer and the responsible closer, introduce a measured signal delay, and
   show the dependent GPU submission stays incomplete for that delay. For an
   exported GPU `sync_file`, name its closer and prove it is not signaled early.
8. Exercise timeline semaphores only if they remain advertised after M4.
   Exercise events, queue idle, fence reset/reuse, and concurrent host waiters.
   Judge results against the Vulkan specification and dEQP-VK; use stock only
   to localize a divergence.
9. Run the synchronous-submit and CPU fence wait/export modes as correctness
   controls. Run the deliberately broken no-fence-dependency and early-export
   modes only as negative controls; the probe must detect their ordering error.
10. Test rejection with a safe userspace validation case, an ioctl payload the
   kernel rejects before queueing, or a mock-kmod test. Do not submit a
   deliberately malformed GPU job on the phone. Defer real MMU/GPU fault
   injection until ordinary device-loss reporting and contained recovery are
   proven. A rejected operation or bounded timeout must not advance a successful
   sequence or signal a successful fence.
11. Compare file-descriptor count, mapped-region count, and process memory before
   steady state and after 4,096 submissions. There must be no monotonic growth,
   no leaked fence payload, no stale mapping after process exit, no kbase fault,
   no Android tombstone or ANR, and no device reboot.

Retain the raw supervisor log, probe output, fault scan, resource-count samples,
and result hashes for every counted execution. A clean happy path does not
satisfy the checkpoint until the injected failures are also contained.

### Current M1 evidence (exploratory)

- **Observed:** evidence directory
  `/tmp/tensor-g1-jm-async-20261014-1518`; summary SHA-256
  `5314b3abd8a9db8aee94c5ae3bbda394169e4cfed323e7230c3f8a0c42d3e861`,
  APK SHA-256
  `cd23a2f7c37fd10b1ac975a6adfc8cfad6b6c5ff6478d42c61d4989271ff3235`,
  and driver SHA-256
  `444f5f34c512ede03910d9e9327a888c9fd56b8cce579456ed9fa3dd48698a6f`.
- **Observed:** all 10 cold stock/PanVK run pairs passed the 4,096-command-buffer
  16-wave workload and the separate 121-job dependency chain.
- **Observed:** every PanVK run reported `atom_allocated=4222`,
  `atom_freed=4222`, `reused=3967`, `wraps=16`, `pressure_waits=16`,
  `pressure_iterations=16`, `max_live=255`, `failed_events=0`, and the
  atom-limit split `119+2`.
- **Observed, not a leak verdict:** each cold PanVK process reported FD delta 0,
  `VmSize` delta 0, map-line delta +2, and RSS delta 1,268–2,340 kB. Cold
  process comparisons cannot establish monotonic growth, and one mapping can
  span multiple map lines. A leak decision requires repeated cycles within the
  same process and a trend over those cycles.
- **Observed:** probe commit
  [`ed3a5bcdc0f9b6212c076e419c1da5db770d7b4b`](https://github.com/RandomCoderOrg/AdrenoToolsTest/commit/ed3a5bcdc0f9b6212c076e419c1da5db770d7b4b)
  completed 64 same-queue binary-semaphore signal/wait reuse cycles and two
  simultaneous host waits on the same fence under both stock and PanVK. The
  PanVK run freed all 4,362 allocated atoms and reported zero failed events.
  The validated APK SHA-256 is
  `5c645e6c8faed10ff120eb84555fc83fc4764617795b7582ba5c9a81dcd0280d`;
  it used driver SHA-256
  `62b15b72279b08bf40d535b733c23e25cf775b4116ee9d9989131be260eea8f6`.
  Because JM exposes one queue, this is lifecycle and waiter evidence, not
  independent cross-queue ordering proof.
- **Observed:** probe commit
  [`33ad2e629d0bfe4db8b1a1dd222ea31e97852f56`](https://github.com/RandomCoderOrg/AdrenoToolsTest/commit/33ad2e629d0bfe4db8b1a1dd222ea31e97852f56)
  adds post-warmup same-process sampling during the existing submission and
  semaphore workloads. Three cold Pixel runs reported exact PanVK stability in
  file descriptors, VMA count, total mapped bytes, and `VmSize` across both
  measured phases, with zero failed JM events. The stock control exhibited a
  late bounded lazy-allocation step in some runs; it is diagnostic and does not
  grade PanVK. Evidence is retained in
  `/tmp/tensor-g1-resource-trend-20261014-1636`.
- **Observed:** probe commit
  [`268ff2ae4ca6b8a8b4660fa79f8676a7687d49b6`](https://github.com/RandomCoderOrg/AdrenoToolsTest/commit/268ff2ae4ca6b8a8b4660fa79f8676a7687d49b6)
  adds a same-process Vulkan object-lifecycle gate. After at least 16 warmup
  cycles, each run completed 128 measured create, two-submit dependency,
  bounded fence wait, GPU-written sentinel verification, and destroy cycles.
  Three cold Pixel 6a runs reported exact PanVK stability in file descriptors,
  VMA count, total mapped bytes, and `VmSize`; the existing 2,048-submit and
  binary-semaphore resource gates also remained stable, and no device loss,
  Mali/MMU fault, tombstone, or ANR appeared. The APK SHA-256 is
  `15a5c14e206a8a175ab46bf878cac85ed499d8e20b338128ae6a1d9a03558138`;
  the driver SHA-256 is
  `54e9b2cd92be47d2433e1d3e04a356616b44663c6553e6d757f8bc11d0c73514`.
  The stock driver remains a diagnostic control and does not grade PanVK.
- **Observed:** harness commit
  [`e25d98affcb0132835361185aa72fd7314b5dada`](https://github.com/RandomCoderOrg/AdrenoToolsTest/commit/e25d98affcb0132835361185aa72fd7314b5dada)
  completed 10 HOME-triggered presenter teardowns, GPU-quiescent force-stops,
  and cold recoveries, followed by an eleventh successful launch. Every HOME
  path produced balanced PanVK atom telemetry with zero failed events, reduced
  per-process DMA-BUF accounting to zero, and settled total per-process GPU
  accounting from about 76.15 MiB to the 3.06 MiB loader floor. Every
  subsequent force-stop removed the exact old `/proc` generation and
  `kprcs/<pid>` entry before relaunch; the device did not reboot and the fault
  scan remained clean. Installed and local APK SHA-256 values matched
  `15a5c14e206a8a175ab46bf878cac85ed499d8e20b338128ae6a1d9a03558138`.
  Evidence is retained in
  `/tmp/tensor-g1-process-teardown-20261014-final`. This proves removal of the
  old process's kbase accounting entry after graceful Vulkan teardown; it does
  not prove forced cleanup of live Vulkan work or reclamation of every shared,
  deferred, or globally accounted kernel allocation.
- **Incomplete:** safe error-path rejection and propagation, repeated
  active-context process-death cleanup, and an independently delayed semaphore
  dependency remain. The quiescent teardown result does not establish general
  leak freedom. This checkpoint therefore does not pass M1.

### Current M6/M8 evidence (exploratory)

- **Observed:** probe commit
  [`1260b968216326fc6b1e4c9840a157b0f6b876a8`](https://github.com/RandomCoderOrg/AdrenoToolsTest/commit/1260b968216326fc6b1e4c9840a157b0f6b876a8)
  replaces the presentation triangle with a bounded full-resolution procedural
  raymarch scene and overlays successful present-call rate, CPU-side present
  interval, surface extent, and Vulkan device name. On the Pixel 6a, three cold
  launches and one background/foreground cycle rendered successfully with both
  PanVK resource gates stable and no observed GPU/MMU fault, device loss,
  tombstone, or ANR. A sampled run reported about 19 present calls/s and a
  52 ms CPU-side interval at 1080x2263.
- **Limit:** this is a visual stress and lifecycle observation, not GPU timing,
  display FPS, utilization, frame-pacing, power, conformance, or a pass for M6
  or M8.
- **Observed:** a Pixel 6a four-way XCB control separated DMA-heap access from
  implicit-sync handling. The unchanged shared-memory path measured 266
  frames/s; sync suppression alone retained that fallback at 259 frames/s;
  DMA-heap access alone reached four allocations and imports but stopped on an
  `EINVAL` sync-file import; both behaviors together used four imported DMA-
  BUFs and measured 1305 frames/s. A seven-scene fullscreen control improved
  from aggregate `vkmark` score 135 to 982.
- **Limit:** those controls simulated the source behavior with a temporary
  preload against a frozen ICD. Exact rebuilt-driver lifecycle, reuse, resize,
  present-mode, pacing, thermal, and power evidence is still required. M6 and
  M8 remain incomplete.

## Clean-room evidence pipeline

1. Start with the authoritative Vulkan specification and dEQP-VK suite linked
   above, the
   [AOSP Vulkan HAL](https://android.googlesource.com/platform/frameworks/native/+/324b700b4b0669683efff3d696324dc0d4e24ec5/vulkan/include/hardware/hwvulkan.h),
   Google's public
   [Bluejay kbase kernel source](https://android.googlesource.com/kernel/google-modules/gpu/+/refs/heads/android-gs-bluejay-6.1-android15-qpr2/),
   and Arm's public
   [Valhall architecture material](https://developer.arm.com/Downloads/-/Valhall%20Mali%204th%20Gen%20GPU%20Architecture).
2. State one narrow unknown and design an observable stock-versus-PanVK probe
   before inspecting proprietary code.
3. Record the locally inspected artifact's source, build identifier, and hash.
   Do not add the artifact to the repository.
4. Use Ghidra privately only when public sources and black-box probes cannot
   answer the narrow question. Do not publish decompiler output, disassembly,
   recovered implementation text, or proprietary data structures.
5. Record only an independently worded interface fact, then corroborate it with
   a public source or an owned-device behavior probe.
6. Quarantine any result with uncertain origin or license. Do not be a
   biblioklept: retained open-source work must keep its license, author,
   repository, path, and commit reference.

An implementation change follows only after the failure boundary is
reproducible. Private reverse-engineering output is supporting evidence, never
the source text for a patch.

## Maintained ledgers

### Advertised features

Track every core feature and extension exposed on v9 with these fields:

| Feature or extension | Advertised | Implementation boundary | Required dEQP group | Target evidence | Decision |
|---|---:|---|---|---|---|
| Multiview | No on v9 | v9 JM graphics and layer selection | Multiview plus dynamic rendering | View masks and input attachments pass, but instanced multiview fails image comparison | Disabled until the v9 instancing path passes |
| `alphaToOne` | Yes | PanVK multisample blend state | Pipeline multisample alpha-to-one | Samples 1, 4, and 8 passed twice on the Pixel 6a checkpoint build | Retain for focused CTS expansion |
| `shaderClipDistance` | Yes | NIR fragment discard using interpolated compact clip varyings | User-defined clip-distance cases | Vertex cases 1-8 and fragment-read cases 1, 5, and 8 passed on the Pixel 6a checkpoint build | Experimental until primitive and multisample coverage passes |

Add rows before changing feature advertisement. Source comments or success in
an unrelated application are not target evidence.

### Device matrix

| Device | Submission interface | Role | Current evidence |
|---|---|---|---|
| Pixel 6a / Tensor G1 / Mali-G78 | kbase JM | Primary development target | Narrow buffer, compute, image, draw, and first-frame probes pass on one recorded build |
| Pixel 6 / Tensor G1 / Mali-G78 | kbase JM | Required release target | Unknown |
| Pixel 6 Pro / Tensor G1 / Mali-G78 | kbase JM | Required release target | Unknown |
| Pixel 7 / Tensor G2 / Mali-G710 | kbase CSF | Reference only | Separate CSF notes; not evidence for JM |
| Other Mali-G78 devices | Unknown until identified | Out of current release scope | Unknown |

### Failure corpus

Every divergence gets a stable ID and one row containing: target fingerprint,
driver/probe revision, minimal reproduction, stock result, PanVK result, first
diverging boundary, kernel and userspace signatures, retained artifact, fix
revision, and regression test. Failed experiments stay in the corpus; do not
rewrite them as conclusions.

## Release criteria

A release candidate requires all of the following:

- M0-M9 are `Pass` for the declared target matrix, with no gate satisfied by
  CSF-only or different-device evidence.
- The advertised-feature ledger contains no unowned or untested `Yes` entry.
- The active must-pass dEQP list is reproducible and has no unexplained fail,
  crash, timeout, or quality warning.
- Lifecycle, fault-injection, stress, and power artifacts identify the exact
  binaries and devices used.
- No unresolved kernel panic, silent corruption, early fence signal, leaked
  object, or false-success path remains.
- Installation and rollback are reproducible, and experimental controls are
  off by default in the release package.

Performance work may improve a passed implementation, but it cannot waive a
correctness, conformance, lifecycle, provenance, or safety gate.
