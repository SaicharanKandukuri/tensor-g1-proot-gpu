/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_PHYSICAL_DEVICE_H
#define PANVK_PHYSICAL_DEVICE_H

#include "util/u_debug.h"
#include <stdint.h>
#include <sys/types.h>

#include "panvk_instance.h"
#include "panvk_macros.h"

#include "vk_physical_device.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include "vk_util.h"
#include "wsi_common.h"

#include "lib/kmod/pan_kmod.h"

struct pan_model;
struct pan_blendable_format;
struct pan_format;
struct panvk_instance;

struct panvk_physical_device {
   struct vk_physical_device vk;

   struct {
      struct pan_kmod_dev *dev;
   } kmod;

   const struct pan_model *model;

   union {
      struct {
         struct {
            uint32_t chunk_size;
            uint32_t initial_chunks;
            uint32_t max_chunks;
         } tiler;
      } csf;
   };

   struct {
      dev_t primary_rdev;
      dev_t render_rdev;
   } drm;

   /* Device node path for kbase (non-DRM) devices, e.g. "/dev/mali0";
    * empty for DRM devices.  Logical devices must open() a fresh fd from
    * this path: dup()ing the physical device fd would share the kbase
    * context, whose version handshake can only be performed once. */
   char kbase_node_path[32];

   struct {
      const struct pan_blendable_format *blendable;
      const struct pan_format *all;
   } formats;

   char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
   uint8_t cache_uuid[VK_UUID_SIZE];

   struct {
      VkMemoryHeap heaps[1];
      uint32_t heap_count;

      VkMemoryType types[4];
      uint32_t type_count;

      uint64_t max_supported_va;
      alignas(8) uint64_t heap_used;
   } memory;

   struct vk_sync_type drm_syncobj_type;
   struct vk_sync_timeline_type sync_timeline_type;
   const struct vk_sync_type *sync_types[3];

   struct wsi_device wsi_device;

   uint64_t compute_core_mask;
   uint64_t fragment_core_mask;
};

VK_DEFINE_HANDLE_CASTS(panvk_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

static inline struct panvk_physical_device *
to_panvk_physical_device(struct vk_physical_device *phys_dev)
{
   return container_of(phys_dev, struct panvk_physical_device, vk);
}

float panvk_get_gpu_system_timestamp_period(
   const struct panvk_physical_device *device);

VkResult panvk_physical_device_init(struct panvk_physical_device *device,
                                    struct panvk_instance *instance,
                                    drmDevicePtr drm_device);

#if defined(HAVE_PAN_KMOD_KBASE)
VkResult panvk_physical_device_init_kbase(struct panvk_physical_device *device,
                                          struct panvk_instance *instance,
                                          const char *path);

#define PANVK_KBASE_SYNC_TARGET_COUNT 3

typedef VkResult (*panvk_kbase_sync_wait_func)(
   void *data,
   const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT],
   uint64_t abs_timeout_ns);

/* Turn a pending payload into a sync_file: *sync_file is a new fd, or -1 if
 * the work has already completed. */
typedef VkResult (*panvk_kbase_sync_export_func)(
   void *data,
   const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT],
   int *sync_file);

void panvk_kbase_sync_set_pending(
   struct vk_sync *sync, void *data, panvk_kbase_sync_wait_func wait,
   panvk_kbase_sync_export_func export_fn,
   const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT]);

/* What a submit has to wait for before it may run. */
enum panvk_kbase_sync_payload {
   PANVK_KBASE_SYNC_PAYLOAD_NONE,    /* signalled: nothing to wait for */
   PANVK_KBASE_SYNC_PAYLOAD_GPU,     /* our submitted work: *data, targets */
   PANVK_KBASE_SYNC_PAYLOAD_FILE,    /* an imported sync_file: *sync_file = dup */
   PANVK_KBASE_SYNC_PAYLOAD_OTHER,   /* anything else: wait on the CPU */
};

enum panvk_kbase_sync_payload panvk_kbase_sync_get_payload(
   struct vk_sync *sync, void **data,
   uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT], int *sync_file);
#endif

void panvk_physical_device_finish(struct panvk_physical_device *device);


VkSampleCountFlags panvk_get_sample_counts(unsigned arch,
                                           unsigned max_tib_size,
                                           unsigned max_cbuf_atts,
                                           unsigned format_size);

VkDeviceSize
panvk_get_max_resource_size(const struct panvk_physical_device *device);

VkDeviceSize
panvk_get_max_buffer_size(const struct panvk_physical_device *device);

#ifdef PAN_ARCH
void panvk_per_arch(get_physical_device_extensions)(
   const struct panvk_physical_device *device,
   const struct panvk_instance *instance,
   struct vk_device_extension_table *ext);

void panvk_per_arch(get_physical_device_features)(
   const struct panvk_instance *instance,
   const struct panvk_physical_device *device, struct vk_features *features);

void panvk_per_arch(get_physical_device_properties)(
   const struct panvk_instance *instance,
   const struct panvk_physical_device *device,
   struct vk_properties *properties);
#endif

/* EXPERIMENTAL v9: sampler min/max reduction (samplerFilterMinmax) and
 * cubic filtering (VK_EXT_filter_cubic) are emulated in the shader
 * (panvk_v9_nir_lower_minmax.h). Off by default: once active, every float
 * texture sample of every shader carries the emulation code (a one-sample
 * fragment shader grew from 256 to 7552 bytes). PANVK_V9_EMULATE_SAMPLER=1
 * exposes the features; the pass then only runs on devices that enable
 * them. */
static inline bool
panvk_v9_sampler_emulation_exposed(unsigned arch)
{
   return arch == 9 && debug_get_bool_option("PANVK_V9_EMULATE_SAMPLER", false);
}

#endif
