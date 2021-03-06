// Copyright (C) 2009, Chris Double. All Rights Reserved.
// See the license at the end of this file.
#include <cassert>
#include <cmath>
#include <map>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <ogg/ogg.h>
#include <theora/theora.h>
#include <theora/theoradec.h>
#include <vorbis/codec.h>
#include <SDL/SDL.h>
#include <SDL/SDL_syswm.h>
// For OpenGL support.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
// Timing.
#include <sys/time.h>
#include <float.h>
// Texture streaming.
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#define LINUX 1
#include "bc_cat.h"
#include <GLES2/gl2ext.h>

#define UYVY_CPU_BUF 0
#define CMEM 0
#define TEST_GETPHYS 0
#define SOUND 0

#if CMEM
#include "cmem.h"
#endif

PFNGLTEXBINDSTREAMIMGPROC glTexBindStreamIMG;
PFNGLGETTEXSTREAMDEVICENAMEIMGPROC glGetTexStreamDeviceNameIMG;
PFNGLGETTEXSTREAMDEVICEATTRIBUTEIVIMGPROC glGetTexStreamDeviceAttributeivIMG;

extern "C" {
#include <sydney_audio.h>
}

using namespace std;

enum StreamType {
  TYPE_VORBIS,
  TYPE_THEORA,
  TYPE_UNKNOWN
};

class OggStream;

class TheoraDecode {
public:
  th_info mInfo;
  th_comment mComment;
  th_setup_info* mSetup;
  th_dec_ctx* mCtx;

public:
  TheoraDecode() :
    mSetup(0),
    mCtx(0)
  {
    th_info_init(&mInfo);
    th_comment_init(&mComment);
  }

  void initForData(OggStream* stream);

  ~TheoraDecode() {
    if (mSetup)
      th_setup_free(mSetup);
    if (mCtx)
      th_decode_free(mCtx);
  }   
};

class VorbisDecode {
public:
  vorbis_info mInfo;
  vorbis_comment mComment;
  vorbis_dsp_state mDsp;
  vorbis_block mBlock;

public:
  VorbisDecode()
  {
    vorbis_info_init(&mInfo);
    vorbis_comment_init(&mComment);    
  }

  void initForData(OggStream* stream);
};

class OggStream
{
public:
  int mSerial;
  ogg_stream_state mState;
  StreamType mType;
  bool mActive;
  TheoraDecode mTheora;
  VorbisDecode mVorbis;

public:
  OggStream(int serial = -1) : 
    mSerial(serial),
    mType(TYPE_UNKNOWN),
    mActive(true)
  { 
  }

  ~OggStream() {
    int ret = ogg_stream_clear(&mState);
    assert(ret == 0);
  }
};

void TheoraDecode::initForData(OggStream* stream) {
  stream->mTheora.mCtx = 
    th_decode_alloc(&stream->mTheora.mInfo, 
		    stream->mTheora.mSetup);
  assert(stream->mTheora.mCtx != NULL);
  int ppmax = 0;
  int ret = th_decode_ctl(stream->mTheora.mCtx,
			  TH_DECCTL_GET_PPLEVEL_MAX,
			  &ppmax,
			  sizeof(ppmax));
  assert(ret == 0);

  // Set to a value between 0 and ppmax inclusive to experiment with
  // this parameter.
  ppmax = 0;
  ret = th_decode_ctl(stream->mTheora.mCtx,
		      TH_DECCTL_SET_PPLEVEL,
		      &ppmax,
		      sizeof(ppmax));
  assert(ret == 0);
}

void VorbisDecode::initForData(OggStream* stream) {
  int ret = vorbis_synthesis_init(&stream->mVorbis.mDsp, &stream->mVorbis.mInfo);
  assert(ret == 0);
  ret = vorbis_block_init(&stream->mVorbis.mDsp, &stream->mVorbis.mBlock);
  assert(ret == 0);
}

typedef map<int, OggStream*> StreamMap; 

void make_hildon_leave_us_alone(Display * dpy, Window win)
{
  Atom atom = XInternAtom(dpy, "_HILDON_NON_COMPOSITED_WINDOW", False);
  assert(atom);
  long atomval = 1;
  XChangeProperty(dpy, win, atom, XA_INTEGER, 32, PropModeReplace,
                 (unsigned char*) &atomval, 1);
}

struct DisplaySink
{
  virtual void Show(th_dec_ctx* dec, th_ycbcr_buffer const& buffer) = 0;
};

