# Patch series

These are the minimal source changes used by the first passing Exynos vendor
Vulkan and AHardwareBuffer WSI proof. They are kept as reviewable patches so a
release build can fetch pinned upstream sources and fail if a patch no longer
applies cleanly.

| Component | Upstream | Pinned revision | Patch |
| --- | --- | --- | --- |
| Mesa/Zink | `https://github.com/SaicharanKandukuri/tensor-g1-proot-gpu.git` | `608be7749cc9ec129d54b2e8251a0821fe4929eb` | `mesa/0001-support-khr-vertex-attribute-divisor.patch` |
| libhybris | `https://github.com/libhybris/libhybris.git` | `7079712a42ea2754adf747e70c6cc75764c8596e` | `libhybris/0001-complete-locale-and-ui-compatibility.patch`, `libhybris/0002-support-relocatable-runtime-linking.patch`, `libhybris/0003-isolate-android-q-tls-in-static-reserve.patch`, `libhybris/0004-redirect-raw-bionic-thread-pointer-reads.patch` |
| sysvk | `https://github.com/xMeM/sysvk.git` | `23ecd775ed6fe06bb5ac0063b5f981f70c543c67` | `sysvk/0001-discover-and-validate-explicit-vulkan-hal.patch` |
| xMeM WSI | `https://github.com/xMeM/vulkan-wsi-layer.git` | `d5624d42d8b2debbd910ad25662a05c751eb38b7` | `xmem-wsi/0001-port-ahb-x11-wsi-to-glibc-and-rgba-semantics.patch`, `xmem-wsi/0002-pass-sync-file-fences-to-x-present.patch`, `xmem-wsi/0003-negotiate-private-x11-buffer-transport.patch`, `xmem-wsi/0004-own-x11-present-thread-lifecycle.patch`, `xmem-wsi/0005-report-lost-x11-surfaces.patch`, `xmem-wsi/0006-make-swapchain-error-state-atomic.patch`, `xmem-wsi/0007-keep-filtered-layer-manifest-valid.patch`, `xmem-wsi/0008-respect-selected-build-type.patch` |
| Termux:X11 | uDroid's pinned Termux:X11 submodule | recorded by the consuming uDroid revision | `termux-x11/0001-import-rgba-ahardwarebuffer-content-without-swizzle.patch`, `termux-x11/0002-import-linux-sync-file-fences.patch`, `termux-x11/0003-use-realtime-for-timed-mutex-deadline.patch`, `termux-x11/0004-gate-gpu-copy-hot-path-logging.patch`, `termux-x11/0005-advertise-buffer-transport-protocol.patch`, `termux-x11/0006-publish-present-offload-statistics.patch` |

The Mesa patch lets the pinned Zink revision consume the ratified
`VK_KHR_vertex_attribute_divisor` capability used by newer Android vendor
drivers. The KHR feature and pipeline structures share the EXT ABI, while the
expanded KHR properties structure is queried with its own structure type. EXT
remains preferred when both forms are advertised.

The xMeM WSI and Termux:X11 patches form one private protocol revision. The X
server publishes `_UDROID_X11_BUFFER_TRANSPORT` on the root window as six
32-bit cardinals: protocol version, capability bits, BGRA modifier low/high,
and RGBA modifier low/high. Version 1 publishes modifiers `1255` and `1257`.
They are transport identifiers, not Linux DRM modifiers. xMeM refuses the
private transport when the property is absent, malformed, unsupported, or
missing a required capability.

The sixth Termux:X11 patch publishes `_UDROID_X11_PRESENT_STATS` on the same
root window. It snapshots cumulative attempts, successful GPU copies, and one
mutually exclusive fallback reason per failed attempt on the existing
five-second frame timer. These counters let an unprivileged guest prove actual
offload without parsing Android logcat.

The second Termux:X11 patch recognizes Linux `sync_file` FDs in the existing
DRI3 fence import backend while preserving ordinary xshmfence behavior. It is
the server half of the optional explicit-acquire synchronization path; the CPU
Vulkan-fence wait remains the safe client fallback.

