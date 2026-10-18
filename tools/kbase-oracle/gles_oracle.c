#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 64
#define HEIGHT 64

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

enum probe_case {
   PROBE_CLEAR,
   PROBE_CONSTANT,
   PROBE_VARYING,
};

struct sample {
   int x;
   int y;
};

static const char position_vertex_shader[] =
   "attribute vec2 position;\n"
   "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

static const char constant_fragment_shader[] =
   "precision mediump float;\n"
   "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

static const char varying_vertex_shader[] =
   "attribute vec2 position;\n"
   "attribute vec3 color;\n"
   "varying mediump vec3 interpolated_color;\n"
   "void main() {\n"
   "  gl_Position = vec4(position, 0.0, 1.0);\n"
   "  interpolated_color = color;\n"
   "}\n";

static const char varying_fragment_shader[] =
   "precision mediump float;\n"
   "varying mediump vec3 interpolated_color;\n"
   "void main() { gl_FragColor = vec4(interpolated_color, 1.0); }\n";

static void
fail_egl(const char *operation)
{
   fprintf(stderr, "%s failed: EGL error 0x%04x\n", operation, eglGetError());
   exit(1);
}

static void
check_egl(const char *operation, EGLBoolean result)
{
   if (result != EGL_TRUE)
      fail_egl(operation);
}

static void
check_gl(const char *operation)
{
   GLenum error = glGetError();
   if (error != GL_NO_ERROR) {
      fprintf(stderr, "%s failed: GL error 0x%04x\n", operation, error);
      exit(1);
   }
}

static int
has_extension(const char *extensions, const char *name)
{
   size_t length = strlen(name);
   const char *match = extensions;

   if (!extensions || strchr(name, ' '))
      return 0;

   while ((match = strstr(match, name))) {
      if ((match == extensions || match[-1] == ' ') &&
          (match[length] == '\0' || match[length] == ' '))
         return 1;
      match += length;
   }

   return 0;
}

static EGLDisplay
get_display(const char **platform)
{
   const char *extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

   if (has_extension(extensions, "EGL_EXT_platform_base") &&
       has_extension(extensions, "EGL_MESA_platform_surfaceless")) {
      PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
         (PFNEGLGETPLATFORMDISPLAYEXTPROC)
            eglGetProcAddress("eglGetPlatformDisplayEXT");
      if (get_platform_display) {
         EGLDisplay display = get_platform_display(
            EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
         if (display != EGL_NO_DISPLAY) {
            *platform = "surfaceless";
            return display;
         }
         eglGetError();
      }
   } else if (!extensions) {
      eglGetError();
   }

   *platform = "default";
   return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static EGLConfig
choose_config(EGLDisplay display)
{
   static const EGLint attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   EGLint count = 0;

   check_egl("eglChooseConfig(count)",
             eglChooseConfig(display, attributes, NULL, 0, &count));
   if (count < 1) {
      fprintf(stderr, "no RGBA8 GLES2 pbuffer EGL config\n");
      exit(1);
   }

   EGLConfig *configs = calloc((size_t)count, sizeof(*configs));
   if (!configs) {
      perror("calloc");
      exit(1);
   }
   check_egl("eglChooseConfig",
             eglChooseConfig(display, attributes, configs, count, &count));

   EGLConfig selected = NULL;
   for (EGLint i = 0; i < count; ++i) {
      EGLint red, green, blue, alpha;
      check_egl("eglGetConfigAttrib(red)",
                eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &red));
      check_egl("eglGetConfigAttrib(green)",
                eglGetConfigAttrib(display, configs[i], EGL_GREEN_SIZE,
                                   &green));
      check_egl("eglGetConfigAttrib(blue)",
                eglGetConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &blue));
      check_egl("eglGetConfigAttrib(alpha)",
                eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE,
                                   &alpha));
      if (red == 8 && green == 8 && blue == 8 && alpha == 8) {
         selected = configs[i];
         break;
      }
   }
   free(configs);

   if (!selected) {
      fprintf(stderr, "no exact RGBA8 GLES2 pbuffer EGL config\n");
      exit(1);
   }
   return selected;
}

static GLuint
compile_shader(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   if (!shader) {
      check_gl("glCreateShader");
      fprintf(stderr, "glCreateShader returned zero\n");
      exit(1);
   }
   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);

   GLint compiled = GL_FALSE;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
   if (compiled != GL_TRUE) {
      GLchar log[1024];
      GLsizei length = 0;
      glGetShaderInfoLog(shader, sizeof(log), &length, log);
      fprintf(stderr, "shader compilation failed: %.*s\n", (int)length, log);
      exit(1);
   }
   check_gl("shader compilation");
   return shader;
}

