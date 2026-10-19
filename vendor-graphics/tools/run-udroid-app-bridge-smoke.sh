#!/system/bin/sh
set -eu

package=${UDROID_PACKAGE:-org.randomcoder.udroid.dev}
data=/data/user/0/$package
files=$data/files
archive=${1:-$files/tmp/udroid-vendor-bridge-smoke.tar}
proot=$files/runtime/proot-5.1.107.86-1-arm64-v8a/proot
tar=$files/runtime/proot-5.1.107.86-1-arm64-v8a/tar-1.35-1

rootfs=
for candidate in noble-raw jammy-kdeRaw udroid-focal-xfce4 proot-debian-trixie; do
    if [ -d "$files/rootfs/$candidate" ]; then
        rootfs=$files/rootfs/$candidate
        break
    fi
done
[ -n "$rootfs" ] || { echo 'FAIL no supported uDroid rootfs'; exit 2; }
[ -r "$archive" ] || { echo "FAIL archive is not readable: $archive"; exit 2; }
[ -x "$proot" ] || { echo "FAIL PRoot is missing: $proot"; exit 2; }

work=$files/tmp/udroid-bridge-smoke.$$
mkdir -p "$work"
trap 'rm -rf -- "$work"' EXIT
/system/bin/linker64 "$tar" -xf "$archive" -C "$work"
runtime=$work/opt/udroid/graphics
hal=${UDROID_VULKAN_HAL:-/vendor/lib64/hw/vulkan.mali.so}
export PROOT_TMP_DIR=$work/proot-tmp
mkdir -p "$PROOT_TMP_DIR"
cd "$files"

echo "rootfs=$rootfs"
echo "hal=$hal"
echo "runtime=$runtime"

run_guest()
{
    stage=$1
    tls_preload=$2
    shift 2
    echo "stage=$stage"
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
        -b "$runtime:/opt/udroid/graphics" \
        --cwd=/root \
        /usr/bin/env -i \
        HOME=/root \
        USER=root \
        LOGNAME=root \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        LANG=C.UTF-8 \
        LD_PRELOAD=/opt/udroid/graphics/lib/bridge/libhybris-common.so.1 \
        LD_LIBRARY_PATH=/opt/udroid/graphics/lib/bridge \
        HYBRIS_TLS_PRELOAD="$tls_preload" \
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
        VK_LOADER_DEBUG=warn,error \
        NODEVICE_SELECT=1 \
        "$@"
    echo "PASS stage=$stage"
}

run_guest libhybris-preload '' /usr/bin/true
run_guest libhardware-load '' /usr/bin/env \
    LD_PRELOAD=/opt/udroid/graphics/lib/bridge/libhardware.so.2 /usr/bin/true
run_guest sysvk-load '' /usr/bin/env \
    LD_PRELOAD=/opt/udroid/graphics/lib/bridge/libsysvk.so /usr/bin/true
run_guest mali-hal-preload "$hal" /usr/bin/true
run_guest vulkan-lifecycle "$hal" \
    /opt/udroid/graphics/libexec/vulkan-thread-lifecycle 2 4
