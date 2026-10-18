#define _POSIX_C_SOURCE 200809L

#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OUTPUT_WIDTH 256
#define OUTPUT_HEIGHT 192

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

int
main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: %s WINDOW_XID [frames]\n", argv[0]);
      return 2;
   }

   const Window source_window = strtoul(argv[1], NULL, 0);
   const unsigned frames = argc > 2 ? strtoul(argv[2], NULL, 10) : 8;
   const int already_redirected = argc > 3 &&
                                  !strcmp(argv[3], "already-redirected");
   Display *display = XOpenDisplay(NULL);
   if (!display) {
      fprintf(stderr, "WINDOW_TFP_FAIL stage=XOpenDisplay\n");
      return 3;
   }

   XWindowAttributes source_attributes;
   if (!XGetWindowAttributes(display, source_window, &source_attributes)) {
      fprintf(stderr, "WINDOW_TFP_FAIL stage=XGetWindowAttributes xid=0x%lx\n",
              source_window);
      return 4;
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
      fprintf(stderr, "WINDOW_TFP_FAIL stage=glXChooseFBConfig\n");
      return 5;
   }

   GLXFBConfig config = configs[0];
   XVisualInfo *visual = glXGetVisualFromFBConfig(display, config);
   Colormap colormap = XCreateColormap(display,
                                       RootWindow(display, visual->screen),
                                       visual->visual, AllocNone);
   XSetWindowAttributes output_attributes = {
      .colormap = colormap,
      .event_mask = ExposureMask | StructureNotifyMask,
   };
   Window output_window = XCreateWindow(
      display, RootWindow(display, visual->screen), 680, 100,
      OUTPUT_WIDTH, OUTPUT_HEIGHT, 0, visual->depth, InputOutput,
      visual->visual, CWColormap | CWEventMask, &output_attributes);
   XStoreName(display, output_window, "GLX redirected-window TFP probe");
   XMapWindow(display, output_window);
   XSync(display, False);

   if (!already_redirected) {
      XCompositeRedirectWindow(display, source_window,
                               CompositeRedirectAutomatic);
      XSync(display, False);
   }
   Pixmap source_pixmap = XCompositeNameWindowPixmap(display, source_window);
   const int pixmap_attributes[] = {
      GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
      GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT,
      None,
   };
   GLXPixmap glx_pixmap = glXCreatePixmap(display, config, source_pixmap,
                                          pixmap_attributes);
   GLXContext context = glXCreateNewContext(display, config, GLX_RGBA_TYPE,
                                             NULL, True);
   if (!glx_pixmap || !context ||
       !glXMakeCurrent(display, output_window, context)) {
      fprintf(stderr, "WINDOW_TFP_FAIL stage=create glx_pixmap=%lu context=%p\n",
              glx_pixmap, (void *)context);
      return 6;
   }

   PFNGLXBINDTEXIMAGEEXTPROC bind_tex_image =
      (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddress(
         (const GLubyte *)"glXBindTexImageEXT");
   PFNGLXRELEASETEXIMAGEEXTPROC release_tex_image =
      (PFNGLXRELEASETEXIMAGEEXTPROC)glXGetProcAddress(
         (const GLubyte *)"glXReleaseTexImageEXT");
   if (!bind_tex_image || !release_tex_image)
      return 7;

   GLuint texture;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   uint8_t *pixels = malloc((size_t)OUTPUT_WIDTH * OUTPUT_HEIGHT * 4);
   unsigned black = 0;
   unsigned changed = 0;
   uint32_t previous_crc = 0;

   for (unsigned frame = 0; frame < frames; frame++) {
      glXWaitX();
      bind_tex_image(display, glx_pixmap, GLX_FRONT_LEFT_EXT, NULL);
      GLenum bind_error = glGetError();

      glViewport(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT);
      glClearColor(0.0, 0.0, 0.0, 1.0);
      glClear(GL_COLOR_BUFFER_BIT);
      glEnable(GL_TEXTURE_2D);
      glBegin(GL_QUADS);
      glTexCoord2f(0, 0); glVertex2f(-1, -1);
      glTexCoord2f(1, 0); glVertex2f( 1, -1);
      glTexCoord2f(1, 1); glVertex2f( 1,  1);
      glTexCoord2f(0, 1); glVertex2f(-1,  1);
      glEnd();
      glFinish();
      glReadPixels(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT, GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels);
      GLenum render_error = glGetError();
      release_tex_image(display, glx_pixmap, GLX_FRONT_LEFT_EXT);
      GLenum release_error = glGetError();
      glXSwapBuffers(display, output_window);

      unsigned non_black = 0;
      for (size_t i = 0; i < (size_t)OUTPUT_WIDTH * OUTPUT_HEIGHT; i++)
         non_black += pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2];
      const uint32_t crc = crc32_update(0, pixels,
                                        (size_t)OUTPUT_WIDTH * OUTPUT_HEIGHT * 4);
      black += !non_black;
      changed += frame && crc != previous_crc;
      printf("WINDOW_TFP_FRAME frame=%u crc=%08x non_black=%u "
             "errors=%04x/%04x/%04x\n",
             frame, crc, non_black, bind_error, render_error, release_error);
      fflush(stdout);
      previous_crc = crc;
      const struct timespec delay = {.tv_nsec = 100 * 1000 * 1000};
      nanosleep(&delay, NULL);
   }

   printf("WINDOW_TFP_SUMMARY source=0x%lx source_extent=%dx%d frames=%u "
          "changed=%u black=%u renderer=%s\n",
          source_window, source_attributes.width, source_attributes.height,
          frames, changed, black, glGetString(GL_RENDERER));
   return black == frames;
}
