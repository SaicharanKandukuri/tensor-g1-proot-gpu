#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <libdrm/drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/dri3.h>

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

static GLuint
compile_shader(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);
   GLint ok = GL_FALSE;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[1024];
      glGetShaderInfoLog(shader, sizeof(log), NULL, log);
      fprintf(stderr, "EGL_IMPORT_FAIL stage=compile log=%s\n", log);
      exit(5);
   }
   return shader;
}

static int
has_extension(const char *extensions, const char *name)
{
   size_t length = strlen(name);
   for (const char *match = extensions; match && (match = strstr(match, name));
        match += length) {
      if ((match == extensions || match[-1] == ' ') &&
          (match[length] == '\0' || match[length] == ' '))
         return 1;
   }
   return 0;
}

int
main(void)
{
   Display *xdisplay = XOpenDisplay(NULL);
   if (!xdisplay) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=XOpenDisplay\n");
      return 1;
   }

   int screen = DefaultScreen(xdisplay);
   Pixmap pixmap = XCreatePixmap(xdisplay, RootWindow(xdisplay, screen),
                                 WIDTH, HEIGHT,
                                 DefaultDepth(xdisplay, screen));
   GC gc = XCreateGC(xdisplay, pixmap, 0, NULL);
   XSetForeground(xdisplay, gc, 0x00e53935);
   XFillRectangle(xdisplay, pixmap, gc, 0, 0, WIDTH / 2, HEIGHT);
   XSetForeground(xdisplay, gc, 0x0043a047);
   XFillRectangle(xdisplay, pixmap, gc, WIDTH / 2, 0, WIDTH / 2, HEIGHT);
   XSync(xdisplay, False);

   xcb_connection_t *connection = XGetXCBConnection(xdisplay);
   XSetEventQueueOwner(xdisplay, XCBOwnsEventQueue);
   xcb_dri3_buffer_from_pixmap_cookie_t cookie =
      xcb_dri3_buffer_from_pixmap(connection, pixmap);
   xcb_dri3_buffer_from_pixmap_reply_t *reply =
      xcb_dri3_buffer_from_pixmap_reply(connection, cookie, NULL);
   if (!reply || reply->nfd != 1) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=buffer_from_pixmap\n");
      return 2;
   }
   int fd = xcb_dri3_buffer_from_pixmap_reply_fds(connection, reply)[0];

   EGLDisplay display = eglGetDisplay((EGLNativeDisplayType)xdisplay);
   EGLint major = 0, minor = 0;
   if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=eglInitialize error=0x%x\n",
              eglGetError());
      return 3;
   }

   const char *egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
   if (!has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=extension extensions=%s\n",
              egl_extensions ? egl_extensions : "(null)");
      return 4;
   }

   const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   EGLConfig config;
   EGLint config_count = 0;
   if (!eglChooseConfig(display, config_attributes, &config, 1,
                        &config_count) || config_count != 1) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=eglChooseConfig error=0x%x\n",
              eglGetError());
      return 5;
   }
   const EGLint pbuffer_attributes[] = {
      EGL_WIDTH, WIDTH,
      EGL_HEIGHT, HEIGHT,
      EGL_NONE,
   };
   EGLSurface surface = eglCreatePbufferSurface(display, config,
                                                pbuffer_attributes);
   const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                         context_attributes);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context)) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=context error=0x%x\n",
              eglGetError());
      return 6;
   }

   PFNEGLCREATEIMAGEKHRPROC create_image =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   PFNEGLDESTROYIMAGEKHRPROC destroy_image =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
         eglGetProcAddress("glEGLImageTargetTexture2DOES");
   if (!create_image || !destroy_image || !image_target_texture) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=entrypoints\n");
      return 7;
   }

   const EGLint image_attributes[] = {
      EGL_WIDTH, WIDTH,
      EGL_HEIGHT, HEIGHT,
      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
      EGL_DMA_BUF_PLANE0_FD_EXT, fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)reply->stride,
      EGL_NONE,
   };
   EGLImageKHR image = create_image(display, EGL_NO_CONTEXT,
                                    EGL_LINUX_DMA_BUF_EXT, NULL,
                                    image_attributes);
   if (image == EGL_NO_IMAGE_KHR) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=eglCreateImage error=0x%x\n",
              eglGetError());
      return 8;
   }

   static const char vertex_source[] =
      "attribute vec2 position; attribute vec2 uv; varying vec2 texcoord;"
      "void main(){gl_Position=vec4(position,0.,1.);texcoord=uv;}";
   static const char fragment_source[] =
      "precision mediump float; uniform sampler2D source; varying vec2 texcoord;"
      "void main(){gl_FragColor=texture2D(source,texcoord);}";
   GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_source);
   GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
   GLuint program = glCreateProgram();
   glAttachShader(program, vertex_shader);
   glAttachShader(program, fragment_shader);
   glBindAttribLocation(program, 0, "position");
   glBindAttribLocation(program, 1, "uv");
   glLinkProgram(program);
   GLint linked = GL_FALSE;
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   if (!linked) {
      fprintf(stderr, "EGL_IMPORT_FAIL stage=link\n");
      return 9;
   }

   GLuint texture;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   image_target_texture(GL_TEXTURE_2D, image);

   static const GLfloat vertices[] = {
      -1.f, -1.f, 0.f, 0.f,
       1.f, -1.f, 1.f, 0.f,
      -1.f,  1.f, 0.f, 1.f,
       1.f,  1.f, 1.f, 1.f,
   };
   glViewport(0, 0, WIDTH, HEIGHT);
   glClearColor(0.f, 0.f, 0.f, 1.f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(program);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                         vertices);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                         vertices + 2);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glFinish();

   uint8_t *pixels = malloc((size_t)WIDTH * HEIGHT * 4);
   glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   GLenum gl_error = glGetError();
   uint32_t crc = crc32_update(0, pixels, (size_t)WIDTH * HEIGHT * 4);
   unsigned non_black = 0;
   for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; i++)
      non_black += pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2];
   size_t left = ((size_t)HEIGHT / 2 * WIDTH + 16) * 4;
   size_t right = ((size_t)HEIGHT / 2 * WIDTH + 200) * 4;
   printf("EGL_IMPORT_RESULT egl=%d.%d renderer=%s crc=%08x non_black=%u "
          "left=%02x%02x%02x%02x right=%02x%02x%02x%02x gl_error=0x%x\n",
          major, minor, glGetString(GL_RENDERER), crc, non_black,
          pixels[left], pixels[left + 1], pixels[left + 2], pixels[left + 3],
          pixels[right], pixels[right + 1], pixels[right + 2],
          pixels[right + 3], gl_error);

   free(pixels);
   glDeleteTextures(1, &texture);
   glDeleteProgram(program);
   glDeleteShader(vertex_shader);
   glDeleteShader(fragment_shader);
   destroy_image(display, image);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   close(fd);
   free(reply);
   XCloseDisplay(xdisplay);
   return gl_error != GL_NO_ERROR || non_black != WIDTH * HEIGHT;
}
