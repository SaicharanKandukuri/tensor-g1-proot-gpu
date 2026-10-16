<!-- biblioklept -->

# PanVK on the proprietary Mali kbase driver

This branch provides an experimental PanVK Vulkan ICD for Arm Mali GPUs using
the proprietary `kbase` kernel driver. It supports the CSF path and the
branch-specific v9 Job Manager path used by Tensor G1. It does **not** use the
upstream `panthor` or legacy `panfrost` DRM kernel interfaces. It is intended
for arm64 Linux environments such as chroot, PRoot, DroidSpaces, and similar
containers running on Android, provided that the required device nodes are
accessible.

This is an experimental driver. Support has only been confirmed on the devices
listed below and should not be interpreted as support for every Mali GPU or
kbase kernel release.

## KRAID shader compiler

Release packages include Mesa's upstream KRAID Rust shader compiler for
Valhall and newer Mali GPUs. Upstream keeps compiler selection opt-in while
coverage is expanded. Select stages with `PAN_USE_KRAID`; for example:

```sh
PAN_USE_KRAID=all vulkan-app
```

Stage names such as `vs`, `fs`, `cs`, and `internal` can be combined. Leave
the variable unset to use the established Bifrost compiler path.

## Tested hardware

- Mali-G78 MC20 in Google Tensor G1 (Pixel 6a), with Ubuntu 24.04 in PRoot.
  Buffer, compute, image, draw, X11 presentation, and focused `vkmark` tests
  have been exercised. This v9 Job Manager path remains experimental.
- Mali-G710 MC7 in Google Tensor G2 (Pixel 7), with Debian 13 in DroidSpaces.
  `vulkaninfo`, `vkcube`, `vkmark`, X11 presentation, and Minecraft 26.2's
  native Vulkan backend at 1920x1080 have been tested.
- Mali-G925 (reported as G725 by the tested platform). `vulkaninfo` and
  `vkmark`, including X11 presentation, have been tested.

## Requirements

- An arm64 Linux userspace and Vulkan loader.
- A supported proprietary Mali kbase CSF or Job Manager kernel driver and
  access to `/dev/mali0`.
- Access to `/dev/dma_heap/system` for the fast raw dma-buf X11 WSI path.
- An X server supporting the Termux:X11/Winlator private DRI3 protocol when
  using raw dma-buf presentation.
- Device-node permissions that allow the container user to open the Mali and
  dma-heap devices.

PRoot cannot bypass Android file permissions or SELinux. Check the host before
building or installing anything:

```sh
uname -m
ls -lZ /dev/mali0 /dev/dma_heap/system 2>/dev/null
test -r /dev/mali0 && test -w /dev/mali0 && echo "Mali node visible"
test -r /dev/dma_heap/system && echo "DMA heap visible"
```

`aarch64` and a visible `/dev/mali0` are mandatory. The dma-heap is optional
for Vulkan rendering, but without it X11 presentation uses the slower
shared-memory fallback. Passing these shell checks does not guarantee that
Android will permit the required ioctls; `vulkaninfo` is the decisive test.

## Build from source on Ubuntu 24.04 arm64

This is the smallest native build: the PanVK Vulkan ICD only, without Zink,
OpenGL, video codecs, tests, or the optional Rust/KRAID compiler. Building in a
PRoot works but needs several GiB of free space and is much slower than an
arm64 CI runner or native Linux machine.

