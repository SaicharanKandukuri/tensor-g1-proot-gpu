#define _POSIX_C_SOURCE 200809L

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void
fill_pattern(uint8_t *pixels, unsigned width, unsigned height)
{
   static const uint8_t colors[][4] = {
      {0xe5, 0x39, 0x35, 0xff}, {0x43, 0xa0, 0x47, 0xff},
      {0x1e, 0x88, 0xe5, 0xff}, {0xfd, 0xd8, 0x35, 0xff},
      {0x8e, 0x24, 0xaa, 0xff}, {0x00, 0xac, 0xc1, 0xff},
      {0xfb, 0x8c, 0x00, 0xff}, {0xf5, 0xf5, 0xf5, 0xff},
   };

   for (unsigned y = 0; y < height; y++) {
      const uint8_t *color = colors[(y / 30) % 8];
      for (unsigned x = 0; x < width; x++) {
         uint8_t *pixel = pixels + ((size_t)y * width + x) * 4;
         pixel[0] = color[0];
         pixel[1] = color[1];
         pixel[2] = color[2];
         pixel[3] = color[3];
      }
   }
}

int
main(int argc, char **argv)
{
   unsigned frame_count = argc > 1 ? strtoul(argv[1], NULL, 10) : 600;
   int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, None};
   Display *display = XOpenDisplay(NULL);
   if (!display)
      return 1;

   XVisualInfo *visual = glXChooseVisual(display, DefaultScreen(display),
                                        attributes);
   if (!visual)
      return 2;

   Colormap colormap = XCreateColormap(display,
                                      RootWindow(display, visual->screen),
                                      visual->visual, AllocNone);
   XSetWindowAttributes window_attributes = {
      .colormap = colormap,
      .event_mask = ExposureMask | StructureNotifyMask,
   };
   Window window = XCreateWindow(display, RootWindow(display, visual->screen),
                                 80, 80, 640, 480, 0, visual->depth,
                                 InputOutput, visual->visual,
                                 CWColormap | CWEventMask, &window_attributes);
   XStoreName(display, window, "GLX CPU texture upload probe");
   XMapWindow(display, window);
   XSync(display, False);

   GLXContext context = glXCreateContext(display, visual, NULL, True);
   if (!context || !glXMakeCurrent(display, window, context))
      return 3;

   GLuint texture;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glEnable(GL_TEXTURE_2D);

   struct timespec delay = {.tv_nsec = 16 * 1000 * 1000};
   for (unsigned frame = 0; frame < frame_count; frame++) {
      unsigned width = (frame / 30) % 2 ? 500 : 640;
      unsigned height = (frame / 30) % 2 ? 360 : 480;
      size_t size = (size_t)width * height * 4;
      uint8_t *pixels = malloc(size);
      if (!pixels)
         return 4;

      fill_pattern(pixels, width, height);
      XResizeWindow(display, window, width, height);
      XSync(display, False);
      glViewport(0, 0, width, height);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      free(pixels);

      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glBegin(GL_QUADS);
      glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
      glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
      glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
      glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
      glEnd();
      glXSwapBuffers(display, window);
      nanosleep(&delay, NULL);
   }

   glDeleteTextures(1, &texture);
   glXMakeCurrent(display, None, NULL);
   glXDestroyContext(display, context);
   XDestroyWindow(display, window);
   XFreeColormap(display, colormap);
   XFree(visual);
   XCloseDisplay(display);
   return 0;
}
