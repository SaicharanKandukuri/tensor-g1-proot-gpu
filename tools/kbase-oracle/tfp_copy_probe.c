#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct copy_state {
   unsigned char *base;
   unsigned width;
   unsigned height;
   unsigned row_bytes;
   uint32_t tight_crc;
   int active;
   int guard;
};

static _Thread_local struct copy_state state;

static uint32_t
crc32_update(uint32_t crc, const unsigned char *data, size_t size)
{
   crc = ~crc;
   for (size_t i = 0; i < size; i++) {
      crc ^= data[i];
      for (unsigned bit = 0; bit < 8; bit++)
         crc = (crc >> 1) ^ (0xedb88320u & -(int32_t)(crc & 1));
   }
   return ~crc;
}

static uint32_t
row_crc(const unsigned char *base, unsigned stride, unsigned row_bytes,
        unsigned height)
{
   uint32_t crc = 0;
   for (unsigned y = 0; y < height; y++)
      crc = crc32_update(crc, base + (size_t)y * stride, row_bytes);
   return crc;
}

XImage *
XGetSubImage(Display *display, Drawable drawable, int x, int y,
             unsigned width, unsigned height, unsigned long plane_mask,
             int format, XImage *dest_image, int dest_x, int dest_y)
{
   static XImage *(*real_xgetsubimage)(Display *, Drawable, int, int,
                                      unsigned, unsigned, unsigned long, int,
                                      XImage *, int, int);
   if (!real_xgetsubimage)
      real_xgetsubimage = dlsym(RTLD_NEXT, "XGetSubImage");

   XImage *result = real_xgetsubimage(display, drawable, x, y, width, height,
                                     plane_mask, format, dest_image,
                                     dest_x, dest_y);

   if ((width == 500 && height == 396) ||
       (width == 640 && height == 516)) {
      state.base = (unsigned char *)dest_image->data;
      state.width = width;
      state.height = height;
      state.row_bytes = width * 4;
      state.tight_crc = row_crc(state.base, state.row_bytes,
                                state.row_bytes, state.height);
      state.guard = 1;
      dprintf(2,
              "TFP_XGET drawable=0x%lx xy=%d,%d extent=%ux%u "
              "ximage_stride=%d tight_crc=%08x samples=%08x,%08x,%08x,%08x\n",
              (unsigned long)drawable, x, y, width, height,
              dest_image->bytes_per_line, state.tight_crc,
              *(uint32_t *)(state.base),
              *(uint32_t *)(state.base + (size_t)30 * state.row_bytes),
              *(uint32_t *)(state.base + (size_t)60 * state.row_bytes),
              *(uint32_t *)(state.base + (size_t)(height - 1) * state.row_bytes));
      state.guard = 0;
      state.active = 1;
   }

   return result;
}

void *
memmove(void *dest, const void *src, size_t size)
{
   static void *(*real_memmove)(void *, const void *, size_t);
   if (!real_memmove)
      real_memmove = dlsym(RTLD_NEXT, "memmove");

   void *result = real_memmove(dest, src, size);

   if (state.active && !state.guard && size == state.row_bytes &&
       src == state.base + state.row_bytes) {
      unsigned stride = (unsigned)((unsigned char *)dest - state.base);
      uint32_t expanded_crc =
         row_crc(state.base, stride, state.row_bytes, state.height);
      state.guard = 1;
      dprintf(2,
              "TFP_MEMMOVE extent=%ux%u transfer_stride=%u "
              "tight_crc=%08x expanded_crc=%08x crc_match=%d "
              "samples=%08x,%08x,%08x,%08x\n",
              state.width, state.height, stride, state.tight_crc,
              expanded_crc, state.tight_crc == expanded_crc,
              *(uint32_t *)(state.base),
              *(uint32_t *)(state.base + (size_t)30 * stride),
              *(uint32_t *)(state.base + (size_t)60 * stride),
              *(uint32_t *)(state.base + (size_t)(state.height - 1) * stride));
      state.guard = 0;
      state.active = 0;
   }

   return result;
}
