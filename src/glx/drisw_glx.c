/*
 * Copyright 2008 George Sapountzis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#if defined(GLX_DIRECT_RENDERING)

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shm.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include "glxclient.h"
#include <dlfcn.h>
#include "dri_common.h"
#include "drisw_priv.h"
#ifdef HAVE_LIBDRM
#include "dri3_priv.h"
#endif
#include <X11/extensions/shmproto.h>
#include <assert.h>
#include <limits.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_xcb.h>
#include "util/u_debug.h"
#include "util/log.h"
#include "kopper_interface.h"
#include "loader_dri_helper.h"
#include "dri_drawable.h"
#include "dri_screen.h"
#include "dri_util.h"
#include "mesa/glapi/glapi/glapi.h"
#include "dispatch.h"
#include "x11_display.h"

static int xshm_error = 0;
static int xshm_opcode = -1;

#ifdef HAVE_PAN_KMOD_KBASE
static const __DRIimageLoaderExtension rawDri3ImageLoaderExtension;
#endif

static struct drisw_drawable *
drisw_loader_to_drawable(struct dri_drawable *draw, void *loader_private)
{
#ifdef HAVE_PAN_KMOD_KBASE
   if (draw->screen->image.loader == &rawDri3ImageLoaderExtension &&
       draw->is_window) {
      struct loader_dri3_drawable *loader_drawable = loader_private;
      return container_of(loader_drawable, struct drisw_drawable,
                          loader_drawable);
   }
#endif
   return loader_private;
}

/**
 * Catches potential Xlib errors.
 */
static int
handle_xerror(Display *dpy, XErrorEvent *event)
{
   (void) dpy;

   assert(xshm_opcode != -1);
   if (event->request_code != xshm_opcode)
      return 0;

   xshm_error = event->error_code;
   return 0;
}

static Bool
XCreateDrawable(struct drisw_drawable * pdp, int shmid, Display * dpy)
{
   if (pdp->ximage) {
      XDestroyImage(pdp->ximage);
      pdp->ximage = NULL;
      if ((pdp->shminfo.shmid > 0) && (shmid != pdp->shminfo.shmid))
         XShmDetach(dpy, &pdp->shminfo);
   }

   if (!xshm_error && shmid >= 0) {
      pdp->shminfo.shmid = shmid;
      pdp->ximage = XShmCreateImage(dpy,
                                    NULL,
                                    pdp->xDepth,
                                    ZPixmap,              /* format */
                                    NULL,                 /* data */
                                    &pdp->shminfo,        /* shminfo */
                                    0, 0);                /* width, height */
      if (pdp->ximage != NULL) {
         int (*old_handler)(Display *, XErrorEvent *);

         /* dispatch pending errors */
         XSync(dpy, False);

         old_handler = XSetErrorHandler(handle_xerror);
         /* This may trigger the X protocol error we're ready to catch: */
         XShmAttach(dpy, &pdp->shminfo);
         XSync(dpy, False);

         if (xshm_error) {
         /* we are on a remote display, this error is normal, don't print it */
            XDestroyImage(pdp->ximage);
            pdp->ximage = NULL;
         }

         (void) XSetErrorHandler(old_handler);
      }
   }

   if (pdp->ximage == NULL) {
      pdp->shminfo.shmid = -1;
      pdp->ximage = XCreateImage(dpy,
                                 NULL,
                                 pdp->xDepth,
                                 ZPixmap, 0,             /* format, offset */
                                 NULL,                   /* data */
                                 0, 0,                   /* width, height */
                                 32,                     /* bitmap_pad */
                                 0);                     /* bytes_per_line */
   }

  /**
   * swrast does not handle 24-bit depth with 24 bpp, so let X do the
   * the conversion for us.
   */
  if (pdp->ximage->bits_per_pixel == 24)
     pdp->ximage->bits_per_pixel = 32;

   return True;
}

static void
XDestroyDrawable(struct drisw_drawable * pdp, Display * dpy, XID drawable)
{
   if (pdp->ximage)
      XDestroyImage(pdp->ximage);

   if (pdp->shminfo.shmid > 0)
      XShmDetach(dpy, &pdp->shminfo);

   XFreeGC(dpy, pdp->gc);
}