static GLuint
create_program(const char *vertex_source, const char *fragment_source,
               int use_color)
{
   GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_source);
   GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
   GLuint program = glCreateProgram();
   if (!program) {
      check_gl("glCreateProgram");
      fprintf(stderr, "glCreateProgram returned zero\n");
      exit(1);
   }

   glAttachShader(program, vertex_shader);
   glAttachShader(program, fragment_shader);
   glBindAttribLocation(program, 0, "position");
   if (use_color)
      glBindAttribLocation(program, 1, "color");
   glLinkProgram(program);

   GLint linked = GL_FALSE;
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   if (linked != GL_TRUE) {
      GLchar log[1024];
      GLsizei length = 0;
      glGetProgramInfoLog(program, sizeof(log), &length, log);
      fprintf(stderr, "program link failed: %.*s\n", (int)length, log);
      exit(1);
   }
   check_gl("program link");

   glDeleteShader(vertex_shader);
   glDeleteShader(fragment_shader);
   return program;
}

static uint64_t
fnv1a(const uint8_t *bytes, size_t length)
{
   uint64_t hash = UINT64_C(14695981039346656037);
   for (size_t i = 0; i < length; ++i) {
      hash ^= bytes[i];
      hash *= UINT64_C(1099511628211);
   }
   return hash;
}

static const uint8_t *
pixel_at(const uint8_t *pixels, int x, int y)
{
   return pixels + ((size_t)y * WIDTH + (size_t)x) * 4;
}

static int
close_byte(uint8_t actual, uint8_t expected, int tolerance)
{
   int difference = (int)actual - (int)expected;
   return difference >= -tolerance && difference <= tolerance;
}

static void
check_pixel(const char *name, const uint8_t *pixels, int x, int y,
            const uint8_t expected[4], int tolerance)
{
   const uint8_t *actual = pixel_at(pixels, x, y);
   for (int channel = 0; channel < 4; ++channel) {
      if (!close_byte(actual[channel], expected[channel], tolerance)) {
         fprintf(stderr,
                 "%s pixel (%d,%d) mismatch: got %u,%u,%u,%u expected "
                 "%u,%u,%u,%u tolerance %d\n",
                 name, x, y, actual[0], actual[1], actual[2], actual[3],
                 expected[0], expected[1], expected[2], expected[3],
                 tolerance);
         exit(1);
      }
   }
}

static void
check_constant_pixels(const char *name, const uint8_t *pixels,
                      const uint8_t expected[4])
{
   static const struct sample samples[] = {
      {8, 8}, {16, 48}, {32, 32}, {48, 16}, {55, 40},
   };

   for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
      check_pixel(name, pixels, samples[i].x, samples[i].y, expected, 1);
}

static uint8_t
unorm8(float value)
{
   return (uint8_t)(value * 255.0f + 0.5f);
}

static void
check_varying_pixels(const uint8_t *pixels)
{
   static const struct sample samples[] = {
      {8, 8}, {24, 16}, {32, 32}, {48, 8},
   };

   for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
      float green = ((float)samples[i].x + 0.5f) / (2.0f * WIDTH);
      float blue = ((float)samples[i].y + 0.5f) / (2.0f * HEIGHT);
      uint8_t expected[4] = {
         unorm8(1.0f - green - blue),
         unorm8(green),
         unorm8(blue),
         255,
      };
      check_pixel("varying", pixels, samples[i].x, samples[i].y, expected, 2);
   }

   const uint8_t *center = pixel_at(pixels, 32, 32);
   if ((center[0] == 0 && center[1] == 0 && center[2] == 0) ||
       center[3] == 0) {
      fprintf(stderr, "varying center pixel is clear or zero\n");
      exit(1);
   }
}

static void
print_samples(const uint8_t *pixels)
{
   static const struct sample samples[] = {
      {0, 0}, {8, 8}, {24, 16}, {32, 32}, {48, 8}, {63, 63},
   };

   for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
      const uint8_t *pixel = pixel_at(pixels, samples[i].x, samples[i].y);
      printf("pixel[%d,%d]=%u,%u,%u,%u\n", samples[i].x, samples[i].y,
             pixel[0], pixel[1], pixel[2], pixel[3]);
   }
}