The second xMeM patch is the paired producer half for sync-file acquire fences.
The third patch negotiates those fences automatically from the server property.
`UDROID_X11_EXPLICIT_SYNC=0` forces the validated CPU Vulkan-fence wait;
`UDROID_X11_EXPLICIT_SYNC=1` is strict and fails if the advertised peer lacks
the capability. `UDROID_X11_PROTOCOL=legacy` is a lab-only escape hatch for an
older paired server and must not be selected automatically by packaging.

The fourth xMeM patch makes the Present event worker an explicitly owned
resource. Swapchain teardown joins a constructed thread even if it has not run
yet, special-event registration is initialized and checked, and partial setup
unwinds without leaving a worker or XCB registration behind.

The fifth xMeM patch converts a failed X geometry query into
`VK_ERROR_SURFACE_LOST_KHR` for both surface-capability entry points. Without
it, a destroyed window returned success with an undefined current extent.

The sixth xMeM patch makes the base swapchain error result atomic. Presentation
workers and Vulkan application threads otherwise read and write that state
concurrently without a common lock, which is undefined behavior in C++.

The seventh xMeM patch keeps conditionally filtered extension declarations on
single lines. This matches upstream's line-oriented manifest generator and
prevents disabled features from leaving nameless extension records.

The eighth xMeM patch stops the X11 backend from overriding the project-wide
build type. Release artifacts are therefore optimized by the locked build job
instead of silently compiling the compositor hot path as Debug.

The third Termux:X11 patch fixes a separate POSIX contract at the renderer/X
server lock boundary. `pthread_mutex_timedlock()` consumes an absolute
`CLOCK_REALTIME` deadline; passing a monotonic timestamp made every contended
attempt expire immediately and spin. The patch retains the same 33 ms recovery
interval while allowing Bionic to sleep between retries.

The fourth Termux:X11 patch removes unconditional Android logging from the
per-rectangle GPU-copy hot path. The detailed coordinate and texture trace is
still available with `TERMUX_X11_DEBUG=1`, but normal desktop rendering no
longer performs a log write for every copied damage rectangle.

The second libhybris patch adds `--enable-relocatable-runtime`. It suppresses
Libtool's absolute build-prefix RUNPATH while retaining install-time `-L`
resolution. Runtime launchers must supply the component-local library path.
Release packaging must stage `make install` output, never a build tree's
uninstalled `.libs` files.

The third libhybris patch gives the embedded Android Q linker its missing
startup TLS lifecycle. A preloaded `libhybris-common` owns a fixed 64 KiB
initial-exec TLS reserve, and Bionic TPREL/TLSDESC relocations target that
reserve instead of glibc's thread state. The first Android library graph is
loaded before `main()` through `HYBRIS_TLS_PRELOAD`; the linker finalizes the
layout before its constructors and initializes the main thread. The preloaded
bridge also wraps host `pthread_create`, while libhybris' Android hook covers
vendor-created threads. The last eight reserve bytes hold the initialization
generation, leaving 65,528 bytes for the Android layout. A larger layout aborts
at startup rather than corrupting host TLS.

Shipping launchers must set both of these values to the same discovered and
validated vendor root library:

```sh
LD_PRELOAD=/opt/udroid/graphics/lib/bridge/libhybris-common.so.1
HYBRIS_TLS_PRELOAD=/vendor/lib64/hw/vulkan.DEVICE.so
```

The clean pinned build must expose a `PT_TLS` segment in
`libhybris-common.so.1`, contain no absolute `RPATH`/`RUNPATH`, and pass
`vulkan-thread-lifecycle 8 25`. That probe requires every worker to be
initialized before Vulkan entry and verifies a glibc TLS canary across 200
concurrent instance lifecycles. On the Exynos 9611 qualification device, the
same build also passed an 8 by 100 soak with no Android fatal signal or Mali
fault.

On AArch64 Android Q and newer, inline `MRS TPIDR_EL0` reads in Bionic and
vendor code are redirected to that isolated TCB. Its `TLS_SLOT_THREAD_ID`
points to a per-thread shadow so Bionic errno handling cannot dereference or
overwrite glibc's TCB. Older linker generations retain their existing path.

When `UDROID_VULKAN_HAL` is set, sysvk treats its validated absolute path as
authoritative. Without it, normal Android HAL discovery remains available. A
packager must discover the HAL only from Android's standard `vendor`, `odm`, or
`system` hardware-module directories.