/**
 * swrast loader functions
 */

static void
get_drawable_geometry(struct dri_drawable * draw,
                      int *x, int *y, int *w, int *h,
                      void *loaderPrivate)
{
   struct drisw_drawable *pdp =
      drisw_loader_to_drawable(draw, loaderPrivate);
   __GLXDRIdrawable *pdraw = &(pdp->base);
   Display *dpy = pdraw->psc->dpy;
   Drawable drawable;

   Window root;
   unsigned uw, uh, bw, depth;

   drawable = pdraw->xDrawable;

   uw = uh = 0;
   XGetGeometry(dpy, drawable, &root, x, y, &uw, &uh, &bw, &depth);
   *w = uw;
   *h = uh;
}

static void
swrastGetDrawableInfo(struct dri_drawable * draw,
                      int *x, int *y, int *w, int *h,
                      void *loaderPrivate)
{
   get_drawable_geometry(draw, x, y, w, h, loaderPrivate);
}

/**
 * Align renderbuffer pitch.
 *
 * This should be chosen by the driver and the loader (libGL, xserver/glx)
 * should use the driver provided pitch.
 *
 * It seems that the xorg loader (that is the xserver loading swrast_dri for
 * indirect rendering, not client-side libGL) requires that the pitch is
 * exactly the image width padded to 32 bits. XXX
 *
 * The above restriction can probably be overcome by using ScratchPixmap and
 * CopyArea in the xserver, similar to ShmPutImage, and setting the width of
 * the scratch pixmap to 'pitch / cpp'.
 */
static inline int
bytes_per_line(unsigned pitch_bits, unsigned mul)
{
   unsigned mask = mul - 1;

   return ((pitch_bits + mask) & ~mask) / 8;
}

static void
swrastXPutImage(struct dri_drawable * draw, int op,
                int srcx, int srcy, int x, int y,
                int w, int h, int stride,
                int shmid, char *data, void *loaderPrivate)
{
   struct drisw_drawable *pdp =
      drisw_loader_to_drawable(draw, loaderPrivate);
   __GLXDRIdrawable *pdraw = &(pdp->base);
   Display *dpy = pdraw->psc->dpy;
   Drawable drawable;
   XImage *ximage;
   GC gc = pdp->gc;

   if (!pdp->ximage || shmid != pdp->shminfo.shmid) {
      if (!XCreateDrawable(pdp, shmid, dpy))
         return;
   }

   drawable = pdraw->xDrawable;
   ximage = pdp->ximage;
   ximage->bytes_per_line = stride ? stride : bytes_per_line(w * ximage->bits_per_pixel, 32);
   ximage->data = data;

   ximage->width = ximage->bytes_per_line / ((ximage->bits_per_pixel + 7)/ 8);
   ximage->height = h;

   if (pdp->shminfo.shmid >= 0) {
      XShmPutImage(dpy, drawable, gc, ximage, srcx, srcy, x, y, w, h, False);
      XSync(dpy, False);
   } else {
      XPutImage(dpy, drawable, gc, ximage, srcx, srcy, x, y, w, h);
   }
   ximage->data = NULL;
}

static void
swrastPutImageShm(struct dri_drawable * draw, int op,
                  int x, int y, int w, int h, int stride,
                  int shmid, char *shmaddr, unsigned offset,
                  void *loaderPrivate)
{
   struct drisw_drawable *pdp =
      drisw_loader_to_drawable(draw, loaderPrivate);

   if (!pdp)
      return;

   pdp->shminfo.shmaddr = shmaddr;
   swrastXPutImage(draw, op, 0, 0, x, y, w, h, stride, shmid,
                   shmaddr + offset, loaderPrivate);
}

static void
swrastPutImageShm2(struct dri_drawable * draw, int op,
                   int x, int y,
                   int w, int h, int stride,
                   int shmid, char *shmaddr, unsigned offset,
                   void *loaderPrivate)
{
   struct drisw_drawable *pdp =
      drisw_loader_to_drawable(draw, loaderPrivate);

   if (!pdp)
      return;

   pdp->shminfo.shmaddr = shmaddr;
   swrastXPutImage(draw, op, x, 0, x, y, w, h, stride, shmid,
                   shmaddr + offset, loaderPrivate);
}

