#!/system/bin/sh
set -eu

package=${UDROID_PACKAGE:-org.randomcoder.udroid.dev}
data=/data/user/0/$package
files=$data/files
archive=${1:-$files/tmp/udroid-wsi-smoke.tar}
proot=$files/runtime/proot-5.1.107.86-1-arm64-v8a/proot
tar=$files/runtime/proot-5.1.107.86-1-arm64-v8a/tar-1.35-1
rootfs=${UDROID_ROOTFS:-$files/rootfs/noble-raw}
x11_runtime=$files/runtime/x11

[ -d "$rootfs" ] || { echo "FAIL rootfs is unavailable: $rootfs"; exit 2; }
[ -S "$x11_runtime/.X11-unix/X0" ] || { echo 'FAIL display :0 is not running'; exit 2; }
[ -r "$archive" ] || { echo "FAIL archive is not readable: $archive"; exit 2; }

work=$files/tmp/udroid-wsi-smoke.$$
mkdir -p "$work"
trap 'rm -rf -- "$work"' EXIT
/system/bin/linker64 "$tar" -xf "$archive" -C "$work"
runtime=$work/opt/udroid/graphics
hal=${UDROID_VULKAN_HAL:-/vendor/lib64/hw/vulkan.mali.so}
export PROOT_TMP_DIR=$work/proot-tmp
mkdir -p "$PROOT_TMP_DIR"

case ${UDROID_WSI_STAGE:-present} in
    preload) guest_command='/usr/bin/true' ;;
    core) guest_command='/opt/udroid/graphics/libexec/vulkan-thread-lifecycle 1 1' ;;
    present)
        guest_command="/opt/udroid/graphics/libexec/vulkan-xcb-present --frames ${UDROID_WSI_FRAMES:-30} --hold-ms ${UDROID_WSI_HOLD_MS:-0}"
        ;;
    *) echo "FAIL unknown WSI smoke stage: $UDROID_WSI_STAGE"; exit 2 ;;
esac
echo "stage=${UDROID_WSI_STAGE:-present}"

/system/bin/linker64 "$proot" \
    --link2symlink \
    --kill-on-exit \
    --root-id \
    "--rootfs=$rootfs" \
    -b /system \
    -b /system_ext \
    -b /product \
    -b /vendor \
    -b /odm \
    -b /apex \
    -b /dev \
    -b /proc \
    -b /sys \
    -b /linkerconfig/ld.config.txt \
    -b "$x11_runtime/.X11-unix:/tmp/.X11-unix" \
    -b "$x11_runtime/.X0-lock:/tmp/.X0-lock" \
    -b "$runtime:/opt/udroid/graphics" \
    --cwd=/root \
    /usr/bin/env -i \
    HOME=/root \
    USER=root \
    LOGNAME=root \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    LANG=C.UTF-8 \
    DISPLAY=:0 \
    LD_PRELOAD=/opt/udroid/graphics/lib/bridge/libhybris-common.so.1:/opt/udroid/graphics/lib/bridge/libahb-wrapper.so \
    LD_LIBRARY_PATH=/opt/udroid/graphics/lib/bridge:/opt/udroid/graphics/share/vulkan/explicit_layer.d \
    HYBRIS_TLS_PRELOAD="$hal" \
    UDROID_VULKAN_HAL="$hal" \
    HYBRIS_FORCE_LEGACY_PROPERTY_HOOKS=1 \
    HYBRIS_SKIP_BIONIC_PROPERTY_INIT=1 \
    HYBRIS_LINKER_DIR=/opt/udroid/graphics/lib/bridge/libhybris/linker \
    HYBRIS_PTHREAD_RAW_EXIT=1 \
    HYBRIS_LD_LIBRARY_PATH=/opt/udroid/graphics/lib/bridge/bionic:/system/lib64:/system_ext/lib64:/product/lib64:/vendor/lib64:/vendor/lib64/egl:/vendor/lib64/hw:/odm/lib64:/apex/com.android.runtime/lib64/bionic \
    ANDROID_ROOT=/system \
    ANDROID_DATA=/data \
    ANDROID_RUNTIME_ROOT= \
    ANDROID_ART_ROOT=/apex/com.android.art \
    ANDROID_I18N_ROOT=/apex/com.android.i18n \
    ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \
    VK_DRIVER_FILES=/opt/udroid/graphics/share/vulkan/icd.d/sysvk.json \
    VK_ICD_FILENAMES=/opt/udroid/graphics/share/vulkan/icd.d/sysvk.json \
    VK_LAYER_PATH=/opt/udroid/graphics/share/vulkan/explicit_layer.d \
    VK_INSTANCE_LAYERS=VK_LAYER_window_system_integration \
    VK_LOADER_DEBUG=warn,error \
    WSI_X11_AHB=1 \
    WSI_X11_PRIVATE_CONNECTION=1 \
    UDROID_AHB_TRACE=${UDROID_AHB_TRACE:-} \
    VULKAN_WSI_DEBUG_LEVEL=${VULKAN_WSI_DEBUG_LEVEL:-1} \
    NODEVICE_SELECT=1 \
    /bin/sh -lc "$guest_command"
