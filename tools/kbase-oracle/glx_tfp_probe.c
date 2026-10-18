#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static unsigned long
pack_rgb(const XVisualInfo *visual, unsigned r, unsigned g, unsigned b)
{
   unsigned long pixel = 0;
   const unsigned values[] = {r, g, b};
   const unsigned long masks[] = {
      visual->red_mask, visual->green_mask, visual->blue_mask,
   };

   for (unsigned i = 0; i < 3; i++) {
      unsigned shift = 0;
      unsigned bits = 0;
      unsigned long mask = masks[i];
      while (mask && !(mask & 1)) {
         shift++;
         mask >>= 1;
      }
      while (mask & 1) {
         bits++;
         mask >>= 1;
      }
      if (bits)
         pixel |= ((unsigned long)values[i] * ((1ul << bits) - 1) / 255)
                  << shift;
   }

   return pixel;
}

static void
paint_pixmap(Display *display, Pixmap pixmap, GC gc,
             const XVisualInfo *visual, unsigned frame)
{
   static const unsigned colors[][3] = {
      {0xe5, 0x39, 0x35},
      {0x43, 0xa0, 0x47},
      {0x1e, 0x88, 0xe5},
      {0xfd, 0xd8, 0x35},
      {0x8e, 0x24, 0xaa},
      {0x00, 0xac, 0xc1},
   };

   for (unsigned band = 0; band < 6; band++) {
      unsigned color = (band + frame) % 6;
      XSetForeground(display, gc,
                     pack_rgb(visual, colors[color][0], colors[color][1],
                              colors[color][2]));
      unsigned x0 = band * WIDTH / 6;
      unsigned x1 = (band + 1) * WIDTH / 6;
      XFillRectangle(display, pixmap, gc, x0, 0, x1 - x0, HEIGHT);
   }

   XSetForeground(display, gc, pack_rgb(visual, 0xff, 0xff, 0xff));
   XFillRectangle(display, pixmap, gc, frame % WIDTH, 0, 1, HEIGHT);
   XSync(display, False);
}

static uint32_t
render_and_read(GLuint texture, uint8_t *pixels)
{
   glViewport(0, 0, WIDTH, HEIGHT);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glBindTexture(GL_TEXTURE_2D, texture);
   glEnable(GL_TEXTURE_2D);
   glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
   glBegin(GL_QUADS);
   glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
   glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
   glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
   glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
   glEnd();
   glFinish();
   glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   return crc32_update(0, pixels, (size_t)WIDTH * HEIGHT * 4);
}