class SDL_DisplaySink : public DisplaySink
{
public:
  SDL_DisplaySink() : mSurface(0), mOverlay(0) {
    int r = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE);
    assert(r == 0);
  }


  void Show(th_dec_ctx* dec, th_ycbcr_buffer const& buffer) {
    if (!mSurface) {
      mSurface = SDL_SetVideoMode(buffer[0].width,
				  buffer[0].height,
				  16,
				  SDL_SWSURFACE | SDL_FULLSCREEN);
      assert(mSurface);

      mOverlay = SDL_CreateYUVOverlay(buffer[0].width,
				      buffer[0].height,
				      SDL_YV12_OVERLAY,
				      mSurface);
      assert(mOverlay);

      SDL_SysWMinfo winfo;
      SDL_VERSION(&winfo.version);
      int r = SDL_GetWMInfo(&winfo);
      assert(r == 1);

      Display* dpys[2];
      dpys[0] = winfo.info.x11.display;
      dpys[1] = winfo.info.x11.gfxdisplay;

      Window wins[3];
      wins[0] = winfo.info.x11.window;
      wins[1] = winfo.info.x11.fswindow;
      wins[2] = winfo.info.x11.wmwindow;

      for (int i = 0; i < 2; ++i) {
	for (int j = 0; j < 3; ++j) {
	  make_hildon_leave_us_alone(dpys[i], wins[j]);
	}
      }
    }

    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = buffer[0].width;
    rect.h = buffer[0].height;
    SDL_LockYUVOverlay(mOverlay);

    for (int i = 0; i < buffer[0].height; ++i)
      memcpy(mOverlay->pixels[0]+(mOverlay->pitches[0]*i), 
	     buffer[0].data+(buffer[0].stride*i), 
	     mOverlay->pitches[0]);
    
    for (int i = 0; i < buffer[2].height; ++i)
      memcpy(mOverlay->pixels[2]+(mOverlay->pitches[2]*i), 
	     buffer[1].data+(buffer[1].stride*i), 
	     mOverlay->pitches[2]);
    
    for (int i = 0; i < buffer[1].height; ++i)
      memcpy(mOverlay->pixels[1]+(mOverlay->pitches[1]*i), 
	     buffer[2].data+(buffer[2].stride*i), 
	     mOverlay->pitches[1]);

    SDL_UnlockYUVOverlay(mOverlay);
    SDL_DisplayYUVOverlay(mOverlay, &rect);
  }

  virtual ~SDL_DisplaySink() {
    if (mSurface) {
      SDL_FreeYUVOverlay(mOverlay);
      SDL_FreeSurface(mSurface);
    }
    SDL_Quit();
  }

private:
  SDL_Surface* mSurface;
  SDL_Overlay* mOverlay;
};

class GL_DisplaySink : public DisplaySink
{
public:
  GL_DisplaySink(int glMode) : mDisplay(0), mContext(0), mSurface(0), mMode(glMode), mMappedTexture(0) {
    assert(mMode >= 0 && mMode <= 5);
  }

  void Show(th_dec_ctx* dec, th_ycbcr_buffer const& buffer) {
    if (!mDisplay) {
      init_x11(buffer[0].width, buffer[0].height, true);
      init_gles();
      if (mMode == 3) {
	setup_bcbufs(NULL, buffer[0].width, buffer[0].height);
      } else if (mMode == 4) {
	setup_bcbufs(dec, buffer[0].width, buffer[0].height);
      }
    }

    struct timeval start, end, dt;
    gettimeofday(&start, NULL);
    if (mMode == 3) {
      unsigned char* p = (unsigned char*) mMappedTexture;
      for (unsigned int i = 0; i < buffer[0].height; ++i) {
	unsigned char* y = buffer[0].data + (i * buffer[0].stride);
	unsigned char* u = buffer[1].data + (i / 2 * buffer[1].stride);
	unsigned char* v = buffer[2].data + (i / 2 * buffer[2].stride);
	for (unsigned int j = 0; j < buffer[0].width; j += 2) {
	  p[0] = u[j / 2];
	  p[1] = y[j];
	  p[2] = v[j / 2];
	  p[3] = y[j + 1];
	  p += 4;
	}
      }
    } else if (mMode == 4) {
#if UYVY_CPU_BUF
      memcpy(mMappedTexture, mUYVYBuffer, buffer[0].height * buffer[0].width * 2);
#endif
    } else {
      glActiveTexture(GL_TEXTURE0);
      bind_texture(mTextures[0], buffer[0].width, buffer[0].height,
		   buffer[0].stride, buffer[0].data);

      if (mMode != 2) {
	glActiveTexture(GL_TEXTURE1);
	bind_texture(mTextures[1], buffer[1].width, buffer[1].height,
		     buffer[1].stride, buffer[1].data);

	glActiveTexture(GL_TEXTURE2);
	bind_texture(mTextures[2], buffer[2].width, buffer[2].height,
		     buffer[2].stride, buffer[2].data);
      }
    }
    gettimeofday(&end, NULL);
    timersub(&end, &start, &dt);
    //printf("%f\t", dt.tv_sec * 1000.0 + dt.tv_usec / 1000.0);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    GLfloat const identity[] = {
      1.0, 0.0, 0.0, 0.0,
      0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, 0.0, 0.0, 1.0
    };
    glUniformMatrix4fv(mLocation, 1, GL_FALSE, identity);

