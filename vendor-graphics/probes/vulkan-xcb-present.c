#define _POSIX_C_SOURCE 200809L
#define VK_USE_PLATFORM_XCB_KHR
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#define CHECK_VK(call, stage) do { \
    VkResult check_result = (call); \
    if (check_result != VK_SUCCESS) { \
        fprintf(stderr, "FAIL stage=%s result=%d\n", stage, check_result); \
        return 1; \
    } \
} while (0)

static xcb_screen_t *get_screen(xcb_connection_t *connection, int number)
{
    xcb_screen_iterator_t iterator =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (iterator.rem && number-- > 0)
        xcb_screen_next(&iterator);
    return iterator.data;
}

static uint64_t parse_u64(const char *value, const char *option)
{
    char *end = NULL;
    if (value[0] == '\0')
        goto invalid;
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor)
        if (*cursor < '0' || *cursor > '9')
            goto invalid;

    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == NULL || *end != '\0' || errno == ERANGE)
        goto invalid;
    return (uint64_t)parsed;

invalid:
    fprintf(stderr, "FAIL stage=arguments option=%s value=%s\n", option, value);
    exit(2);
}

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    uint64_t hold_ms = 6000;
    uint64_t frame_count = 1;
    uint64_t create_destroy_cycles = 0;
    uint64_t resize_after_frames = 0;
    uint64_t destroy_window_after_frames = 0;
    uint64_t disconnect_x_after_frames = 0;
    int destroy_window_before_capabilities = 0;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--hold-ms") == 0 && index + 1 < argc) {
            hold_ms = parse_u64(argv[++index], "--hold-ms");
        } else if (strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
            frame_count = parse_u64(argv[++index], "--frames");
            if (frame_count == 0 || frame_count > 100000) {
                fprintf(stderr, "FAIL stage=arguments option=--frames value=%llu\n",
                        (unsigned long long)frame_count);
                return 2;
            }
        } else if (strcmp(argv[index], "--create-destroy-cycles") == 0 &&
                   index + 1 < argc) {
            create_destroy_cycles =
                parse_u64(argv[++index], "--create-destroy-cycles");
            if (create_destroy_cycles == 0 || create_destroy_cycles > 100000) {
                fprintf(stderr,
                        "FAIL stage=arguments option=--create-destroy-cycles value=%llu\n",
                        (unsigned long long)create_destroy_cycles);
                return 2;
            }
        } else if (strcmp(argv[index], "--resize-after-frames") == 0 &&
                   index + 1 < argc) {
            resize_after_frames =
                parse_u64(argv[++index], "--resize-after-frames");
            if (resize_after_frames == 0 || resize_after_frames > 100000) {
                fprintf(stderr,
                        "FAIL stage=arguments option=--resize-after-frames value=%llu\n",
                        (unsigned long long)resize_after_frames);
                return 2;
            }
        } else if (strcmp(argv[index],
                          "--destroy-window-before-capabilities") == 0) {
            destroy_window_before_capabilities = 1;
        } else if (strcmp(argv[index], "--destroy-window-after-frames") == 0 &&
                   index + 1 < argc) {
            destroy_window_after_frames =
                parse_u64(argv[++index], "--destroy-window-after-frames");
            if (destroy_window_after_frames == 0 ||
                destroy_window_after_frames > 100000) {
                fprintf(stderr,
                        "FAIL stage=arguments option=--destroy-window-after-frames value=%llu\n",
                        (unsigned long long)destroy_window_after_frames);
                return 2;
            }
        } else if (strcmp(argv[index], "--disconnect-x-after-frames") == 0 &&
                   index + 1 < argc) {
            disconnect_x_after_frames =
                parse_u64(argv[++index], "--disconnect-x-after-frames");
            if (disconnect_x_after_frames == 0 ||
                disconnect_x_after_frames > 100000) {
                fprintf(stderr,
                        "FAIL stage=arguments option=--disconnect-x-after-frames value=%llu\n",
                        (unsigned long long)disconnect_x_after_frames);
                return 2;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--hold-ms MILLISECONDS] [--frames COUNT] "
                    "[--create-destroy-cycles COUNT] "
                    "[--resize-after-frames COUNT] "
                    "[--destroy-window-before-capabilities] "
                    "[--destroy-window-after-frames COUNT] "
                    "[--disconnect-x-after-frames COUNT]\n",
                    argv[0]);
            return 2;
        }
    }
    if (resize_after_frames >= frame_count) {
        fprintf(stderr,
                "FAIL stage=arguments option=--resize-after-frames value=%llu frames=%llu\n",
                (unsigned long long)resize_after_frames,
                (unsigned long long)frame_count);
        return 2;
    }
    if (destroy_window_after_frames > frame_count) {
        fprintf(stderr,
                "FAIL stage=arguments option=--destroy-window-after-frames value=%llu frames=%llu\n",
                (unsigned long long)destroy_window_after_frames,
                (unsigned long long)frame_count);
        return 2;
    }
    if (disconnect_x_after_frames > frame_count) {
        fprintf(stderr,
                "FAIL stage=arguments option=--disconnect-x-after-frames value=%llu frames=%llu\n",
                (unsigned long long)disconnect_x_after_frames,
                (unsigned long long)frame_count);
        return 2;
    }
    int special_mode_count = (create_destroy_cycles != 0) +
        (resize_after_frames != 0) + destroy_window_before_capabilities +
        (destroy_window_after_frames != 0) +
        (disconnect_x_after_frames != 0);
    if (special_mode_count > 1) {
        fprintf(stderr,
                "FAIL stage=arguments reason=multiple-special-modes\n");
        return 2;
    }

    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "FAIL stage=xcb-connect\n");
        return 1;
    }
    xcb_screen_t *screen = get_screen(connection, screen_number);
    if (!screen) {
        fprintf(stderr, "FAIL stage=xcb-screen\n");
        return 1;
    }

    xcb_window_t window = xcb_generate_id(connection);
    uint32_t values[] = { screen->black_pixel, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root,
                      80, 100, 480, 320, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const char title[] = "uDroid vendor Vulkan WSI probe";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        sizeof(title) - 1, title);
    xcb_map_window(connection, window);
    xcb_flush(connection);
    printf("PASS stage=xcb-window\n");

    const char *instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XCB_SURFACE_EXTENSION_NAME,
    };
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "udroid-vulkan-xcb-present-probe",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo instance_create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    CHECK_VK(vkCreateInstance(&instance_create, NULL, &instance), "create-instance");
    printf("PASS stage=vulkan-instance\n");

    uint32_t physical_count = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_count, NULL),
             "count-physical-devices");
    if (physical_count == 0) {
        fprintf(stderr, "FAIL stage=no-physical-device\n");
        return 1;
    }
    VkPhysicalDevice *physical_devices =
        calloc(physical_count, sizeof(*physical_devices));
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices),
             "enumerate-physical-devices");
    VkPhysicalDevice physical = physical_devices[0];
    free(physical_devices);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("PASS stage=physical-device device=%s api=%u.%u.%u\n",
           properties.deviceName,
           VK_VERSION_MAJOR(properties.apiVersion),
           VK_VERSION_MINOR(properties.apiVersion),
           VK_VERSION_PATCH(properties.apiVersion));

    VkXcbSurfaceCreateInfoKHR surface_create = {
        .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection = connection,
        .window = window,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    CHECK_VK(vkCreateXcbSurfaceKHR(instance, &surface_create, NULL, &surface),
             "create-xcb-surface");
    printf("PASS stage=xcb-surface\n");

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
    VkQueueFamilyProperties *queue_properties =
        calloc(queue_count, sizeof(*queue_properties));
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queue_properties);
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t index = 0; index < queue_count; ++index) {
        VkBool32 present_supported = VK_FALSE;
        CHECK_VK(vkGetPhysicalDeviceSurfaceSupportKHR(
                     physical, index, surface, &present_supported),
                 "query-surface-support");
        if ((queue_properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            present_supported) {
            queue_family = index;
            break;
        }
    }
    free(queue_properties);
    if (queue_family == UINT32_MAX) {
        fprintf(stderr, "FAIL stage=no-present-queue\n");
        return 1;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo device_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    CHECK_VK(vkCreateDevice(physical, &device_create, NULL, &device),
             "create-device");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    printf("PASS stage=vulkan-device queue_family=%u\n", queue_family);

    if (destroy_window_before_capabilities) {
        xcb_void_cookie_t destroy_cookie =
            xcb_destroy_window_checked(connection, window);
        xcb_generic_error_t *destroy_error =
            xcb_request_check(connection, destroy_cookie);
        if (destroy_error != NULL) {
            fprintf(stderr,
                    "FAIL stage=destroy-window xcb_error=%u\n",
                    destroy_error->error_code);
            free(destroy_error);
            return 1;
        }

        VkSurfaceCapabilitiesKHR lost_capabilities = {0};
        VkResult lost_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physical, surface, &lost_capabilities);
        if (lost_result != VK_ERROR_SURFACE_LOST_KHR) {
            fprintf(stderr,
                    "FAIL stage=lost-surface-capabilities expected=%d actual=%d extent=%ux%u\n",
                    VK_ERROR_SURFACE_LOST_KHR, lost_result,
                    lost_capabilities.currentExtent.width,
                    lost_capabilities.currentExtent.height);
            return 1;
        }

        vkDestroyDevice(device, NULL);
        vkDestroySurfaceKHR(instance, surface, NULL);
        vkDestroyInstance(instance, NULL);
        xcb_disconnect(connection);
        printf("PASS stage=lost-surface-capabilities result=%d\n",
               lost_result);
        printf("PASS stage=clean-exit\n");
        return 0;
    }

    VkSurfaceCapabilitiesKHR capabilities = {0};
    printf("BEGIN stage=surface-capabilities\n");
    CHECK_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                 physical, surface, &capabilities),
             "surface-capabilities");
    printf("PASS stage=surface-capabilities extent=%ux%u images=%u-%u\n",
           capabilities.currentExtent.width, capabilities.currentExtent.height,
           capabilities.minImageCount, capabilities.maxImageCount);
    uint32_t format_count = 0;
    printf("BEGIN stage=count-surface-formats\n");
    CHECK_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(
                 physical, surface, &format_count, NULL),
             "count-surface-formats");
    printf("PASS stage=count-surface-formats count=%u\n", format_count);
    VkSurfaceFormatKHR *formats = calloc(format_count, sizeof(*formats));
    printf("BEGIN stage=surface-formats\n");
    CHECK_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(
                 physical, surface, &format_count, formats),
             "surface-formats");
    if (format_count == 0) {
        fprintf(stderr, "FAIL stage=no-surface-format\n");
        return 1;
    }
    VkSurfaceFormatKHR format = formats[0];
    free(formats);
    VkExtent2D extent = capabilities.currentExtent;
    uint32_t image_count = capabilities.minImageCount;
    if (image_count < 2)
        image_count = 2;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & alpha))
        alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainCreateInfoKHR swapchain_create = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    CHECK_VK(vkCreateSwapchainKHR(device, &swapchain_create, NULL, &swapchain),
             "create-swapchain");
    printf("PASS stage=swapchain extent=%ux%u format=%u images=%u\n",
           extent.width, extent.height, format.format, image_count);

    if (create_destroy_cycles != 0) {
        vkDestroySwapchainKHR(device, swapchain, NULL);
        swapchain = VK_NULL_HANDLE;
        for (uint64_t cycle = 1; cycle < create_destroy_cycles; ++cycle) {
            CHECK_VK(vkCreateSwapchainKHR(device, &swapchain_create, NULL,
                                          &swapchain),
                     "create-destroy-swapchain");
            vkDestroySwapchainKHR(device, swapchain, NULL);
            swapchain = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device, NULL);
        vkDestroySurfaceKHR(instance, surface, NULL);
        vkDestroyInstance(instance, NULL);
        xcb_destroy_window(connection, window);
        xcb_disconnect(connection);
        printf("PASS stage=create-destroy cycles=%llu\n",
               (unsigned long long)create_destroy_cycles);
        printf("PASS stage=clean-exit\n");
        return 0;
    }

    uint32_t swapchain_image_count = 0;
    CHECK_VK(vkGetSwapchainImagesKHR(device, swapchain,
                                     &swapchain_image_count, NULL),
             "count-swapchain-images");
    VkImage *images = calloc(swapchain_image_count, sizeof(*images));
    CHECK_VK(vkGetSwapchainImagesKHR(device, swapchain,
                                     &swapchain_image_count, images),
                 "swapchain-images");

    VkImageViewCreateInfo view_create = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = images[0],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format.format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    VkImageView image_view = VK_NULL_HANDLE;
    CHECK_VK(vkCreateImageView(device, &view_create, NULL, &image_view),
             "create-swapchain-image-view");
    printf("PASS stage=image-view format=%u\n", format.format);

    VkCommandPoolCreateInfo pool_create = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK_VK(vkCreateCommandPool(device, &pool_create, NULL, &pool),
             "create-command-pool");
    VkCommandBufferAllocateInfo command_allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    CHECK_VK(vkAllocateCommandBuffers(device, &command_allocate, &command),
             "allocate-command-buffer");
    VkSemaphoreCreateInfo semaphore_create = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    CHECK_VK(vkCreateSemaphore(device, &semaphore_create, NULL, &acquired),
             "create-acquire-semaphore");
    CHECK_VK(vkCreateSemaphore(device, &semaphore_create, NULL, &rendered),
             "create-render-semaphore");

    uint32_t image_index = 0;
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    uint64_t present_start = monotonic_ns();
    for (uint64_t frame = 0; frame < frame_count; ++frame) {
        if (frame > 0)
            CHECK_VK(vkResetCommandBuffer(command, 0), "reset-command-buffer");
        CHECK_VK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                       acquired, VK_NULL_HANDLE, &image_index),
                 "acquire-image");
        CHECK_VK(vkBeginCommandBuffer(command, &begin), "begin-command-buffer");
        VkImageMemoryBarrier to_transfer = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[image_index],
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 0, NULL, 1, &to_transfer);
        VkClearColorValue color = frame & 1
            ? (VkClearColorValue) { .float32 = { 0.04f, 0.12f, 0.32f, 1.0f } }
            : (VkClearColorValue) { .float32 = { 1.0f, 0.08f, 0.0f, 1.0f } };
        vkCmdClearColorImage(command, images[image_index],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1, &range);
        VkImageMemoryBarrier to_present = to_transfer;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, NULL, 0, NULL, 1, &to_present);
        CHECK_VK(vkEndCommandBuffer(command), "end-command-buffer");

        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &command,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &rendered,
        };
        CHECK_VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "queue-submit");
        VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &rendered,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &image_index,
        };
        CHECK_VK(vkQueuePresentKHR(queue, &present), "queue-present");
        CHECK_VK(vkQueueWaitIdle(queue), "queue-idle");

        if (frame + 1 == destroy_window_after_frames ||
            frame + 1 == disconnect_x_after_frames) {
            const char *loss_stage = "live-surface-loss";
            if (frame + 1 == destroy_window_after_frames) {
                xcb_void_cookie_t destroy_cookie =
                    xcb_destroy_window_checked(connection, window);
                xcb_generic_error_t *destroy_error =
                    xcb_request_check(connection, destroy_cookie);
                if (destroy_error != NULL) {
                    fprintf(stderr,
                            "FAIL stage=destroy-live-window xcb_error=%u\n",
                            destroy_error->error_code);
                    free(destroy_error);
                    return 1;
                }
            } else {
                loss_stage = "x-connection-loss";
                int xcb_fd = xcb_get_file_descriptor(connection);
                if (xcb_fd < 0 || shutdown(xcb_fd, SHUT_RDWR) != 0) {
                    fprintf(stderr,
                            "FAIL stage=disconnect-x-transport errno=%d\n",
                            errno);
                    return 1;
                }
            }

            struct timespec event_delivery = {
                .tv_sec = 0,
                .tv_nsec = 100000000,
            };
            nanosleep(&event_delivery, NULL);

            uint32_t lost_image_index = 0;
            VkResult lost_result = vkAcquireNextImageKHR(
                device, swapchain, 3000000000ULL, acquired, VK_NULL_HANDLE,
                &lost_image_index);
            if (lost_result != VK_ERROR_SURFACE_LOST_KHR) {
                fprintf(stderr,
                        "FAIL stage=%s expected=%d actual=%d\n",
                        loss_stage, VK_ERROR_SURFACE_LOST_KHR, lost_result);
                return 1;
            }

            vkDeviceWaitIdle(device);
            vkDestroySemaphore(device, rendered, NULL);
            vkDestroySemaphore(device, acquired, NULL);
            vkDestroyCommandPool(device, pool, NULL);
            vkDestroyImageView(device, image_view, NULL);
            free(images);
            vkDestroySwapchainKHR(device, swapchain, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroySurfaceKHR(instance, surface, NULL);
            vkDestroyInstance(instance, NULL);
            xcb_disconnect(connection);
            printf("PASS stage=%s frame=%llu result=%d\n", loss_stage,
                   (unsigned long long)(frame + 1), lost_result);
            printf("PASS stage=clean-exit\n");
            return 0;
        }

        if (frame + 1 == resize_after_frames) {
            const uint32_t resized_width = extent.width + 96;
            const uint32_t resized_height = extent.height + 64;
            const uint32_t resized_values[] = {
                resized_width,
                resized_height,
            };
            xcb_void_cookie_t resize_cookie = xcb_configure_window_checked(
                connection, window,
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                resized_values);
            xcb_generic_error_t *resize_error =
                xcb_request_check(connection, resize_cookie);
            if (resize_error != NULL) {
                fprintf(stderr,
                        "FAIL stage=resize-request xcb_error=%u\n",
                        resize_error->error_code);
                free(resize_error);
                return 1;
            }

            xcb_get_geometry_cookie_t geometry_cookie =
                xcb_get_geometry(connection, window);
            xcb_get_geometry_reply_t *geometry =
                xcb_get_geometry_reply(connection, geometry_cookie, NULL);
            if (geometry == NULL || geometry->width != resized_width ||
                geometry->height != resized_height) {
                fprintf(stderr,
                        "FAIL stage=resize-geometry expected=%ux%u actual=%ux%u\n",
                        resized_width, resized_height,
                        geometry != NULL ? geometry->width : 0,
                        geometry != NULL ? geometry->height : 0);
                free(geometry);
                return 1;
            }
            free(geometry);

            VkSurfaceCapabilitiesKHR resized_capabilities;
            CHECK_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                         physical, surface, &resized_capabilities),
                     "resize-surface-capabilities");
            if (resized_capabilities.currentExtent.width != resized_width ||
                resized_capabilities.currentExtent.height != resized_height) {
                fprintf(stderr,
                        "FAIL stage=resize-surface-extent expected=%ux%u actual=%ux%u\n",
                        resized_width, resized_height,
                        resized_capabilities.currentExtent.width,
                        resized_capabilities.currentExtent.height);
                return 1;
            }

            VkSwapchainKHR old_swapchain = swapchain;
            VkSwapchainKHR replacement_swapchain = VK_NULL_HANDLE;
            swapchain_create.imageExtent = resized_capabilities.currentExtent;
            swapchain_create.preTransform = resized_capabilities.currentTransform;
            swapchain_create.oldSwapchain = old_swapchain;
            CHECK_VK(vkCreateSwapchainKHR(device, &swapchain_create, NULL,
                                          &replacement_swapchain),
                     "resize-create-replacement-swapchain");

            uint32_t replacement_image_count = 0;
            CHECK_VK(vkGetSwapchainImagesKHR(device, replacement_swapchain,
                                             &replacement_image_count, NULL),
                     "resize-count-swapchain-images");
            VkImage *replacement_images =
                calloc(replacement_image_count, sizeof(*replacement_images));
            if (replacement_images == NULL) {
                fprintf(stderr, "FAIL stage=resize-allocate-image-list\n");
                return 1;
            }
            CHECK_VK(vkGetSwapchainImagesKHR(device, replacement_swapchain,
                                             &replacement_image_count,
                                             replacement_images),
                     "resize-swapchain-images");

            view_create.image = replacement_images[0];
            VkImageView replacement_image_view = VK_NULL_HANDLE;
            CHECK_VK(vkCreateImageView(device, &view_create, NULL,
                                       &replacement_image_view),
                     "resize-create-swapchain-image-view");

            vkDestroyImageView(device, image_view, NULL);
            free(images);
            vkDestroySwapchainKHR(device, old_swapchain, NULL);
            swapchain = replacement_swapchain;
            images = replacement_images;
            swapchain_image_count = replacement_image_count;
            image_view = replacement_image_view;
            extent = resized_capabilities.currentExtent;
            swapchain_create.oldSwapchain = VK_NULL_HANDLE;
            printf("PASS stage=resize-recreate frame=%llu extent=%ux%u images=%u\n",
                   (unsigned long long)(frame + 1), extent.width, extent.height,
                   swapchain_image_count);
        }
    }
    uint64_t present_ns = monotonic_ns() - present_start;
    printf("PASS stage=present frames=%llu last_image=%u duration_ms=%.3f fps=%.2f\n",
           (unsigned long long)frame_count, image_index,
           (double)present_ns / 1000000.0,
           (double)frame_count * 1000000000.0 / (double)present_ns);
    fflush(stdout);

    struct timespec visible = {
        .tv_sec = (time_t)(hold_ms / 1000),
        .tv_nsec = (long)((hold_ms % 1000) * 1000000),
    };
    nanosleep(&visible, NULL);
    vkDeviceWaitIdle(device);
    vkDestroySemaphore(device, rendered, NULL);
    vkDestroySemaphore(device, acquired, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyImageView(device, image_view, NULL);
    free(images);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
    printf("PASS stage=clean-exit\n");
    return 0;
}
