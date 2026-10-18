#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xcb/dri3.h>

#include "../../include/drm-uapi/mali_kbase_ioctl.h"

#define WIDTH 256
#define HEIGHT 192

static uint32_t
crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
   crc = ~crc;
   for (size_t i = 0; i < size; i++) {
      crc ^= data[i];
      for (unsigned bit = 0; bit < 8; bit++)
         crc = (crc >> 1) ^ (0xedb88320u & -(int32_t)(crc & 1));
   }
   return ~crc;
}

static int
dma_buf_sync(int fd, uint64_t flags)
{
   struct dma_buf_sync sync = {.flags = flags};
   int result;
   do {
      result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
   } while (result < 0 && errno == EINTR);
   return result;
}

static int
probe_kbase_import(int dma_buf_fd, size_t logical_size, uint32_t expected_crc)
{
   int mali_fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
   if (mali_fd < 0) {
      fprintf(stderr, "KBASE_IMPORT_FAIL stage=open errno=%d\n", errno);
      return -1;
   }

   struct kbase_ioctl_version_check version = {0};
   if (ioctl(mali_fd, KBASE_IOCTL_VERSION_CHECK_JM, &version) < 0) {
      fprintf(stderr, "KBASE_IMPORT_FAIL stage=version errno=%d\n", errno);
      close(mali_fd);
      return -1;
   }

   struct kbase_ioctl_set_flags set_flags = {.create_flags = 0};
   if (ioctl(mali_fd, KBASE_IOCTL_SET_FLAGS, &set_flags) < 0) {
      fprintf(stderr, "KBASE_IMPORT_FAIL stage=set_flags errno=%d\n", errno);
      close(mali_fd);
      return -1;
   }

   void *tracking = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, mali_fd,
                         BASE_MEM_MAP_TRACKING_HANDLE);
   if (tracking == MAP_FAILED) {
      fprintf(stderr, "KBASE_IMPORT_FAIL stage=tracking errno=%d\n", errno);
      close(mali_fd);
      return -1;
   }

   int import_fd = dma_buf_fd;
   union kbase_ioctl_mem_import request = {
      .in = {
         .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                  BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                  BASE_MEM_IMPORT_SHARED | BASE_MEM_COHERENT_SYSTEM,
         .phandle = (uintptr_t)&import_fd,
         .type = BASE_MEM_IMPORT_TYPE_UMM,
      },
   };
   if (ioctl(mali_fd, KBASE_IOCTL_MEM_IMPORT, &request) < 0) {
      fprintf(stderr, "KBASE_IMPORT_FAIL stage=mem_import errno=%d\n", errno);
      munmap(tracking, 4096);
      close(mali_fd);
      return -1;
   }

   fprintf(stderr,
           "KBASE_IMPORT_META version=%u.%u pages=%llu flags=%llx "
           "cookie=%llx logical_size=%zu\n",
           version.major, version.minor,
           (unsigned long long)request.out.va_pages,
           (unsigned long long)request.out.flags,
           (unsigned long long)request.out.gpu_va, logical_size);

   size_t map_size = (size_t)request.out.va_pages * 4096;
   void *mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        mali_fd, request.out.gpu_va);
   if (mapping == MAP_FAILED) {
      fprintf(stderr,
              "KBASE_IMPORT_FAIL stage=import_mmap errno=%d cookie=%" PRIx64
              " pages=%" PRIu64 " flags=%" PRIx64 "\n",
              errno, (uint64_t)request.out.gpu_va,
              (uint64_t)request.out.va_pages, (uint64_t)request.out.flags);
      struct kbase_ioctl_mem_free free_request = {
         .gpu_addr = request.out.gpu_va,
      };
      ioctl(mali_fd, KBASE_IOCTL_MEM_FREE, &free_request);
      munmap(tracking, 4096);
      close(mali_fd);
      return -1;
   }

   /*
    * The kbase mapping establishes the GPU VA for an imported UMM region,
    * but Pixel's r46 kernel rejects CPU faults through that mapping.  Read
    * through a fresh dma-buf mapping instead, exactly as the kmod backend
    * does, and bracket the access with the dma-buf CPU ownership protocol.
    */
   void *cpu_mapping = mmap(NULL, logical_size, PROT_READ, MAP_SHARED,
                            dma_buf_fd, 0);
   if (cpu_mapping == MAP_FAILED) {
      fprintf(stderr, "KBASE_IMPORT_FAIL stage=cpu_mmap errno=%d\n", errno);
      munmap(mapping, map_size);
      munmap(tracking, 4096);
      close(mali_fd);
      return -1;
   }

   uint32_t before_sync_crc = crc32_update(0, cpu_mapping, logical_size);
   errno = 0;
   int sync_start = dma_buf_sync(dma_buf_fd,
                                 DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
   int sync_start_errno = errno;
   uint32_t after_sync_crc = crc32_update(0, cpu_mapping, logical_size);
   errno = 0;
   int sync_end = dma_buf_sync(dma_buf_fd,
                               DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
   int sync_end_errno = errno;
   printf("KBASE_IMPORT_RESULT version=%u.%u pages=%" PRIu64
          " flags=%" PRIx64 " cookie=%" PRIx64
          " gpu_mapping=%p before_sync=%08x after_sync=%08x expected=%08x "
          "equal=%u sync=%d:%d/%d:%d\n",
          version.major, version.minor, (uint64_t)request.out.va_pages,
          (uint64_t)request.out.flags, (uint64_t)request.out.gpu_va,
          mapping, before_sync_crc,
          after_sync_crc, expected_crc, after_sync_crc == expected_crc,
          sync_start, sync_start_errno, sync_end, sync_end_errno);
   munmap(cpu_mapping, logical_size);

   bool same_va = request.out.flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP);
   munmap(mapping, map_size);
   if (!same_va) {
      struct kbase_ioctl_mem_free free_request = {
         .gpu_addr = request.out.gpu_va,
      };
      ioctl(mali_fd, KBASE_IOCTL_MEM_FREE, &free_request);
   }
   munmap(tracking, 4096);
   close(mali_fd);
   return after_sync_crc == expected_crc ? 0 : 1;
}