    glEnableVertexAttribArray(0);
    GLfloat const vertices[] = {
      -1.0, 1.0, 0.0,
      -1.0, -1.0, 0.0,
      1.0, 1.0, 0.0,
      1.0, -1.0, 0.0
    };
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vertices);

    glEnableVertexAttribArray(1);
    GLfloat const coords[] = {
      0.0, 0.0,
      0.0, 1.0,
      1.0, 0.0,
      1.0, 1.0
    };
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, coords);

    gettimeofday(&start, NULL);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gettimeofday(&end, NULL);
    timersub(&end, &start, &dt);
    //printf("%f\n", dt.tv_sec * 1000.0 + dt.tv_usec / 1000.0);

    eglSwapBuffers(mDisplay, mSurface);
  }

  virtual ~GL_DisplaySink() {
    if (mDisplay) {
      glDeleteProgram(mProgram);
      glDeleteShader(mVertexShader);
      glDeleteShader(mFragmentShader);

      eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroySurface(mDisplay, mSurface);
      eglDestroyContext(mDisplay, mContext);
      eglTerminate(mDisplay);
#if UYVY_CPU_BUF
      delete []mUYVYBuffer;
#endif
    }
  }
private:
  void init_x11(unsigned int w, unsigned int h, bool fullscreen) {
    /*
     * Create a Window in 100 easy steps.
     */
    Display* xdpy = XOpenDisplay(NULL);
    assert(xdpy);

    int screen = XDefaultScreen(xdpy);
    Window root = RootWindow(xdpy, screen);
    int depth = DefaultDepth(xdpy, screen);

    XVisualInfo visual;
    XMatchVisualInfo(xdpy, screen, depth, TrueColor, &visual);

    Colormap cmap = XCreateColormap(xdpy, root, visual.visual, AllocNone);

    XSetWindowAttributes xswa;
    memset(&xswa, 0, sizeof(xswa));
    xswa.colormap = cmap;
    xswa.event_mask = StructureNotifyMask | ExposureMask | ButtonPressMask |
      ButtonReleaseMask | KeyPressMask | KeyReleaseMask;

    unsigned int mask = CWEventMask | CWColormap;

    Window xwin = XCreateWindow(xdpy, root, 0, 0,
				w, h, 0,
				CopyFromParent, InputOutput,
				CopyFromParent, mask, &xswa);
    assert(xwin);

    if (fullscreen) {
      Atom xatom = XInternAtom(xdpy, "_NET_WM_STATE", False);
      Atom xstate = XInternAtom(xdpy, "_NET_WM_STATE_FULLSCREEN", False);
      assert(xatom && xstate);
      XChangeProperty(xdpy, xwin, xatom, XA_ATOM, 32, PropModeReplace,
		      (unsigned char*) &xstate, 1);

      make_hildon_leave_us_alone(xdpy, xwin);
    }

    XMapWindow(xdpy, xwin);
    XFlush(xdpy);

    init_egl(xdpy, xwin);
  }

  void init_egl(Display* display, Window window) {
    /*
     * Initialize OpenGL ES 2.0 via EGL.
     */
    EGLDisplay dpy = eglGetDisplay((NativeDisplayType) display);

    if (eglInitialize(dpy, NULL, NULL) != EGL_TRUE)
      assert(false);

    EGLint attrs[] = {
      EGL_BUFFER_SIZE, 16,
      EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
      EGL_DEPTH_SIZE, 16,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_NONE
    };
    EGLConfig cfg;
    EGLint n_cfgs;
    if (eglChooseConfig(dpy, attrs, &cfg, 1, &n_cfgs) != EGL_TRUE)
      assert(false);

    EGLSurface srf = eglCreateWindowSurface(dpy, cfg,
					    (NativeWindowType) window, NULL);
    if (srf == EGL_NO_SURFACE)
      assert(false);

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint ctxattrs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattrs);
    if (ctx == EGL_NO_CONTEXT)
      assert(false);

    if (eglMakeCurrent(dpy, srf, srf, ctx) != EGL_TRUE)
      assert(false);

    mDisplay = dpy;
    mContext = ctx;
    mSurface = srf;
  }

  void init_gles() {
    GLint maxUnits;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits);
    assert(maxUnits >= 3);

    GLubyte const* glExts = glGetString(GL_EXTENSIONS);
    assert(strstr((char const*) glExts, "GL_IMG_texture_npot"));

    printf("vendor: %s\n", glGetString(GL_VENDOR));
    printf("renderer: %s\n", glGetString(GL_RENDERER));
    printf("version: %s\n", glGetString(GL_VERSION));
    printf("shading language version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    printf("extensions: %s\n", glGetString(GL_EXTENSIONS));

    char const* vshader =
      "attribute vec4 myVertex;\n"
      "attribute vec4 myUV;\n"
      "uniform mat4 myPMVMatrix;\n"
      "varying vec2 myTexCoord;\n"
      "void main()\n"
      "{\n"
      "gl_Position = myPMVMatrix * myVertex;\n"
      "myTexCoord = myUV.st;\n"
      "}\n";
    mVertexShader = compile_shader(GL_VERTEX_SHADER, vshader);

    char const* fshaders[] = {
      // Obvious colour conversion implementation.
      "uniform sampler2D ytx, utx, vtx;\n"
      "varying mediump vec2 myTexCoord;\n"
      "void main()\n"
      "{\n"
      "lowp float y = (texture2D(ytx, myTexCoord).r - 0.0625) * 1.1643;\n"
      "lowp float u = texture2D(utx, myTexCoord).r - 0.5;\n"
      "lowp float v = texture2D(vtx, myTexCoord).r - 0.5;\n"
      "lowp float r = y + 1.5958 * v;\n"
      "lowp float g = y - 0.39173 * u - 0.8129 * v;\n"
      "lowp float b = y + 2.017 * u;\n"
      "gl_FragColor = vec4(r, g, b, 1.0);\n"
      "}\n",

      // Slightly optimized routine.
      "uniform sampler2D ytx, utx, vtx;\n"
      "uniform mediump mat3 yuv2rgb;\n"
      "varying mediump vec2 myTexCoord;\n"
      "void main()\n"
      "{\n"
      "lowp vec3 yuv = vec3(texture2D(ytx, myTexCoord).x, texture2D(utx, myTexCoord).x, texture2D(vtx, myTexCoord).x);\n"
      "yuv -= vec3(0.0, 0.5, 0.5);\n"
      "gl_FragColor = vec4(yuv2rgb * yuv, 1.0);\n"
      "}\n",

      // Greyscale "fast" routine.
      "uniform sampler2D ytx;\n"
      "varying mediump vec2 myTexCoord;\n"
      "void main()\n"
      "{\n"
      "gl_FragColor = texture2D(ytx, myTexCoord);\n"
      "}\n",

      // Texture streaming.
      "#extension GL_IMG_texture_stream2 : enable\n"
      "uniform samplerStreamIMG tx;\n"
      "varying mediump vec2 myTexCoord;\n"
      "void main()\n"
      "{\n"
      "gl_FragColor = textureStreamIMG(tx, myTexCoord);\n"
      "}\n",

      // More optimized routine.
      "precision highp float;\n"
      "uniform sampler2D ytx, utx, vtx;\n"
      "uniform mat3 yuv2rgb;\n"
      "varying vec2 myTexCoord;\n"
      "void main()\n"
      "{\n"
      "vec3 yuv = vec3(texture2D(ytx, myTexCoord).x, texture2D(utx, myTexCoord).x, texture2D(vtx, myTexCoord).x);\n"
      "yuv -= vec3(0.0, 0.5, 0.5);\n"
      "gl_FragColor = vec4(yuv2rgb * yuv, 1.0);\n"
      "}\n"
    };

    int mode = mMode;
    if (mMode == 4) {
      mode = 3;
    }
    if (mMode == 5) {
      mode = 4;
    }
    mFragmentShader = compile_shader(GL_FRAGMENT_SHADER, fshaders[mode]);

    mProgram = glCreateProgram();
    assert(mProgram);
    glAttachShader(mProgram, mVertexShader);
    glAttachShader(mProgram, mFragmentShader);
    glBindAttribLocation(mProgram, 0, "myVertex");
    glBindAttribLocation(mProgram, 1, "myUV");
    glLinkProgram(mProgram);
    GLint linked;
    glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
    if (!linked) {
      char buf[4096];
      glGetProgramInfoLog(mProgram, sizeof(buf), NULL, buf);
      printf("link error: %s\n", buf);
    }
    assert(linked);

    glUseProgram(mProgram);

    mLocation = glGetUniformLocation(mProgram, "myPMVMatrix");

    if (mMode == 3 || mMode == 4) {
      glUniform1i(glGetUniformLocation(mProgram, "tx"), 0);
    } else {
      if (mMode == 1 || mMode == 5) {
	static GLfloat const yuv2rgb[] = {
	  1.0, 1.0, 1.0,
	  0.0, -0.34414, 1.772,
	  1.402, -0.71414, 0.0
	};
	glUniformMatrix3fv(glGetUniformLocation(mProgram, "yuv2rgb"), 1, GL_FALSE, yuv2rgb);
      }

      glUniform1i(glGetUniformLocation(mProgram, "ytx"), 0);
      glGenTextures(1, &mTextures[0]);
      assert(mTextures[0]);

      if (mMode == 0 || mMode == 1 || mMode == 5) {
	glUniform1i(glGetUniformLocation(mProgram, "utx"), 1);
	glUniform1i(glGetUniformLocation(mProgram, "vtx"), 2);
	glGenTextures(2, &mTextures[1]);
	assert(mTextures[1] && mTextures[2]);
      }
    }
  }

  GLuint compile_shader(GLenum type, char const* src) {
    GLuint shader = glCreateShader(type);
    assert(shader);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
      char buf[4096];
      glGetShaderInfoLog(shader, sizeof(buf), NULL, buf);
      printf("compile error: %s\n", buf);
    }
    assert(compiled);
    return shader;
  }

  void bind_texture(GLuint texID, unsigned int w, unsigned int h, unsigned int stride,
		    unsigned char const* data) {
    glBindTexture(GL_TEXTURE_2D, texID);

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // This would be nice to use if it existed in GLES.
    //    glPixelStorei(GL_UNPACK_ROW_LENGTH, buffer[1].stride);

    // We could use glPixelStorei(GL_UNPACK_ALIGNMENT), but it seems
    // like most Theora frames have more than 16-32 bytes of extra
    // stride.

    // But it doesn't, so we have to upload row-by-row, accounting for
    // the stride ourselves.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
		 GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    for (unsigned int y = 0; y < h; ++y) {
      unsigned char const* row = data + (y * stride);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1,
		      GL_LUMINANCE, GL_UNSIGNED_BYTE, row);
    }
  }

  void setup_bcbufs(th_dec_ctx *dec, unsigned int w, unsigned int h) {
    GLubyte const* glExts = glGetString(GL_EXTENSIONS);
    assert(strstr((char const*) glExts, "GL_IMG_texture_stream"));

    glTexBindStreamIMG = 
      (PFNGLTEXBINDSTREAMIMGPROC) eglGetProcAddress("glTexBindStreamIMG");
    glGetTexStreamDeviceNameIMG =
      (PFNGLGETTEXSTREAMDEVICENAMEIMGPROC) eglGetProcAddress("glGetTexStreamDeviceNameIMG");
    glGetTexStreamDeviceAttributeivIMG =
      (PFNGLGETTEXSTREAMDEVICEATTRIBUTEIVIMGPROC) eglGetProcAddress("glGetTexStreamDeviceAttributeivIMG");
    assert(glTexBindStreamIMG && glGetTexStreamDeviceNameIMG && glGetTexStreamDeviceAttributeivIMG);

    // XXXkinetik what's the point of ndelay?
    int fd = open("/dev/bc_cat", O_RDWR | O_NDELAY);
    assert(fd >= 0);

    bc_buf_params_t param;
    param.count = 1;
    assert(w % 32 == 0);
    param.width = w;
    param.height = h;
    // XXXkinetik example uses UYVY (packed), we want YV12(?) which is
    // planar packed is single buf per frame, how would planar look?
    // doesn't matter - driver doesn't support it anyway
    param.pixel_fmt = PVRSRV_PIXEL_FORMAT_FOURCC_ORG_UYVY;
#if !CMEM
    param.type = BC_MEMORY_MMAP;
#else
    param.type = BC_MEMORY_USERPTR;
#endif
    int r = ioctl(fd, BCIOREQ_BUFFERS, &param);
    assert(r >= 0);

    printf("BC setup: %d bufs of %dx%d (stride %d, size %d)\n",
	   param.count, param.width, param.height, param.stride, param.size);

#if !CMEM
    BCIO_package buf_param;
    buf_param.input = 0;
    r = ioctl(fd, BCIOGET_BUFFERPHYADDR, &buf_param);
    assert(r >= 0);

    size_t size = w * h * 2;
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf_param.output);
    assert(p != MAP_FAILED);
    mMappedTexture = p;

