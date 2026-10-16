#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>

#define WIDTH 64
#define HEIGHT 64
#define PIXEL_BYTES 4

static void
fail(const char *operation, VkResult result)
{
   fprintf(stderr, "%s failed: %d\n", operation, result);
   exit(1);
}

static void
check(const char *operation, VkResult result)
{
   if (result != VK_SUCCESS)
      fail(operation, result);
}

static uint32_t *
read_words(const char *path, size_t *byte_count)
{
   FILE *file = fopen(path, "rb");
   if (!file) {
      perror(path);
      exit(1);
   }
   if (fseek(file, 0, SEEK_END) != 0) {
      perror("fseek");
      exit(1);
   }
   long length = ftell(file);
   if (length <= 0 || length % 4 != 0) {
      fprintf(stderr, "%s has invalid SPIR-V length\n", path);
      exit(1);
   }
   rewind(file);
   uint32_t *words = malloc((size_t)length);
   if (!words || fread(words, 1, (size_t)length, file) != (size_t)length) {
      fprintf(stderr, "could not read %s\n", path);
      exit(1);
   }
   fclose(file);
   *byte_count = (size_t)length;
   return words;
}

static uint32_t
memory_type(const VkPhysicalDeviceMemoryProperties *properties,
            uint32_t allowed, VkMemoryPropertyFlags required)
{
   for (uint32_t i = 0; i < properties->memoryTypeCount; ++i) {
      if ((allowed & (1u << i)) &&
          (properties->memoryTypes[i].propertyFlags & required) == required)
         return i;
   }
   fprintf(stderr, "no compatible Vulkan memory type\n");
   exit(1);
}

static uint64_t
hash_bytes(const uint8_t *bytes, size_t length)
{
   uint64_t hash = UINT64_C(1469598103934665603);
   for (size_t i = 0; i < length; ++i) {
      hash ^= bytes[i];
      hash *= UINT64_C(1099511628211);
   }
   return hash;
}

static uint64_t
monotonic_ns(void)
{
   struct timespec value;
   if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
      perror("clock_gettime");
      exit(1);
   }
   return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
          (uint64_t)value.tv_nsec;
}

static uint32_t
parse_count(const char *value, const char *name)
{
   errno = 0;
   char *end = NULL;
   unsigned long count = strtoul(value, &end, 10);
   if (errno || !end || *end || !count || count > UINT32_MAX) {
      fprintf(stderr, "invalid %s: %s\n", name, value);
      exit(2);
   }
   return (uint32_t)count;
}