static void
swrastPutImage2(struct dri_drawable * draw, int op,
                int x, int y, int w, int h, int stride,
                char *data, void *loaderPrivate)
{
   if (!loaderPrivate)
      return;

   swrastXPutImage(draw, op, 0, 0, x, y, w, h, stride, -1,
                   data, loaderPrivate);
}

static void
swrastPutImage(struct dri_drawable * draw, int op,
               int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   if (!loaderPrivate)
      return;

   swrastXPutImage(draw, op, 0, 0, x, y, w, h, 0, -1,
                   data, loaderPrivate);
}

static void
swrastGetImage2(struct dri_drawable * read,
                int x, int y, int w, int h, int stride,
                char *data, void *loaderPrivate)
{
   struct drisw_drawable *prp =
      drisw_loader_to_drawable(read, loaderPrivate);
   __GLXDRIdrawable *pread = &(prp->base);
   Display *dpy = pread->psc->dpy;
   Drawable readable;
   XImage *ximage;

   if (!prp->ximage || prp->shminfo.shmid >= 0) {
      if (!XCreateDrawable(prp, -1, dpy))
         return;
   }

   readable = pread->xDrawable;

   ximage = prp->ximage;
   ximage->data = data;
   ximage->width = w;
   ximage->height = h;
   ximage->bytes_per_line = stride ? stride : bytes_per_line(w * ximage->bits_per_pixel, 32);

   XGetSubImage(dpy, readable, x, y, w, h, ~0L, ZPixmap, ximage, 0, 0);

   ximage->data = NULL;
}

static void
swrastGetImage(struct dri_drawable * read,
               int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   swrastGetImage2(read, x, y, w, h, 0, data, loaderPrivate);
}

static GLboolean
swrastGetImageShm2(struct dri_drawable * read,
                   int x, int y, int w, int h,
                   int shmid, void *loaderPrivate)
{
   struct drisw_drawable *prp =
      drisw_loader_to_drawable(read, loaderPrivate);
   __GLXDRIdrawable *pread = &(prp->base);
   Display *dpy = pread->psc->dpy;
   Drawable readable;
   XImage *ximage;

   if (!prp->ximage || shmid != prp->shminfo.shmid) {
      if (!XCreateDrawable(prp, shmid, dpy))
         return GL_FALSE;
   }

   if (prp->shminfo.shmid == -1)
      return GL_FALSE;
   readable = pread->xDrawable;

   ximage = prp->ximage;
   ximage->data = prp->shminfo.shmaddr; /* no offset */
   ximage->width = w;
   ximage->height = h;
   ximage->bytes_per_line = bytes_per_line(w * ximage->bits_per_pixel, 32);

   XShmGetImage(dpy, readable, ximage, x, y, ~0L);
   return GL_TRUE;
}

static void
swrastGetImageShm(struct dri_drawable * read,
                  int x, int y, int w, int h,
                  int shmid, void *loaderPrivate)
{
   swrastGetImageShm2(read, x, y, w, h, shmid, loaderPrivate);
}

static const __DRIswrastLoaderExtension swrastLoaderExtension_shm = {
   .base = {__DRI_SWRAST_LOADER, 6 },

   .getDrawableInfo     = swrastGetDrawableInfo,
   .putImage            = swrastPutImage,
   .getImage            = swrastGetImage,
   .putImage2           = swrastPutImage2,
   .getImage2           = swrastGetImage2,
   .putImageShm         = swrastPutImageShm,
   .getImageShm         = swrastGetImageShm,
   .putImageShm2        = swrastPutImageShm2,
   .getImageShm2        = swrastGetImageShm2,
};

static const __DRIswrastLoaderExtension swrastLoaderExtension = {
   .base = {__DRI_SWRAST_LOADER, 3 },

   .getDrawableInfo     = swrastGetDrawableInfo,
   .putImage            = swrastPutImage,
   .getImage            = swrastGetImage,
   .putImage2           = swrastPutImage2,
   .getImage2           = swrastGetImage2,
};