#if TEST_GETPHYS
    r = CMEM_init();
    void *x = (void*)CMEM_getPhys(p);
    fprintf(stderr, "%p %p %p\n", p, x, buf_param.output);
#endif

    if (mMode == 4) {
#if UYVY_CPU_BUF
      mUYVYBuffer = new unsigned char[size];
      thdsp_decode_set_uyvy_buffer(dec, mUYVYBuffer, w * 2, size);
#else
      thdsp_decode_set_uyvy_buffer(dec, (unsigned char*)mMappedTexture, w * 2, size);
#endif
    }

#else
    fprintf(stderr, "b4 init\n"); sleep(1);
    r = CMEM_init();
    assert(r >= 0);
    fprintf(stderr, "af init %d\n", r); sleep(1);

    CMEM_AllocParams cmem_params;
    cmem_params.type = CMEM_HEAP;
    cmem_params.flags = CMEM_CACHED;
    cmem_params.alignment = 32;
    void* p = CMEM_alloc(param.size, &cmem_params);
    assert(p);

    fprintf(stderr, "af alloc\n"); sleep(1);

    bc_buf_ptr_t phys_buf;
    phys_buf.index = 0;
    phys_buf.size = param.size;
    phys_buf.pa = CMEM_getPhys(p);
    r = ioctl(fd, BCIOSET_BUFFERPHYADDR, &phys_buf);
    assert(r >= 0);

    fprintf(stderr, "af setbuf\n"); sleep(1);

    mMappedTexture = p;