```sh
apt update
apt install -y --no-install-recommends \
  bison build-essential ca-certificates cmake flex git glslang-tools \
  libclang-cpp-dev libclang-dev libclc-18-dev libdrm-dev libelf-dev \
  libexpat1-dev libllvmspirvlib-18-dev libx11-dev libx11-xcb-dev \
  libxrandr-dev libxcb-dri3-dev libxcb-present-dev libxcb-randr0-dev \
  libxcb-shm0-dev libxcb-sync-dev libxcb-xfixes0-dev libxshmfence-dev \
  libxxf86vm-dev llvm-dev ninja-build pkgconf python3 python3-mako \
  python3-packaging python3-pip python3-setuptools python3-yaml \
  spirv-tools zlib1g-dev
python3 -m pip install --break-system-packages 'meson>=1.4.0,<2'

git clone https://github.com/SaicharanKandukuri/tensor-g1-proot-gpu.git
cd tensor-g1-proot-gpu
git switch research/pixel6a-panvk-v9-jm
git rev-parse HEAD

meson setup build-proot \
  --buildtype=release \
  --prefix=/opt/panvk-kbase \
  --libdir=lib \
  -Db_ndebug=true \
  -Dbuild-tests=false \
  -Ddisplay-info=disabled \
  -Degl=disabled \
  -Denable-glcpp-tests=false \
  -Dgallium-drivers= \
  -Dgbm=disabled \
  -Dgles1=disabled \
  -Dgles2=disabled \
  -Dglx=disabled \
  -Dinstall-mesa-clc=false \
  -Dinstall-precomp-compiler=false \
  -Dlibunwind=disabled \
  -Dlmsensors=disabled \
  -Dllvm=enabled \
  -Dmesa-clc=enabled \
  -Dopengl=false \
  -Dpanfrost-kmds=kbase \
  -Dpanfrost-rust=false \
  -Dplatforms=x11 \
  -Dprecomp-compiler=enabled \
  -Dshared-glapi=disabled \
  -Dtools= \
  -Dvalgrind=disabled \
  -Dvideo-codecs= \
  -Dvulkan-drivers=panfrost \
  -Dvulkan-layers= \
  -Dxmlconfig=disabled \
  -Dzstd=disabled
meson compile -C build-proot -j "$(nproc)"
meson install -C build-proot
```

After pulling new source, update the existing build with:

```sh
meson setup --reconfigure build-proot
meson compile -C build-proot -j "$(nproc)"
meson install -C build-proot
```

The installed ICD manifest is
`/opt/panvk-kbase/share/vulkan/icd.d/panfrost_icd.aarch64.json`.
The package build uses the same Meson configuration but may rename the
manifest and library under `/opt/udroid/panvk`.

## Installation

Releases contain four package variants:

- Debian 13: `mesa-panvk-kbase-debian13_*_arm64.deb`
- Ubuntu 26.04: `mesa-panvk-kbase-ubuntu2604_*_arm64.deb`
- Arch Linux: `mesa-panvk-kbase-*-aarch64.pkg.tar.zst`
- Generic arm64: `mesa-panvk-kbase-*-aarch64.tar.gz`

Install the matching native package:

```sh
# Debian 13 or Ubuntu 26.04
sudo apt install ./mesa-panvk-kbase-*_arm64.deb

# Arch Linux
sudo pacman -U ./mesa-panvk-kbase-*-aarch64.pkg.tar.zst
```

The generic archive is relocatable. Extract it and source its environment file
from Bash:

```sh
tar -xzf mesa-panvk-kbase-*-aarch64.tar.gz
cd mesa-panvk-kbase-*-aarch64
. ./env.sh
```

## Entering a Termux PRoot

Current `proot-distro` binds `/dev`, `/proc`, and `/sys` by default. Its
`--shared-x11` option binds the Termux X11 socket into the guest:

```sh
termux-x11 :0 &
proot-distro login ubuntu --shared-x11 --env DISPLAY=unix/:0
```

For an older `proot-distro`, bind the socket explicitly:

```sh
proot-distro login ubuntu \
  --bind "$PREFIX/tmp/.X11-unix:/tmp/.X11-unix" \
  --env DISPLAY=unix/:0
```

Custom PRoot launchers must expose `/dev`, `/proc`, and `/sys`, plus the X11
socket. Android system paths such as `/system`, `/apex`, and
`/linkerconfig/ld.config.txt` are needed only when the guest also loads Android
libraries; PanVK itself does not require them. Use `proot-distro login ubuntu
--get-proot-cmd` to inspect the actual bind set.