#ifdef HAVE_PAN_KMOD_KBASE
static void
raw_dri3_set_drawable_size(struct loader_dri3_drawable *draw,
                           int width, int height)
{
   (void)draw;
   (void)width;
   (void)height;
}

static bool
raw_dri3_in_current_context(struct loader_dri3_drawable *draw)
{
   struct drisw_drawable *pdp =
      container_of(draw, struct drisw_drawable, loader_drawable);
   struct glx_context *gc = __glXGetCurrentContext();

   return gc != &dummyContext && gc->psc == pdp->base.psc;
}

static struct dri_context *
raw_dri3_get_dri_context(struct loader_dri3_drawable *draw)
{
   struct glx_context *gc = __glXGetCurrentContext();

   return gc != &dummyContext ? gc->driContext : NULL;
}

static struct dri_screen *
raw_dri3_get_dri_screen(void)
{
   struct glx_context *gc = __glXGetCurrentContext();

   return gc != &dummyContext ? gc->psc->frontend_screen : NULL;
}

static void
raw_dri3_flush_drawable(struct loader_dri3_drawable *draw, unsigned flags)
{
   loader_dri3_flush(draw, flags, __DRI2_THROTTLE_SWAPBUFFER);
}

static int
raw_dri3_get_buffers(struct dri_drawable *dri_drawable,
                     unsigned int format, uint32_t *stamp,
                     void *loader_private, uint32_t buffer_mask,
                     struct __DRIimageList *buffers)
{
   if (!dri_drawable->is_window)
      return 0;

   return loader_dri3_get_buffers(dri_drawable, format, stamp,
                                  loader_private, buffer_mask, buffers);
}

static void
raw_dri3_update_tex_buffer(struct dri_drawable *drawable,
                           struct dri_context *ctx,
                           struct pipe_resource *resource)
{
   struct loader_dri3_drawable *loader_drawable = drawable->loaderPrivate;
   struct drisw_drawable *pdp =
      container_of(loader_drawable, struct drisw_drawable, loader_drawable);
   struct loader_dri3_buffer *front =
      loader_drawable->buffers[LOADER_DRI3_FRONT_ID];

   /*
    * A server-owned pixmap is normally imported directly and already backs
    * the texture passed here.  Some X11 pixmaps cannot be exported through
    * DRI3, though.  drisw_allocate_textures() then creates a local fallback
    * resource, which still needs the ordinary XGetImage upload path.
    */
   if (front && front->image && front->image->texture == resource)
      return;

   if (debug_get_bool_option("PAN_RAW_TFP_TRACE", false))
      mesa_logi("raw-dri3 TFP falling back to XGetImage drawable=0x%x "
                "extent=%ux%u",
                loader_drawable->drawable, resource->width0,
                resource->height0);

   pdp->raw_dri3_fallback_update_tex_buffer(drawable, ctx, resource);
}

static const struct loader_dri3_vtable raw_dri3_vtable = {
   .set_drawable_size = raw_dri3_set_drawable_size,
   .in_current_context = raw_dri3_in_current_context,
   .get_dri_context = raw_dri3_get_dri_context,
   .get_dri_screen = raw_dri3_get_dri_screen,
   .flush_drawable = raw_dri3_flush_drawable,
};

static void
raw_dri3_flush_front_buffer(struct dri_drawable *dri_drawable,
                            void *loader_private)
{
   if (!dri_drawable->is_window)
      return;

   struct loader_dri3_drawable *draw = loader_private;

   loader_dri3_flush(draw, __DRI2_FLUSH_DRAWABLE,
                     __DRI2_THROTTLE_FLUSHFRONT);
   dri_invalidate_drawable(dri_drawable);
   loader_dri3_wait_gl(draw);
}

static void
raw_dri3_flush_swap_buffers(struct dri_drawable *dri_drawable,
                            void *loader_private)
{
   if (!dri_drawable->is_window)
      return;

   loader_dri3_swapbuffer_barrier(loader_private);
}