#endif

    GLubyte const* devname = glGetTexStreamDeviceNameIMG(0);
    printf("BC GL: devname=%s", devname);

    GLint nbufs;
    glGetTexStreamDeviceAttributeivIMG(0, GL_TEXTURE_STREAM_DEVICE_NUM_BUFFERS_IMG, &nbufs);
    GLint width;
    glGetTexStreamDeviceAttributeivIMG(0, GL_TEXTURE_STREAM_DEVICE_WIDTH_IMG, &width);
    GLint height;
    glGetTexStreamDeviceAttributeivIMG(0, GL_TEXTURE_STREAM_DEVICE_HEIGHT_IMG, &height);
    GLint fmt;
    glGetTexStreamDeviceAttributeivIMG(0, GL_TEXTURE_STREAM_DEVICE_FORMAT_IMG, &fmt);
    printf("BC GL: nbufs %d, %dx%d, fmt=%d\n", nbufs, width, height, fmt);

    glTexBindStreamIMG(0, 0);
  }

  EGLDisplay mDisplay;
  EGLContext mContext;
  EGLSurface mSurface;

  int mMode;

  GLuint mVertexShader;
  GLuint mFragmentShader;
  GLuint mProgram;
  GLuint mLocation;
  GLuint mTextures[3];
  void* mMappedTexture;