## Running Vulkan applications

Select the ICD manifest explicitly when another Vulkan driver is also
installed. This example uses the source-build prefix above:

```sh
export DISPLAY=unix/:0            # Change this for the X server in use.
export VK_DRIVER_FILES=/opt/panvk-kbase/share/vulkan/icd.d/panfrost_icd.aarch64.json
export PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1
export WSI_X11_TERMUX=1

vulkaninfo --summary
vkcube
vkmark --winsys xcb --present-mode immediate
```

Release packages may install a differently named manifest. Find it rather
than guessing:

```sh
find /usr/share/vulkan/icd.d /opt -name '*panfrost*icd*.json' 2>/dev/null
```

Use only one ICD selector. `VK_DRIVER_FILES` is current;
`VK_ICD_FILENAMES` is its deprecated loader-compatible predecessor.
`PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` is mandatory because this experimental
driver is not Vulkan-conformant.

`WSI_X11_TERMUX=1` selects PanVK's raw-FD Termux:X11 path when no explicit
PanVK override is present.  `PANVK_KBASE_DRI3=raw` remains available, while
`PANVK_KBASE_DRI3=0` explicitly selects the SHM fallback.

Minecraft 26.2 can use the same environment with its native Vulkan backend.
In Prism Launcher select **Prefer Vulkan (Experimental)**, or launch an
existing instance directly:

```sh
prismlauncher -l 26.2
```

OpenGL applications can run through Zink when a compatible Zink build is
installed:

```sh
export VK_DRIVER_FILES=/opt/panvk-kbase/share/vulkan/icd.d/panfrost_icd.aarch64.json
export PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export GALLIUM_DRIVER=zink
export LIBGL_KOPPER_DRI2=1

glxinfo -B
glmark2
```

Check that `glxinfo -B` names Zink and PanVK rather than `llvmpipe`. On the
current Tensor G1 branch, Zink exposes OpenGL 2.1; OpenGL 3.x and modern
applications such as Blender remain blocked by missing PanVK features. Vulkan
applications are the more complete path today.

Some Termux:X11 installations use a filesystem Unix socket and require a
display value such as `DISPLAY=unix/:0`. Use the value appropriate for the X
server session.

## Useful switches

| Variable | Use |
|---|---|
| `VK_DRIVER_FILES=/path/to/panfrost_icd.aarch64.json` | Select only this Vulkan ICD. Prefer it over deprecated `VK_ICD_FILENAMES`. |
| `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` | Required opt-in for experimental, non-conformant PanVK hardware. |
| `WSI_X11_TERMUX=1` | Select the compatible Termux:X11 raw dma-buf path when available. |
| `PANVK_KBASE_DRI3=raw` | Explicitly select raw dma-buf X11 presentation. `termux` is an alias. |
| `PANVK_KBASE_DRI3=0` | Force the broadly compatible but slower MIT-SHM presentation path. |
| `PANVK_KBASE_DMA_HEAP=/dev/dma_heap/system` | Override the dma-heap used for exportable X11 images. |
| `MESA_LOADER_DRIVER_OVERRIDE=zink` | Select Zink for OpenGL-over-Vulkan. |
| `GALLIUM_DRIVER=zink` | Reinforce Zink selection for Gallium applications. |
| `LIBGL_KOPPER_DRI2=1` | Use Kopper's X11 compatibility path; needed by the tested PRoot Zink setup. |
| `PAN_USE_KRAID=all` | Use KRAID for all supported shader stages when the build includes Rust/KRAID. |
| `PANVK_DEBUG=kbase_diag` | Enable expensive queue-hang breadcrumbs and command-stream dumps. |
| `PANVK_KBASE_USER_CACHE_SYNC=0` | Compatibility fallback to kernel cache-sync ioctls. Leave unset normally. |
| `PANVK_KBASE_DVFS=auto|max|default|FREQUENCY` | Request a global GPU DVFS mode; normally requires writable Mali sysfs controls. |