static const __DRIimageLoaderExtension rawDri3ImageLoaderExtension = {
   .base = { __DRI_IMAGE_LOADER, 3 },
   .getBuffers = raw_dri3_get_buffers,
   .flushFrontBuffer = raw_dri3_flush_front_buffer,
   .flushSwapBuffers = raw_dri3_flush_swap_buffers,
};
#endif

static_assert(sizeof(struct kopper_vk_surface_create_storage) >= sizeof(VkXcbSurfaceCreateInfoKHR), "");

static void
kopperSetSurfaceCreateInfo(void *_draw, struct kopper_loader_info *out)
{
    __GLXDRIdrawable *draw = _draw;
    VkXcbSurfaceCreateInfoKHR *xcb = (VkXcbSurfaceCreateInfoKHR *)&out->bos;

    xcb->sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    xcb->pNext = NULL;
    xcb->flags = 0;
    xcb->connection = XGetXCBConnection(draw->psc->dpy);
    xcb->window = draw->xDrawable;
}

static void
kopperGetDrawableInfo(struct dri_drawable * draw,
                      int *w, int *h,
                      void *loaderPrivate)
{
   int x = 0, y = 0;
   get_drawable_geometry(draw, &x, &y, w, h, loaderPrivate);
}

static const __DRIkopperLoaderExtension kopperLoaderExtension = {
    .base = { __DRI_KOPPER_LOADER, 1 },

    .SetSurfaceCreateInfo   = kopperSetSurfaceCreateInfo,
    .GetDrawableInfo        = kopperGetDrawableInfo,
};

static const __DRIextension *loader_extensions_shm[] = {
   &swrastLoaderExtension_shm.base,
   &kopperLoaderExtension.base,
   NULL
};

static const __DRIextension *loader_extensions_noshm[] = {
   &swrastLoaderExtension.base,
   &kopperLoaderExtension.base,
   NULL
};

#ifdef HAVE_PAN_KMOD_KBASE
static const __DRIextension *loader_extensions_raw_dri3[] = {
   &swrastLoaderExtension.base,
   &rawDri3ImageLoaderExtension.base,
   NULL
};
#endif

static const __DRIextension *kopper_extensions_noshm[] = {
   &swrastLoaderExtension.base,
   &kopperLoaderExtension.base,
   NULL
};

/**
 * GLXDRI functions
 */


static void
drisw_wait_gl(struct glx_context *context)
{
#ifdef HAVE_PAN_KMOD_KBASE
   struct drisw_drawable *pdp = (struct drisw_drawable *)
      GetGLXDRIDrawable(context->currentDpy, context->currentDrawable);

   if (pdp && pdp->raw_dri3) {
      loader_dri3_wait_gl(&pdp->loader_drawable);
      return;
   }
#endif
   CALL_Finish(GET_DISPATCH(), ());
}

static void
drisw_wait_x(struct glx_context *context)
{
#ifdef HAVE_PAN_KMOD_KBASE
   struct drisw_drawable *pdp = (struct drisw_drawable *)
      GetGLXDRIDrawable(context->currentDpy, context->currentDrawable);

   if (pdp && pdp->raw_dri3) {
      loader_dri3_wait_x(&pdp->loader_drawable);
      return;
   }
#endif
   XSync(context->currentDpy, False);
}

int
kopper_get_buffer_age(__GLXDRIdrawable *pdraw);
int
kopper_get_buffer_age(__GLXDRIdrawable *pdraw)
{
   return kopperQueryBufferAge(pdraw->dri_drawable);
}

static const struct glx_context_vtable drisw_context_vtable = {
   .destroy             = dri_destroy_context,
   .bind                = dri_bind_context,
   .unbind              = dri_unbind_context,
   .wait_gl             = drisw_wait_gl,
   .wait_x              = drisw_wait_x,
   .copy_context        = __glXCopyContext,
   .swap_buffers        = __glXSwapBuffers,
};

static void
driswDestroyDrawable(__GLXDRIdrawable * pdraw)
{
   struct drisw_drawable *pdp = (struct drisw_drawable *) pdraw;

#ifdef HAVE_PAN_KMOD_KBASE
   if (pdp->raw_dri3)
      loader_dri3_drawable_fini(&pdp->loader_drawable);
   else
#endif
      driDestroyDrawable(pdp->base.dri_drawable);

   XDestroyDrawable(pdp, pdraw->psc->dpy, pdraw->drawable);
   free(pdp);
}