#if UYVY_CPU_BUF
  unsigned char *mUYVYBuffer;
#endif
};

class Null_DisplaySink : public DisplaySink
{
public:
  Null_DisplaySink() {
  }


  void Show(th_dec_ctx* dec, th_ycbcr_buffer const& buffer) {
  }

  virtual ~Null_DisplaySink() {
  }
};

class OggDecoder
{
public:
  StreamMap mStreams; 
  DisplaySink* mDisplaySink;
  sa_stream_t* mAudio;
  ogg_int64_t  mGranulepos;

private:
  bool mVideoFrameQueued;
  th_ycbcr_buffer mVideoBuffer;
  struct timeval mTimeStamp[100];
  unsigned int mTimeStampPos;

private:
  bool handle_theora_header(OggStream* stream, ogg_packet* packet);
  bool handle_vorbis_header(OggStream* stream, ogg_packet* packet);
  void read_headers(istream& stream, ogg_sync_state* state);

  bool read_page(istream& stream, ogg_sync_state* state, ogg_page* page);
  bool read_packet(istream& is, ogg_sync_state* state, OggStream* stream, ogg_packet* packet);
  void handle_theora_data(OggStream* stream, ogg_packet* packet);
  void handle_theora_out();

public:
  OggDecoder(DisplaySink* display_sink) :
    mDisplaySink(display_sink),
    mAudio(0),
    mGranulepos(0),
    mVideoFrameQueued(false),
    mTimeStampPos(0)
  {
    memset(mVideoBuffer,0,sizeof(mVideoBuffer));
  }

  ~OggDecoder() {
    if (mAudio) {
      sa_stream_drain(mAudio);
      sa_stream_destroy(mAudio);
    }

    for(StreamMap::iterator it = mStreams.begin(); it != mStreams.end(); ++it) {
      OggStream* stream = (*it).second;
      delete stream;
    }

    delete mDisplaySink;
  }
  void play(istream& stream);
};

bool OggDecoder::read_page(istream& stream, ogg_sync_state* state, ogg_page* page) {
  int ret = 0;

  // If we've hit end of file we still need to continue processing
  // any remaining pages that we've got buffered.
  if (!stream.good())
    return ogg_sync_pageout(state, page) == 1;

  while((ret = ogg_sync_pageout(state, page)) != 1) {
    // Returns a buffer that can be written too
    // with the given size. This buffer is stored
    // in the ogg synchronisation structure.
    char* buffer = ogg_sync_buffer(state, 4096);
    assert(buffer);

    // Read from the file into the buffer
    stream.read(buffer, 4096);
    int bytes = stream.gcount();
    if (bytes == 0) {
      // End of file. 
      continue;
    }

    // Update the synchronisation layer with the number
    // of bytes written to the buffer
    ret = ogg_sync_wrote(state, bytes);
    assert(ret == 0);
  }
  return true;
}

bool OggDecoder::read_packet(istream& is, ogg_sync_state* state, OggStream* stream, ogg_packet* packet) {
  int ret = 0;

  while ((ret = ogg_stream_packetout(&stream->mState, packet)) != 1) {
    ogg_page page;
    if (!read_page(is, state, &page))
      return false;

    int serial = ogg_page_serialno(&page);
    assert(mStreams.find(serial) != mStreams.end());
    OggStream* pageStream = mStreams[serial];
    
    // Drop data for streams we're not interested in.
    if (stream->mActive) {
      ret = ogg_stream_pagein(&pageStream->mState, &page);
      assert(ret == 0);
    }
  }
  return true;
}

void OggDecoder::read_headers(istream& stream, ogg_sync_state* state) {
  ogg_page page;

  bool headersDone = false;
  while (!headersDone && read_page(stream, state, &page)) {
    int ret = 0;
    int serial = ogg_page_serialno(&page);
    OggStream* stream = 0;

    if(ogg_page_bos(&page)) {
      // At the beginning of the stream, read headers
      // Initialize the stream, giving it the serial
      // number of the stream for this page.
      stream = new OggStream(serial);
      ret = ogg_stream_init(&stream->mState, serial);
      assert(ret == 0);
      mStreams[serial] = stream;
    }

    assert(mStreams.find(serial) != mStreams.end());
    stream = mStreams[serial];

    // Add a complete page to the bitstream
    ret = ogg_stream_pagein(&stream->mState, &page);
    assert(ret == 0);

    // Process all available header packets in the stream. When we hit
    // the first data stream we don't decode it, instead we
    // return. The caller can then choose to process whatever data
    // streams it wants to deal with.
    ogg_packet packet;
    while (!headersDone &&
	   (ret = ogg_stream_packetpeek(&stream->mState, &packet)) != 0) {
      assert(ret == 1);

      // A packet is available. If it is not a header packet we exit.
      // If it is a header packet, process it as normal.
      headersDone = headersDone || handle_theora_header(stream, &packet);
      headersDone = headersDone || handle_vorbis_header(stream, &packet);
      if (!headersDone) {
	// Consume the packet
	ret = ogg_stream_packetout(&stream->mState, &packet);
	assert(ret == 1);
      }
    }
  } 
}

