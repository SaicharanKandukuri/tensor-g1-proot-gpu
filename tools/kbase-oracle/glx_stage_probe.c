#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <unistd.h>

static void
mark(const char *stage)
{
   fprintf(stderr, "[glx-stage] %s\n", stage);
   fflush(stderr);
}

int
main(void)
{
   int attributes[] = {
      GLX_RGBA,
      GLX_DOUBLEBUFFER,
      GLX_DEPTH_SIZE, 24,
      None,
   };
   Display *display;
   XVisualInfo *visual;
   Colormap colormap;
   XSetWindowAttributes window_attributes;
   Window window;
   GLXContext context;

   mark("XOpenDisplay");
   display = XOpenDisplay(NULL);
   if (!display)
      return 1;

   mark("glXChooseVisual");
   visual = glXChooseVisual(display, DefaultScreen(display), attributes);
   if (!visual)
      return 2;

   colormap = XCreateColormap(display, RootWindow(display, visual->screen),
                              visual->visual, AllocNone);
   window_attributes.colormap = colormap;
   window_attributes.event_mask = ExposureMask;
   window = XCreateWindow(display, RootWindow(display, visual->screen),
                          20, 20, 480, 480, 0, visual->depth, InputOutput,
                          visual->visual, CWColormap | CWEventMask,
                          &window_attributes);
   XStoreName(display, window, "Panfrost GLX stage probe");
   XMapWindow(display, window);
   XSync(display, False);

   mark("glXCreateContext");
   context = glXCreateContext(display, visual, NULL, True);
   if (!context)
      return 3;

   mark("glXMakeCurrent");
   if (!glXMakeCurrent(display, window, context))
      return 4;

   mark("glClear");
   glViewport(0, 0, 480, 480);
   glClearColor(0.1f, 0.2f, 0.8f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   mark("glFinish clear");
   glFinish();
   mark("glXSwapBuffers clear");
   glXSwapBuffers(display, window);
   XSync(display, False);

   mark("triangle draw");
   glClearColor(0.04f, 0.04f, 0.04f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glBegin(GL_TRIANGLES);
   glColor3f(1.0f, 0.1f, 0.1f);
   glVertex2f(-0.8f, -0.7f);
   glColor3f(0.1f, 1.0f, 0.1f);
   glVertex2f(0.8f, -0.7f);
   glColor3f(0.1f, 0.2f, 1.0f);
   glVertex2f(0.0f, 0.8f);
   glEnd();

   mark("glFinish triangle");
   glFinish();
   mark("glXSwapBuffers triangle");
   glXSwapBuffers(display, window);
   XSync(display, False);

   mark("resize loop");
   for (unsigned i = 0; i < 120; i++) {
      unsigned width = 360 + ((i % 7) * 24);
      unsigned height = 320 + ((i % 5) * 28);

      XMoveResizeWindow(display, window, 20 + (i % 9), 20 + (i % 11),
                        width, height);
      XSync(display, False);
      glViewport(0, 0, width, height);
      glClearColor((i % 3) / 2.0f, ((i + 1) % 3) / 2.0f,
                   ((i + 2) % 3) / 2.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glXSwapBuffers(display, window);
   }
   mark("resize loop complete");
   sleep(5);

   glXMakeCurrent(display, None, NULL);
   glXDestroyContext(display, context);
   XDestroyWindow(display, window);
   XFreeColormap(display, colormap);
   XFree(visual);
   XCloseDisplay(display);
   return 0;
}