static __GLXDRIdrawable *
driswCreateDrawable(struct glx_screen *base, XID xDrawable,
                    GLXDrawable drawable, int type,
                    struct glx_config *modes)
{
   struct drisw_drawable *pdp;
   __GLXDRIconfigPrivate *config = (__GLXDRIconfigPrivate *) modes;
   unsigned depth;
   struct drisw_screen *psc = (struct drisw_screen *) base;
   Display *dpy = psc->base.dpy;

   xcb_connection_t *conn = XGetXCBConnection(dpy);
   xcb_generic_error_t *error;
   xcb_get_geometry_cookie_t cookie = xcb_get_geometry(conn, xDrawable);
   xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(conn, cookie, &error);
   if (reply)
      depth = reply->depth;
   free(reply);
   if (!reply || error)
      return NULL;

   pdp = calloc(1, sizeof(*pdp));
   if (!pdp)
      return NULL;

   pdp->base.xDrawable = xDrawable;
   pdp->base.drawable = drawable;
   pdp->base.psc = &psc->base;
   pdp->config = modes;
   pdp->gc = XCreateGC(dpy, xDrawable, 0, NULL);
   pdp->xDepth = 0;

   /* Use the visual depth, if this fbconfig corresponds to a visual */
   if (pdp->config->visualID != 0) {
      int matches = 0;
      XVisualInfo *visinfo, template;

      template.visualid = pdp->config->visualID;
      template.screen = pdp->config->screen;
      visinfo = XGetVisualInfo(dpy, VisualIDMask | VisualScreenMask,
                               &template, &matches);

      if (visinfo && matches) {
         pdp->xDepth = visinfo->depth;
         XFree(visinfo);
      }
   }

   /* Otherwise, or if XGetVisualInfo failed, ask the server */
   if (pdp->xDepth == 0) {
      pdp->xDepth = depth;
   }

   pdp->swapInterval = dri_get_initial_swap_interval(psc->base.frontend_screen);
#ifdef HAVE_PAN_KMOD_KBASE
   const bool raw_dri3_pixmap = type == GLX_PIXMAP_BIT;
   pdp->raw_dri3 = psc->raw_dri3 &&
                   ((type & GLX_WINDOW_BIT) || raw_dri3_pixmap);
   if (pdp->raw_dri3) {
      if (loader_dri3_drawable_init(conn, xDrawable,
                                    raw_dri3_pixmap ?
                                       LOADER_DRI3_DRAWABLE_PIXMAP :
                                       LOADER_DRI3_DRAWABLE_WINDOW,
                                    psc->base.frontend_screen,
                                    psc->base.frontend_screen,
                                    !raw_dri3_pixmap, false,
                                    config->driConfig,
                                    &raw_dri3_vtable,
                                    &pdp->loader_drawable)) {
         XDestroyDrawable(pdp, psc->base.dpy, xDrawable);
         free(pdp);
         return NULL;
      }
      pdp->loader_drawable.use_raw_fd_modifier = true;
      pdp->base.dri_drawable = pdp->loader_drawable.dri_drawable;
      pdp->base.dri_drawable->is_window = true;
      if (raw_dri3_pixmap) {
         pdp->raw_dri3_fallback_update_tex_buffer =
            pdp->base.dri_drawable->update_tex_buffer;
         pdp->base.dri_drawable->update_tex_buffer =
            raw_dri3_update_tex_buffer;
      }
   } else
#endif
      pdp->base.dri_drawable =
         dri_create_drawable(psc->base.frontend_screen, config->driConfig,
                             !(type & GLX_WINDOW_BIT), pdp);
   if (psc->kopper)
      kopperSetSwapInterval(pdp->base.dri_drawable, pdp->swapInterval);

   if (!pdp->base.dri_drawable) {
      XDestroyDrawable(pdp, psc->base.dpy, xDrawable);
      free(pdp);
      return NULL;
   }