void OggDecoder::play(istream& is) {
  ogg_sync_state state;

  int ret = ogg_sync_init(&state);
  assert(ret == 0);

  // Read headers for all streams
  read_headers(is, &state);

  // Find and initialize the first theora and vorbis
  // streams. According to the Theora spec these can be considered the
  // 'primary' streams for playback.
  OggStream* video = 0;
  OggStream* audio = 0;
  for(StreamMap::iterator it = mStreams.begin(); it != mStreams.end(); ++it) {
    OggStream* stream = (*it).second;
    if (!video && stream->mType == TYPE_THEORA) {
      video = stream;
      video->mTheora.initForData(video);
    }
    else if (!audio && stream->mType == TYPE_VORBIS) {
      audio = stream;
      audio->mVorbis.initForData(audio);
    }
    else
      stream->mActive = false;
  }

  assert(audio);

  if (video) {
    cout << "Video stream is " 
	 << video->mSerial << " "
	 << video->mTheora.mInfo.frame_width << "x" << video->mTheora.mInfo.frame_height
	 << endl;
  }

  cout << "Audio stream is " 
       << audio->mSerial << " "
       << audio->mVorbis.mInfo.channels << " channels "
       << audio->mVorbis.mInfo.rate << "KHz"
       << endl;

  ret = sa_stream_create_pcm(&mAudio,
			     NULL,
			     SA_MODE_WRONLY,
			     SA_PCM_FORMAT_S16_NE,
			     audio->mVorbis.mInfo.rate,
			     audio->mVorbis.mInfo.channels);
  assert(ret == SA_SUCCESS);
	
  ret = sa_stream_open(mAudio);
  assert(ret == SA_SUCCESS);

  // Read audio packets, sending audio data to the sound hardware.
  // When it's time to display a frame, decode the frame and display it.
  ogg_packet packet;
  while (read_packet(is, &state, audio, &packet)) {
    if (vorbis_synthesis(&audio->mVorbis.mBlock, &packet) == 0) {
      ret = vorbis_synthesis_blockin(&audio->mVorbis.mDsp, &audio->mVorbis.mBlock);
      assert(ret == 0);
    }

    float** pcm = 0;
    int samples = 0;
    while ((samples = vorbis_synthesis_pcmout(&audio->mVorbis.mDsp, &pcm)) > 0) {
      if (mAudio) {
	if (samples > 0) {
          size_t size = samples * audio->mVorbis.mInfo.channels;
          short* buffer = new short[size];
	  short* p = buffer;
	  for (int i=0;i < samples; ++i) {
	    for(int j=0; j < audio->mVorbis.mInfo.channels; ++j) {
	      int v = static_cast<int>(floorf(0.5 + pcm[j][i]*32767.0));
	      if (v > 32767) v = 32767;
	      if (v <-32768) v = -32768;
	      *p++ = v;
	    }
	  }

#if SOUND	  
          ret = sa_stream_write(mAudio, buffer, sizeof(*buffer)*size);
	  assert(ret == SA_SUCCESS);
#endif
          delete[] buffer;
	}
	
	ret = vorbis_synthesis_read(&audio->mVorbis.mDsp, samples);
	assert(ret == 0);
      }

      // At this point we've written some audio data to the sound
      // system. Now we check to see if it's time to display a video
      // frame.
      //
      // The granule position of a video frame represents the time
      // that that frame should be displayed up to. So we get the
      // current time, compare it to the last granule position read.
      // If the time is greater than that it's time to display a new
      // video frame.
      //
      // The time is obtained from the audio system - this represents
      // the time of the audio data that the user is currently
      // listening to. In this way the video frame should be synced up
      // to the audio the user is hearing.
      //
      if (video) {
#if SOUND
	ogg_int64_t position = 0;
        sa_position_t positionType = SA_POSITION_WRITE_SOFTWARE;
#if defined(WIN32)
        positionType = SA_POSITION_WRITE_HARDWARE;
#endif	
        int ret = sa_stream_get_position(mAudio, positionType, &position);
	assert(ret == SA_SUCCESS);
	float audio_time = 
	  float(position) /
	  float(audio->mVorbis.mInfo.rate) /
	  float(audio->mVorbis.mInfo.channels) /
	  sizeof(short);
#endif

	float video_time = th_granule_time(video->mTheora.mCtx, mGranulepos);
#if !SOUND
	float audio_time = video_time + 1;
#endif
	if (audio_time > video_time) {
	  // Decode one frame and display it. If no frame is available we
	  // don't do anything.
	  ogg_packet packet;
	  handle_theora_data(video, 
	   read_packet(is, &state, video, &packet) ? &packet : NULL);
	}
      }
    }
        
#if 0
    // Check for SDL events to exit
    SDL_Event event;
    if (SDL_PollEvent(&event) == 1) {
      if (event.type == SDL_KEYDOWN &&
	  event.key.keysym.sym == SDLK_ESCAPE)
	break;
      if (event.type == SDL_KEYDOWN &&
	  event.key.keysym.sym == SDLK_SPACE)
	SDL_WM_ToggleFullScreen(mSurface);
    } 
#endif
  }

  // Cleanup
  ret = ogg_sync_clear(&state);
  assert(ret == 0);
}