static enum probe_case
parse_case(const char *name)
{
   if (strcmp(name, "clear") == 0)
      return PROBE_CLEAR;
   if (strcmp(name, "constant") == 0)
      return PROBE_CONSTANT;
   if (strcmp(name, "varying") == 0)
      return PROBE_VARYING;

   fprintf(stderr, "unknown case: %s\n", name);
   exit(2);
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s clear|constant|varying\n", argv[0]);
      return 2;
   }

   enum probe_case probe = parse_case(argv[1]);
   const char *platform;
   EGLDisplay display = get_display(&platform);
   if (display == EGL_NO_DISPLAY)
      fail_egl("eglGetDisplay");

   EGLint major, minor;
   check_egl("eglInitialize", eglInitialize(display, &major, &minor));
   check_egl("eglBindAPI", eglBindAPI(EGL_OPENGL_ES_API));
   EGLConfig config = choose_config(display);

   static const EGLint pbuffer_attributes[] = {
      EGL_WIDTH, WIDTH,
      EGL_HEIGHT, HEIGHT,
      EGL_NONE,
   };
   EGLSurface surface =
      eglCreatePbufferSurface(display, config, pbuffer_attributes);
   if (surface == EGL_NO_SURFACE)
      fail_egl("eglCreatePbufferSurface");

   static const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };
   EGLContext context =
      eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
   if (context == EGL_NO_CONTEXT)
      fail_egl("eglCreateContext");
   check_egl("eglMakeCurrent",
             eglMakeCurrent(display, surface, surface, context));

   const char *egl_vendor = eglQueryString(display, EGL_VENDOR);
   const char *renderer = (const char *)glGetString(GL_RENDERER);
   const char *version = (const char *)glGetString(GL_VERSION);
   if (!egl_vendor)
      fail_egl("eglQueryString(EGL_VENDOR)");
   if (!renderer || !version) {
      fprintf(stderr, "GL renderer or version string is unavailable\n");
      return 1;
   }
   check_gl("initialization");

   glViewport(0, 0, WIDTH, HEIGHT);
   glDisable(GL_DITHER);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   check_gl("clear");

   GLuint program = 0;
   if (probe != PROBE_CLEAR) {
      static const GLfloat positions[] = {
         -1.0f, -1.0f,
          3.0f, -1.0f,
         -1.0f,  3.0f,
      };
      static const GLfloat varying_vertices[] = {
         -1.0f, -1.0f, 1.0f, 0.0f, 0.0f,
          3.0f, -1.0f, 0.0f, 1.0f, 0.0f,
         -1.0f,  3.0f, 0.0f, 0.0f, 1.0f,
      };

      if (probe == PROBE_CONSTANT) {
         program = create_program(position_vertex_shader,
                                  constant_fragment_shader, 0);
         glUseProgram(program);
         glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
      } else {
         program = create_program(varying_vertex_shader,
                                  varying_fragment_shader, 1);
         glUseProgram(program);
         glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                               5 * sizeof(GLfloat), varying_vertices);
         glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                               5 * sizeof(GLfloat), varying_vertices + 2);
         glEnableVertexAttribArray(1);
      }
      glEnableVertexAttribArray(0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      check_gl("draw");
   }

   glFinish();
   check_gl("glFinish");

   uint8_t pixels[WIDTH * HEIGHT * 4];
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   check_gl("glReadPixels");

   static const uint8_t clear_color[4] = {0, 0, 0, 255};
   static const uint8_t constant_color[4] = {0, 255, 0, 255};
   if (probe == PROBE_CLEAR)
      check_constant_pixels("clear", pixels, clear_color);
   else if (probe == PROBE_CONSTANT)
      check_constant_pixels("constant", pixels, constant_color);
   else
      check_varying_pixels(pixels);
   check_gl("validation");

   printf("case=%s egl_platform=%s egl_vendor=%s renderer=%s version=%s "
          "fnv1a=%016" PRIx64 "\n",
          argv[1], platform, egl_vendor, renderer, version,
          fnv1a(pixels, sizeof(pixels)));
   print_samples(pixels);

   if (program)
      glDeleteProgram(program);
   check_gl("cleanup");
   check_egl("eglMakeCurrent(release)",
             eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                            EGL_NO_CONTEXT));
   check_egl("eglDestroyContext", eglDestroyContext(display, context));
   check_egl("eglDestroySurface", eglDestroySurface(display, surface));
   check_egl("eglTerminate", eglTerminate(display));
   return 0;
}