   pdp->base.destroyDrawable = driswDestroyDrawable;

   return &pdp->base;
}

static int64_t
driswSwapBuffers(__GLXDRIdrawable * pdraw,
                 int64_t target_msc, int64_t divisor, int64_t remainder,
                 Bool flush)
{
   struct drisw_screen *psc = (struct drisw_screen *) pdraw->psc;

#ifdef HAVE_PAN_KMOD_KBASE
   struct drisw_drawable *pdp = (struct drisw_drawable *)pdraw;
   if (pdp->raw_dri3) {
      unsigned flags = __DRI2_FLUSH_DRAWABLE;

      if (flush)
         flags |= __DRI2_FLUSH_CONTEXT;

      CALL_Finish(GET_DISPATCH(), ());
      return loader_dri3_swap_buffers_msc(&pdp->loader_drawable,
                                           target_msc, divisor, remainder,
                                           flags, NULL, 0, false);
   }
#endif

   (void) target_msc;
   (void) divisor;
   (void) remainder;

   if (psc->kopper)
       return kopperSwapBuffers(pdraw->dri_drawable, flush ? __DRI2_FLUSH_CONTEXT : 0);

   if (flush) {
      CALL_Flush(GET_DISPATCH(), ());
   }

   driSwapBuffers(pdraw->dri_drawable);

   return 0;
}

static void
drisw_copy_sub_buffer(__GLXDRIdrawable * pdraw,
                      int x, int y, int width, int height, Bool flush)
{
#ifdef HAVE_PAN_KMOD_KBASE
   struct drisw_drawable *pdp = (struct drisw_drawable *)pdraw;
   if (pdp->raw_dri3) {
      CALL_Finish(GET_DISPATCH(), ());
      loader_dri3_copy_sub_buffer(&pdp->loader_drawable, x, y,
                                  width, height, flush);
      return;
   }
#endif

   if (flush) {
      CALL_Flush(GET_DISPATCH(), ());
   }

   driswCopySubBuffer(pdraw->dri_drawable, x, y, width, height);
}

static int
driswKopperSetSwapInterval(__GLXDRIdrawable *pdraw, int interval)
{
   struct drisw_drawable *pdp = (struct drisw_drawable *) pdraw;
   struct drisw_screen *psc = (struct drisw_screen *) pdp->base.psc;

   if (!dri_valid_swap_interval(psc->base.frontend_screen, interval))
      return GLX_BAD_VALUE;

#ifdef HAVE_PAN_KMOD_KBASE
   if (pdp->raw_dri3)
      loader_dri3_set_swap_interval(&pdp->loader_drawable, interval);
   else
#endif
      kopperSetSwapInterval(pdp->base.dri_drawable, interval);
   pdp->swapInterval = interval;

   return 0;
}

static int
kopperGetSwapInterval(__GLXDRIdrawable *pdraw)
{
   struct drisw_drawable *pdp = (struct drisw_drawable *) pdraw;

   return pdp->swapInterval;
}

static int
kopperWaitForMSC(__GLXDRIdrawable *pdraw, int64_t target_msc, int64_t divisor,
                 int64_t remainder, int64_t *ust, int64_t *msc, int64_t *sbc)
{
#ifdef HAVE_PAN_KMOD_KBASE
   struct drisw_drawable *pdp = (struct drisw_drawable *)pdraw;
   if (pdp->raw_dri3)
      return loader_dri3_wait_for_msc(&pdp->loader_drawable, target_msc,
                                      divisor, remainder, ust, msc, sbc);
#endif
   return kopperGetSyncValues(pdraw->dri_drawable, target_msc, divisor, remainder, ust, msc, sbc);
}

static int
kopperGetDrawableMSC(struct glx_screen *psc, __GLXDRIdrawable *pdraw,
                     int64_t *ust, int64_t *msc, int64_t *sbc)
{
#ifdef HAVE_PAN_KMOD_KBASE
   struct drisw_drawable *pdp = (struct drisw_drawable *)pdraw;
   if (pdp->raw_dri3)
      return loader_dri3_wait_for_msc(&pdp->loader_drawable, 0, 0, 0,
                                      ust, msc, sbc);
#endif
   return kopperGetSyncValues(pdraw->dri_drawable, 0, 0, 0, ust, msc, sbc);
}