int
main(int argc, char **argv)
{
   unsigned frames = argc > 1 ? strtoul(argv[1], NULL, 10) : 12;
   Display *display = XOpenDisplay(NULL);
   if (!display) {
      fprintf(stderr, "TFP_FAIL stage=XOpenDisplay\n");
      return 1;
   }

   const char *extensions = glXQueryExtensionsString(display,
                                                      DefaultScreen(display));
   if (!extensions || !strstr(extensions, "GLX_EXT_texture_from_pixmap")) {
      fprintf(stderr, "TFP_FAIL stage=extension extensions=%s\n",
              extensions ? extensions : "(null)");
      return 2;
   }

   const int config_attributes[] = {
      GLX_X_RENDERABLE, True,
      GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT | GLX_PIXMAP_BIT,
      GLX_RENDER_TYPE, GLX_RGBA_BIT,
      GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
      GLX_RED_SIZE, 8,
      GLX_GREEN_SIZE, 8,
      GLX_BLUE_SIZE, 8,
      GLX_BIND_TO_TEXTURE_RGB_EXT, True,
      None,
   };
   int config_count = 0;
   GLXFBConfig *configs = glXChooseFBConfig(display, DefaultScreen(display),
                                            config_attributes, &config_count);
   if (!configs || !config_count) {
      fprintf(stderr, "TFP_FAIL stage=glXChooseFBConfig count=%d\n",
              config_count);
      return 3;
   }

   GLXFBConfig config = configs[0];
   XVisualInfo *visual = glXGetVisualFromFBConfig(display, config);
   if (!visual) {
      fprintf(stderr, "TFP_FAIL stage=glXGetVisualFromFBConfig\n");
      return 4;
   }

   Colormap colormap = XCreateColormap(display,
                                       RootWindow(display, visual->screen),
                                       visual->visual, AllocNone);
   XSetWindowAttributes window_attributes = {
      .colormap = colormap,
      .event_mask = ExposureMask | StructureNotifyMask,
   };
   Window window = XCreateWindow(display, RootWindow(display, visual->screen),
                                 24, 24, WIDTH, HEIGHT, 0, visual->depth,
                                 InputOutput, visual->visual,
                                 CWColormap | CWEventMask, &window_attributes);
   XStoreName(display, window, "GLX texture-from-pixmap probe");
   XMapWindow(display, window);

   Pixmap pixmap = XCreatePixmap(display, RootWindow(display, visual->screen),
                                 WIDTH, HEIGHT, visual->depth);
   GC gc = XCreateGC(display, pixmap, 0, NULL);
   const int pixmap_attributes[] = {
      GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
      GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT,
      None,
   };
   GLXPixmap glx_pixmap = glXCreatePixmap(display, config, pixmap,
                                          pixmap_attributes);
   if (!glx_pixmap) {
      fprintf(stderr, "TFP_FAIL stage=glXCreatePixmap\n");
      return 5;
   }

   GLXContext context = glXCreateNewContext(display, config,
                                             GLX_RGBA_TYPE, NULL, True);
   if (!context || !glXMakeCurrent(display, window, context)) {
      fprintf(stderr, "TFP_FAIL stage=glXMakeCurrent\n");
      return 6;
   }

   PFNGLXBINDTEXIMAGEEXTPROC bind_tex_image =
      (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddress(
         (const GLubyte *)"glXBindTexImageEXT");
   PFNGLXRELEASETEXIMAGEEXTPROC release_tex_image =
      (PFNGLXRELEASETEXIMAGEEXTPROC)glXGetProcAddress(
         (const GLubyte *)"glXReleaseTexImageEXT");
   if (!bind_tex_image || !release_tex_image) {
      fprintf(stderr, "TFP_FAIL stage=get_proc bind=%p release=%p\n",
              (void *)bind_tex_image, (void *)release_tex_image);
      return 7;
   }

   GLuint texture = 0;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   uint8_t *pixels = malloc((size_t)WIDTH * HEIGHT * 4);
   if (!pixels)
      return 8;

   uint32_t previous_crc = 0;
   unsigned changed = 0;
   unsigned black = 0;
   for (unsigned frame = 0; frame < frames; frame++) {
      paint_pixmap(display, pixmap, gc, visual, frame);
      glXWaitX();
      bind_tex_image(display, glx_pixmap, GLX_FRONT_LEFT_EXT, NULL);
      GLenum bind_error = glGetError();
      uint32_t crc = render_and_read(texture, pixels);
      GLenum render_error = glGetError();
      release_tex_image(display, glx_pixmap, GLX_FRONT_LEFT_EXT);
      GLenum release_error = glGetError();
      glXSwapBuffers(display, window);

      unsigned non_black = 0;
      for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; i++)
         non_black += pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2];
      black += !non_black;
      if (frame && crc != previous_crc)
         changed++;

      const size_t center = ((size_t)HEIGHT / 2 * WIDTH + WIDTH / 2) * 4;
      printf("TFP_FRAME frame=%u crc=%08x non_black=%u "
             "center=%02x%02x%02x%02x errors=%04x/%04x/%04x\n",
             frame, crc, non_black,
             pixels[center], pixels[center + 1], pixels[center + 2],
             pixels[center + 3], bind_error, render_error, release_error);
      fflush(stdout);
      previous_crc = crc;
   }

   printf("TFP_SUMMARY frames=%u changed=%u black=%u renderer=%s\n",
          frames, changed, black, glGetString(GL_RENDERER));

   free(pixels);
   glDeleteTextures(1, &texture);
   glXMakeCurrent(display, None, NULL);
   glXDestroyContext(display, context);
   glXDestroyPixmap(display, glx_pixmap);
   XFreeGC(display, gc);
   XFreePixmap(display, pixmap);
   XDestroyWindow(display, window);
   XFreeColormap(display, colormap);
   XFree(visual);
   XFree(configs);
   XCloseDisplay(display);
   return black || changed + 1 != frames;
}
