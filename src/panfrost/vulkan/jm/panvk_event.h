/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_EVENT_H
#define PANVK_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#include "vk_object.h"

struct panvk_event {
   struct vk_object_base base;
   uint32_t syncobj;
   /* kbase has no DRM syncobjs. Every kbase submission completes before
    * vkQueueSubmit returns, so the event state is a plain flag that the
    * host and the queue (after a batch finished) update in order. */
   bool kbase;
   uint32_t kbase_set;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_event, base, VkEvent, VK_OBJECT_TYPE_EVENT)

#endif