bool OggDecoder::handle_theora_header(OggStream* stream, ogg_packet* packet) {
  int ret = th_decode_headerin(&stream->mTheora.mInfo,
			       &stream->mTheora.mComment,
			       &stream->mTheora.mSetup,
			       packet);
  if (ret == TH_ENOTFORMAT)
    return false; // Not a theora header

  if (ret > 0) {
    // This is a theora header packet
    stream->mType = TYPE_THEORA;
    return false;
  }

  // Any other return value is treated as a fatal error
  assert(ret == 0);

  // This is not a header packet. It is the first 
  // video data packet.
  return true;
}

void OggDecoder::handle_theora_data(OggStream* stream, ogg_packet* packet) {
  th_ycbcr_buffer buffer;
  if (mVideoFrameQueued) {
    // We have a frame. Get the YUV data
    int ret = th_decode_ycbcr_out(stream->mTheora.mCtx, buffer);
    assert(ret == 0);
  }

  if (packet != NULL) {
    // The granulepos for a packet gives the time of the end of the
    // display interval of the frame in the packet.  We keep the
    // granulepos of the frame we've decoded and use this to know the
    // time when to display the next frame.
    int ret = th_decode_packetin(stream->mTheora.mCtx,
                                 packet,
                                 &mGranulepos);
    // If the return code is TH_DUPFRAME then we don't need to
    // get the YUV data and display it since it's the same as
    // the previous frame.
    assert(ret == 0 || ret == TH_DUPFRAME);
  }

  if (mVideoFrameQueued) {
    mDisplaySink->Show(stream->mTheora.mCtx, buffer);

    gettimeofday(&mTimeStamp[mTimeStampPos++], NULL);
    size_t elems = sizeof(mTimeStamp) / sizeof(mTimeStamp[0]);
    if (mTimeStampPos == elems) {
      mTimeStampPos = 0;
      double frameTime  = 0;
      double minTime = DBL_MAX;
      double maxTime = 0;
      for (unsigned i = 1; i < elems; ++i) {
	struct timeval dt;
	timersub(&mTimeStamp[i], &mTimeStamp[i - 1], &dt);
	double time = dt.tv_sec * 1000.0 + dt.tv_usec / 1000.0;
	frameTime += time;
	if (1000.0 / time < minTime) {
	  minTime = 1000.0 / time;
	}
	if (1000.0 / time > maxTime) {
	  maxTime = 1000.0 / time;
	}
      }
      printf("%.1f/%.1f/%.1f FPS\n", minTime, 1000.0 / (frameTime / (elems - 1)), maxTime);
    }
  }
  mVideoFrameQueued = packet != NULL;
}

bool OggDecoder::handle_vorbis_header(OggStream* stream, ogg_packet* packet) {
  int ret = vorbis_synthesis_headerin(&stream->mVorbis.mInfo,
				      &stream->mVorbis.mComment,
				      packet);
  // Unlike libtheora, libvorbis does not provide a return value to
  // indicate that we've finished loading the headers and got the
  // first data packet. To detect this I check if I already know the
  // stream type and if the vorbis_synthesis_headerin call failed.
  if (stream->mType == TYPE_VORBIS && ret == OV_ENOTVORBIS) {
    // First data packet
    return true;
  }
  else if (ret == 0) {
    stream->mType = TYPE_VORBIS;
  }
  return false;
}

void usage() {
  cout << "Usage: plogg [options] filename" << endl;
  cout << "Options:" << endl;
  cout << "  -g <n>   Use OpenGL ES render path." << endl;
  cout << "      0      Use fragment shader v1." << endl;
  cout << "      1      Use fragment shader v2." << endl;
  cout << "      2      Use fast (greyscale) fragment shader." << endl;
  cout << "      3      Use texture streaming." << endl;
  cout << "      4      Use texture streaming with DSP swizzling." << endl;
  cout << "      5      Use fragment shader v3." << endl;
  cout << "  -s       Use SDL render path. (default)" << endl;
  cout << "  -n       Use null render path." << endl;
  exit(1);
}

int main(int argc, char* argv[]) {
  DisplaySink* sink = NULL;
  int ch;

  while ((ch = getopt(argc, argv, "g:snh?")) != -1) {
    switch (ch) {
    case 'g': {
      int glMode = atoi(optarg);
      if (glMode < 0 || glMode > 5) {
	fprintf(stderr, "%s: invalid gl mode %d\n", argv[0], glMode);
	usage();
      }
      sink = new GL_DisplaySink(glMode);
      break;
    }
    case 'n':
      sink = new Null_DisplaySink;
      break;
    case 's':
      sink = new SDL_DisplaySink;
      break;
    case 'h':
    case '?':
    default:
      usage();
    }
  }

  if (!sink) {
    sink = new SDL_DisplaySink;
  }

  ifstream file(argv[optind], ios::in | ios::binary);
  if (!file) {
    fprintf(stderr, "%s: file error\n", argv[0]);
    usage();
  }

  OggDecoder decoder(sink);
  decoder.play(file);
  file.close();

  return 0;
}
// Copyright (C) 2009 Chris Double. All Rights Reserved.
// The original author of this code can be contacted at: chris.double@double.co.nz
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
// FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// DEVELOPERS AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