Do not copy internal `PANVK_KBASE_*` fault-injection, synchronous-submit, or
fence-control variables into launch scripts. They are developer probes that
can deliberately break synchronization or destroy performance.

See [PanVK kbase DVFS control](kbase-dvfs.md) before changing DVFS. The setting
is global to the device and is not restored automatically when an application
exits.

## X11 WSI implementation

Normal Linux DRI3 presentation passes a DRM GEM dma-buf file descriptor to the
X server. The proprietary kbase interface has no DRM device and cannot export a
DRM GEM buffer that way. The fast kbase WSI path instead:

1. allocates exportable swapchain memory from an Android dma-heap;
2. imports that memory into kbase for GPU rendering; and
3. sends the same raw, mmap-able dma-buf to a compatible X server through its
   private DRI3 `PixmapFromBuffers` path.

Set `WSI_X11_TERMUX=1` or `PANVK_KBASE_DRI3=raw` (the alias `termux` is also
accepted) to enable this path. The private raw-buffer modifier value is `1274`.
Termux:X11 and Winlator builds that implement this protocol can present the
buffer without a copy.

The private raw protocol does not provide the normal DRI3 explicit-sync
`FenceFromFD` behavior. PanVK therefore waits for GPU completion before
Present, and Present `IdleNotify` events control when an image may be reused.

Without `PANVK_KBASE_DRI3=raw`, presentation falls back to MIT-SHM. This is
more compatible but can be dramatically slower because rendered images must be
copied through host-visible shared memory. In one Pixel 7 test session, raw
presentation reached 889 FPS at 800x600 and 570 FPS at 1920x1080 in `vkmark`;
the 1920x1080 MIT-SHM path reached 36 FPS. These are device- and session-specific
measurements, not performance guarantees.

`bionic-vulkan-wrapper` uses a different private WSI transport. It sends an
Android `AHardwareBuffer` handle over a Unix socket and identifies that path
with modifier value `1255`. This PanVK implementation does not require the
bionic wrapper: its `1274` path sends a raw dma-buf that the Linux process can
allocate and mmap directly.

The dma-heap path can be overridden if a device exposes a different suitable
heap:

```sh
export PANVK_KBASE_DMA_HEAP=/dev/dma_heap/system
```

## Kernel panic warning

> **Warning:** Running the proprietary kbase driver from chroot, proot,
> DroidSpaces, or another container-like environment can expose kernel-driver
> bugs and may cause a kernel panic or device reboot. Some devices require a
> kernel-side fix. Kernel patches are device- and kernel-specific: review and
> port them carefully, keep backups, and do not flash an unverified kernel.

Reference fixes:

- GKI 2.0 devices: [mali_fxxker](https://github.com/funnymdzz/mali_fxxker)
- Non-GKI example: [Samsung A25 kernel commit
  87003e5](https://github.com/yoshi3jp/android_kernel_samsung_a25ex_mt6835/commit/87003e599eaf2b67bb93523021ca70e7c51f7dca)

These references demonstrate possible fixes; neither patch should be assumed to
apply unchanged to every device.

## Troubleshooting

- For a queue-hang investigation, set `PANVK_DEBUG=kbase_diag` to add GPU
  progress breadcrumbs and command-stream dumps. This instrumentation is off
  by default because its extra memory stores and scoreboard waits reduce
  draw/dispatch throughput.
- If no GPU is found, check access permissions for `/dev/mali0` inside the
  container.
- If raw WSI cannot allocate an image, check `/dev/dma_heap/system` access or
  set `PANVK_KBASE_DMA_HEAP` to the correct heap.
- If the window is blank or the raw path is rejected, confirm that the X server
  supports the Termux:X11/Winlator private DRI3 raw-buffer protocol and that
  `PANVK_KBASE_DRI3=raw` is set.
- If rendering works but presentation is slow, confirm that the process did not
  fall back to MIT-SHM.
