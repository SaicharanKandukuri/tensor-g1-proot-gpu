# Pixel 6a Android 17 PRoot libhybris checkpoint

## Scope

This checkpoint uses uDroid's app-private Ubuntu Noble rootfs, bundled PRoot,
embedded X11 server on display `:0`, the explicit Mali Vulkan HAL at
`/vendor/lib64/hw/vulkan.mali.so`, sysvk, and the patched xMeM AHardwareBuffer
X11 WSI. It does not use the native Termux process path.

## Failure and fix

The initial run reached `vkGetPhysicalDeviceSurfaceFormatsKHR` with a populated
format array, then crashed in Android 17 Bionic's `__set_errno_internal` while
writing `pthread_internal_t::errno_value` at offset 776. Inline
`MRS TPIDR_EL0` code was reading glibc's TCB instead of Bionic state.

The bridge now redirects raw AArch64 thread-pointer reads to uDroid's isolated
Bionic TCB and gives `TLS_SLOT_THREAD_ID` a per-thread shadow. The instruction
patcher is adapted from `wmww/libhybris` commits `77bda7a`, `56954bf`, and
`a7674b1`; the complete references are recorded in libhybris patch `0004`.

## Results

| Gate | Result |
| --- | --- |
| PRoot bridge preload and explicit HAL | Pass |
| Vulkan lifecycle, 2 threads x 4 cycles | Pass; host TLS canary intact |
| Surface capabilities and populated formats | Pass |
| AHardwareBuffer export and four-image swapchain | Pass |
| 30-frame Present and teardown | Pass; 69.69 producer FPS |
| 60-frame visible hold and teardown | Pass; 63.70 producer FPS |

The blue clear window was visible in uDroid's display surface. Reported FPS is
producer throughput, not proof of physical scanout cadence. Full desktop,
compositor, resize, and long-duration qualification remain separate gates.