int
main(void)
{
   Display *display = XOpenDisplay(NULL);
   if (!display) {
      fprintf(stderr, "EXPORT_FAIL stage=XOpenDisplay\n");
      return 1;
   }

   int screen = DefaultScreen(display);
   Pixmap pixmap = XCreatePixmap(display, RootWindow(display, screen),
                                 WIDTH, HEIGHT, DefaultDepth(display, screen));
   GC gc = XCreateGC(display, pixmap, 0, NULL);
   XSetForeground(display, gc, 0x00e53935);
   XFillRectangle(display, pixmap, gc, 0, 0, WIDTH / 2, HEIGHT);
   XSetForeground(display, gc, 0x0043a047);
   XFillRectangle(display, pixmap, gc, WIDTH / 2, 0, WIDTH / 2, HEIGHT);
   XSetForeground(display, gc, 0x00ffffff);
   XFillRectangle(display, pixmap, gc, 31, 0, 1, HEIGHT);
   XSync(display, False);

   XImage *image = XGetImage(display, pixmap, 0, 0, WIDTH, HEIGHT,
                             AllPlanes, ZPixmap);
   if (!image) {
      fprintf(stderr, "EXPORT_FAIL stage=XGetImage\n");
      return 2;
   }
   uint32_t x_crc = crc32_update(0, (const uint8_t *)image->data,
                                 (size_t)image->bytes_per_line * HEIGHT);

   xcb_connection_t *connection = XGetXCBConnection(display);
   XSetEventQueueOwner(display, XCBOwnsEventQueue);
   xcb_dri3_buffer_from_pixmap_cookie_t cookie =
      xcb_dri3_buffer_from_pixmap(connection, pixmap);
   xcb_generic_error_t *error = NULL;
   xcb_dri3_buffer_from_pixmap_reply_t *reply =
      xcb_dri3_buffer_from_pixmap_reply(connection, cookie, &error);
   if (!reply || error) {
      fprintf(stderr, "EXPORT_FAIL stage=buffer_from_pixmap error=%u\n",
              error ? error->error_code : 0);
      free(error);
      return 3;
   }

   int *fds = xcb_dri3_buffer_from_pixmap_reply_fds(connection, reply);
   if (!fds || reply->nfd != 1) {
      fprintf(stderr, "EXPORT_FAIL stage=fds nfd=%u\n", reply->nfd);
      return 4;
   }

   int fd = fds[0];
   struct stat stat_buffer;
   int stat_result = fstat(fd, &stat_buffer);
   size_t logical_size = (size_t)reply->stride * reply->height;
   size_t map_size = stat_result == 0 && stat_buffer.st_size > 0
                        ? (size_t)stat_buffer.st_size
                        : logical_size;
   void *mapping = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
   if (mapping == MAP_FAILED) {
      fprintf(stderr, "EXPORT_FAIL stage=mmap errno=%d size=%zu\n",
              errno, map_size);
      return 5;
   }

   errno = 0;
   int sync_start = dma_buf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
   int sync_start_errno = errno;
   uint32_t fd_crc = crc32_update(0, mapping, logical_size);
   const uint32_t *words = mapping;
   uint32_t sample_left = words[(HEIGHT / 2) * (reply->stride / 4) + 16];
   uint32_t sample_right = words[(HEIGHT / 2) * (reply->stride / 4) + 200];
   errno = 0;
   int sync_end = dma_buf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
   int sync_end_errno = errno;

   printf("EXPORT_RESULT x_crc=%08x fd_crc=%08x equal=%u "
          "x_stride=%d fd_stride=%u dimensions=%ux%u depth=%u bpp=%u "
          "nfd=%u reply_size=%u stat_size=%lld samples=%08x,%08x "
          "sync=%d:%d/%d:%d\n",
          x_crc, fd_crc, x_crc == fd_crc,
          image->bytes_per_line, reply->stride, reply->width, reply->height,
          reply->depth, reply->bpp, reply->nfd, reply->size,
          stat_result == 0 ? (long long)stat_buffer.st_size : -1ll,
          sample_left, sample_right,
          sync_start, sync_start_errno, sync_end, sync_end_errno);
   fflush(stdout);

   int kbase_result = probe_kbase_import(fd, logical_size, fd_crc);

   munmap(mapping, map_size);
   close(fd);
   free(reply);
   XDestroyImage(image);
   XFreeGC(display, gc);
   XFreePixmap(display, pixmap);
   XCloseDisplay(display);
   return fd_crc != x_crc || kbase_result != 0;
}