static void
submit_and_wait(VkDevice device, VkQueue queue, const VkSubmitInfo *submit,
                VkFence fence)
{
   check("vkResetFences", vkResetFences(device, 1, &fence));
   check("vkQueueSubmit", vkQueueSubmit(queue, 1, submit, fence));
   check("vkWaitForFences",
         vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
}

int
main(int argc, char **argv)
{
   if (argc < 3 || argc > 5) {
      fprintf(stderr,
              "usage: %s triangle.vert.spv triangle.frag.spv [iterations] "
              "[warmup]\n",
              argv[0]);
      return 2;
   }
   uint32_t iterations = argc >= 4 ? parse_count(argv[3], "iterations") : 1;
   uint32_t warmup = argc >= 5 ? parse_count(argv[4], "warmup") : 0;

   VkApplicationInfo application = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "kbase-oracle-triangle",
      .applicationVersion = 1,
      .pEngineName = "kbase-oracle",
      .engineVersion = 1,
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
   };
   VkInstance instance;
   check("vkCreateInstance", vkCreateInstance(&instance_info, NULL, &instance));

   uint32_t physical_count = 0;
   check("vkEnumeratePhysicalDevices(count)",
         vkEnumeratePhysicalDevices(instance, &physical_count, NULL));
   if (!physical_count) {
      fprintf(stderr, "no Vulkan physical device\n");
      return 1;
   }
   VkPhysicalDevice *physical_devices =
      calloc(physical_count, sizeof(*physical_devices));
   check("vkEnumeratePhysicalDevices",
         vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices));
   VkPhysicalDevice physical = physical_devices[0];
   free(physical_devices);

   VkPhysicalDeviceProperties device_properties;
   VkPhysicalDeviceMemoryProperties memory_properties;
   vkGetPhysicalDeviceProperties(physical, &device_properties);
   vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

   uint32_t queue_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
   VkQueueFamilyProperties *queue_properties =
      calloc(queue_count, sizeof(*queue_properties));
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count,
                                            queue_properties);
   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < queue_count; ++i) {
      if (queue_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         queue_family = i;
         break;
      }
   }
   free(queue_properties);
   if (queue_family == UINT32_MAX) {
      fprintf(stderr, "no graphics queue\n");
      return 1;
   }

   float queue_priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device;
   check("vkCreateDevice", vkCreateDevice(physical, &device_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {WIDTH, HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   check("vkCreateImage", vkCreateImage(device, &image_info, NULL, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   VkMemoryAllocateInfo image_memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = memory_type(&memory_properties,
                                     image_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory image_memory;
   check("vkAllocateMemory(image)",
         vkAllocateMemory(device, &image_memory_info, NULL, &image_memory));
   check("vkBindImageMemory", vkBindImageMemory(device, image, image_memory, 0));

   VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView view;
   check("vkCreateImageView", vkCreateImageView(device, &view_info, NULL, &view));

   VkAttachmentDescription attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   };
   VkAttachmentReference color_reference = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_reference,
   };
   VkSubpassDependency dependency = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
   };
   VkRenderPassCreateInfo render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dependency,
   };
   VkRenderPass render_pass;
   check("vkCreateRenderPass",
         vkCreateRenderPass(device, &render_pass_info, NULL, &render_pass));

   VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &view,
      .width = WIDTH,
      .height = HEIGHT,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   check("vkCreateFramebuffer",
         vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

   size_t vertex_size;
   size_t fragment_size;
   uint32_t *vertex_words = read_words(argv[1], &vertex_size);
   uint32_t *fragment_words = read_words(argv[2], &fragment_size);
   VkShaderModuleCreateInfo vertex_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = vertex_size,
      .pCode = vertex_words,
   };
   VkShaderModuleCreateInfo fragment_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = fragment_size,
      .pCode = fragment_words,
   };
   VkShaderModule vertex_module;
   VkShaderModule fragment_module;
   check("vkCreateShaderModule(vertex)",
         vkCreateShaderModule(device, &vertex_module_info, NULL, &vertex_module));
   check("vkCreateShaderModule(fragment)",
         vkCreateShaderModule(device, &fragment_module_info, NULL,
                              &fragment_module));
   free(vertex_words);
   free(fragment_words);

   VkPipelineShaderStageCreateInfo stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex_module,
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment_module,
         .pName = "main",
      },
   };
   VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = WIDTH,
      .height = HEIGHT,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
   };
   VkRect2D scissor = {.extent = {WIDTH, HEIGHT}};
   VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };
   VkPipelineRasterizationStateCreateInfo rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendAttachmentState blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
   };
   VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout pipeline_layout;
   check("vkCreatePipelineLayout",
         vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout));
   VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   VkPipeline pipeline;
   check("vkCreateGraphicsPipelines",
         vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                   NULL, &pipeline));

   VkDeviceSize output_size = WIDTH * HEIGHT * PIXEL_BYTES;
   VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = output_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer output_buffer;
   check("vkCreateBuffer",
         vkCreateBuffer(device, &buffer_info, NULL, &output_buffer));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, output_buffer, &buffer_requirements);
   uint32_t output_memory_type = memory_type(
      &memory_properties, buffer_requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   VkMemoryPropertyFlags output_flags =
      memory_properties.memoryTypes[output_memory_type].propertyFlags;
   VkMemoryAllocateInfo buffer_memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = output_memory_type,
   };
   VkDeviceMemory output_memory;
   check("vkAllocateMemory(buffer)",
         vkAllocateMemory(device, &buffer_memory_info, NULL, &output_memory));
   check("vkBindBufferMemory",
         vkBindBufferMemory(device, output_buffer, output_memory, 0));

   VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool command_pool;
   check("vkCreateCommandPool",
         vkCreateCommandPool(device, &pool_info, NULL, &command_pool));
   VkCommandBufferAllocateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   check("vkAllocateCommandBuffers",
         vkAllocateCommandBuffers(device, &command_info, &command_buffer));
   VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   check("vkBeginCommandBuffer",
         vkBeginCommandBuffer(command_buffer, &begin_info));
   VkClearValue clear = {.color = {{0.03125f, 0.0625f, 0.125f, 1.0f}}};
   VkRenderPassBeginInfo render_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {.extent = {WIDTH, HEIGHT}},
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdDraw(command_buffer, 3, 1, 0, 0);
   vkCmdEndRenderPass(command_buffer);
   VkImageMemoryBarrier image_barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &image_barrier);
   VkBufferImageCopy copy = {
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .layerCount = 1,
      },
      .imageExtent = {WIDTH, HEIGHT, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output_buffer, 1,
                          &copy);
   VkBufferMemoryBarrier buffer_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = output_buffer,
      .size = VK_WHOLE_SIZE,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                        &buffer_barrier, 0, NULL);
   check("vkEndCommandBuffer", vkEndCommandBuffer(command_buffer));

   VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   check("vkCreateFence", vkCreateFence(device, &fence_info, NULL, &fence));
   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   for (uint32_t i = 0; i < warmup; ++i)
      submit_and_wait(device, queue, &submit, fence);
   uint64_t start_ns = monotonic_ns();
   for (uint32_t i = 0; i < iterations; ++i)
      submit_and_wait(device, queue, &submit, fence);
   uint64_t elapsed_ns = monotonic_ns() - start_ns;

   if (!(output_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      VkMappedMemoryRange range = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = output_memory,
         .offset = 0,
         .size = VK_WHOLE_SIZE,
      };
      check("vkInvalidateMappedMemoryRanges",
            vkInvalidateMappedMemoryRanges(device, 1, &range));
   }
   uint8_t *pixels;
   check("vkMapMemory",
         vkMapMemory(device, output_memory, 0, output_size, 0, (void **)&pixels));
   uint64_t hash = hash_bytes(pixels, (size_t)output_size);
   size_t colored = 0;
   for (size_t i = 0; i < WIDTH * HEIGHT; ++i)
      colored += pixels[i * 4] > 200 && pixels[i * 4 + 1] > 40;
   size_t center = ((HEIGHT / 2) * WIDTH + WIDTH / 2) * PIXEL_BYTES;
   double elapsed_ms = (double)elapsed_ns / 1000000.0;
   double frames_per_second = (double)iterations * 1000000000.0 / elapsed_ns;
   double frame_us = (double)elapsed_ns / iterations / 1000.0;
   printf("device=%s api=%u.%u.%u hash=%016" PRIx64
          " colored=%zu center=%u,%u,%u,%u iterations=%u warmup=%u "
          "elapsed_ms=%.3f frame_us=%.3f fps=%.3f\n",
          device_properties.deviceName,
          VK_VERSION_MAJOR(device_properties.apiVersion),
          VK_VERSION_MINOR(device_properties.apiVersion),
          VK_VERSION_PATCH(device_properties.apiVersion), hash, colored,
          pixels[center], pixels[center + 1], pixels[center + 2],
          pixels[center + 3], iterations, warmup, elapsed_ms, frame_us,
          frames_per_second);
   vkUnmapMemory(device, output_memory);

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyBuffer(device, output_buffer, NULL);
   vkFreeMemory(device, output_memory, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyShaderModule(device, fragment_module, NULL);
   vkDestroyShaderModule(device, vertex_module, NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL);
   vkDestroyRenderPass(device, render_pass, NULL);
   vkDestroyImageView(device, view, NULL);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, image_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return 0;
}