#ifdef HAVE_PAN_KMOD_KBASE
static int
rawDri3WaitForSBC(__GLXDRIdrawable *pdraw, int64_t target_sbc, int64_t *ust,
                  int64_t *msc, int64_t *sbc)
{
   struct drisw_drawable *pdp = (struct drisw_drawable *)pdraw;

   return loader_dri3_wait_for_sbc(&pdp->loader_drawable, target_sbc,
                                   ust, msc, sbc);
}
#endif


struct glx_screen *
driswCreateScreen(int screen, struct glx_display *priv, enum glx_driver glx_driver, bool driver_name_is_inferred)
{
   __GLXDRIscreen *psp;
   struct drisw_screen *psc;
   const __DRIextension **loader_extensions_local;
   bool kopper_disable = debug_get_bool_option("LIBGL_KOPPER_DISABLE", false);

   /* this is only relevant if zink bits are set */
   glx_driver &= (GLX_DRIVER_ZINK_INFER | GLX_DRIVER_ZINK_YES);
   const char *driver = glx_driver && !kopper_disable ? "zink" : "swrast";

   psc = calloc(1, sizeof *psc);
   if (psc == NULL)
      return NULL;
   psc->kopper = !strcmp(driver, "zink");
   psc->base.driverName = strdup(driver);
#ifdef HAVE_PAN_KMOD_KBASE
   psc->raw_dri3 = !psc->kopper && priv->has_multibuffer &&
                   debug_get_bool_option("PAN_KBASE_GL_DRI3", false);
#endif

   if (glx_driver)
      loader_extensions_local = kopper_extensions_noshm;
#ifdef HAVE_PAN_KMOD_KBASE
   else if (psc->raw_dri3)
      loader_extensions_local = loader_extensions_raw_dri3;
#endif
   else if (debug_get_bool_option("PAN_EXPERIMENTAL_KBASE_GL", false))
      loader_extensions_local = loader_extensions_noshm;
#ifdef HAVE_SYS_SHM_H
   else if (!x11_xcb_display_supports_xshm(XGetXCBConnection(priv->dpy), &xshm_opcode))
      loader_extensions_local = loader_extensions_noshm;
#endif
   else
      loader_extensions_local = loader_extensions_shm;
   priv->driver = glx_driver ? GLX_DRIVER_ZINK_YES : GLX_DRIVER_SW;

   if (!dri_screen_init(&psc->base, priv, screen, -1, loader_extensions_local, driver_name_is_inferred)) {
      if (!glx_driver || !driver_name_is_inferred)
         ErrorMessageF("glx: failed to create drisw screen\n");
      goto handle_error;
   }

   psc->base.context_vtable = &drisw_context_vtable;
   psp = &psc->base.driScreen;
   psc->base.can_EXT_texture_from_pixmap = true;
   psp->createDrawable = driswCreateDrawable;
   psp->swapBuffers = driswSwapBuffers;

   if (!glx_driver)
      psp->copySubBuffer = drisw_copy_sub_buffer;

   if (psc->kopper || psc->raw_dri3) {
      psp->setSwapInterval = driswKopperSetSwapInterval;
      psp->getSwapInterval = kopperGetSwapInterval;
      psp->maxSwapInterval = psc->raw_dri3 ? INT_MAX : 1;
      psp->getDrawableMSC = kopperGetDrawableMSC;
      psp->waitForMSC = kopperWaitForMSC;
#ifdef HAVE_PAN_KMOD_KBASE
      if (psc->raw_dri3)
         psp->waitForSBC = rawDri3WaitForSBC;
#endif
   }

   return &psc->base;

 handle_error:

   glx_screen_cleanup(&psc->base);
   free(psc);

   if (glx_driver & GLX_DRIVER_ZINK_YES && !driver_name_is_inferred)
      CriticalErrorMessageF("failed to load driver: %s\n", driver);

   return NULL;
}

#endif /* GLX_DIRECT_RENDERING */
