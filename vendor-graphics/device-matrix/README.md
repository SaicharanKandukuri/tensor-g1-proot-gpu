# Device matrix

Generated baseline JSON files live here. A baseline describes discovery and
permissions only; Vulkan, AHardwareBuffer, WSI, synchronization, Zink, and
performance results will be added as separate probe records.

Serial numbers are omitted from committed reports. The `deviceId` is derived
from stable public build properties so reports can be compared without
publishing an ADB identifier.

Every generated baseline must validate against `schema-v1.json`. A baseline is
immutable evidence for one Android build fingerprint; re-running after an OS
update produces a new `deviceId` and a separate qualification run.

Current discovery checkpoints include:

- `exynos-9611-sm-m315f-android13.json`: first full AHardwareBuffer WSI proof;
- `tensor-g1-bluejay-android17.json`: Tensor baseline where a global DRM render
  node exists but is inaccessible to the app domain, so the derived profile
  correctly selects `vendor-vulkan-ahb`;
- `tensor-g1-bluejay-android17-native-termux-2026-08-18.md`: native Bionic
  Zink/application evidence, explicitly separate from glibc bridge approval.
- `tensor-g1-bluejay-android17-proot-hybris-2026-10-19.md`: app-private PRoot
  libhybris, sysvk, AHardwareBuffer X11 WSI, and visible Present checkpoint.

WSI suite reports using the original lifecycle-only contract validate against
`wsi-qualification-schema-v1.json`. New reports validate against
`wsi-qualification-schema-v2.json`, which additionally requires observable
GPU Present-offload deltas. Reports produced by the TLS-isolated runtime also
include `results.vulkanThreadLifecycle`; the field is optional in schema v2 so
the earlier immutable v2 evidence remains valid. Keep a report separate from its discovery
baseline: the same Android build can be tested against multiple runtime
revisions without rewriting the original device evidence.
