/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_SAMPLER_H
#define PANVK_SAMPLER_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include <stdint.h>

#include "vk_sampler.h"

/* We use 2 sampler planes for YCbCr conversion with different filters for
 * the Y and CbCr components.
 */
#define PANVK_MAX_DESCS_PER_SAMPLER 2

struct panvk_sampler {
   struct vk_sampler vk;
   struct mali_sampler_packed descs[PANVK_MAX_DESCS_PER_SAMPLER];
   uint8_t desc_count;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_sampler, vk.base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)


/* EXPERIMENTAL: v9 sampler word 3 layout used by the min/max reduction and
 * cubic filter emulation (panvk_vX_sampler.c, panvk_v9_nir_lower_minmax.h).
 * Neither MIN nor MAX set means WEIGHTED_AVERAGE. */
#define PANVK_V9_MINMAX_MIN        (1u << 0)
#define PANVK_V9_MINMAX_MAX        (1u << 1)
#define PANVK_V9_MINMAX_MAG_LINEAR (1u << 4)
#define PANVK_V9_MINMAX_MIN_LINEAR (1u << 5)
#define PANVK_V9_MINMAX_MIP_LINEAR (1u << 6)
#define PANVK_V9_MINMAX_MAG_CUBIC  (1u << 7)
#define PANVK_V9_MINMAX_MIN_CUBIC  (1u << 8)
#define PANVK_V9_MINMAX_TAG        (1u << 31)

#endif
