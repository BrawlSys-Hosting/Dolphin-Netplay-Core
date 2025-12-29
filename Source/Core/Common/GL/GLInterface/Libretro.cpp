// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLInterface/Libretro.h"

#if defined(_WIN32)
#include <array>
#include <windows.h>
#elif defined(HAVE_X11)
#include <GL/glx.h>
#include <X11/Xlib.h>
#endif

#include "Common/GL/GLExtensions/gl_1_1.h"
#include "Common/Logging/Log.h"

#if defined(_WIN32) && !defined(WGL_ARB_create_context)
#define WGL_ARB_create_context 1
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x00000001
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002
typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
#endif

#if defined(_WIN32) && !defined(WGL_CONTEXT_PROFILE_MASK_ARB)
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif

#if defined(_WIN32) && !defined(WGL_ARB_pbuffer)
#define WGL_ARB_pbuffer 1
DECLARE_HANDLE(HPBUFFERARB);
#define WGL_DRAW_TO_PBUFFER_ARB 0x202D
#define WGL_DEPTH_BITS_ARB 0x2022
#define WGL_STENCIL_BITS_ARB 0x2023
#define WGL_RED_BITS_ARB 0x2015
#define WGL_GREEN_BITS_ARB 0x2017
#define WGL_BLUE_BITS_ARB 0x2019
typedef HPBUFFERARB(WINAPI* PFNWGLCREATEPBUFFERARBPROC)(HDC, int, int, int, const int*);
typedef HDC(WINAPI* PFNWGLGETPBUFFERDCARBPROC)(HPBUFFERARB);
typedef int(WINAPI* PFNWGLRELEASEPBUFFERDCARBPROC)(HPBUFFERARB, HDC);
typedef BOOL(WINAPI* PFNWGLDESTROYPBUFFERARBPROC)(HPBUFFERARB);
#endif

#if defined(_WIN32) && !defined(WGL_ARB_pixel_format)
#define WGL_ARB_pixel_format 1
typedef BOOL(WINAPI* PFNWGLCHOOSEPIXELFORMATARBPROC)(HDC, const int*, const float*, UINT, int*,
                                                     UINT*);
#endif

#if defined(HAVE_X11) && !defined(GLX_CONTEXT_MAJOR_VERSION_ARB)
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif

namespace
{
LibretroGLCallbacks s_callbacks;
#if defined(_WIN32)
PFNWGLCREATECONTEXTATTRIBSARBPROC s_wglCreateContextAttribsARB = nullptr;
PFNWGLCHOOSEPIXELFORMATARBPROC s_wglChoosePixelFormatARB = nullptr;
PFNWGLCREATEPBUFFERARBPROC s_wglCreatePbufferARB = nullptr;
PFNWGLGETPBUFFERDCARBPROC s_wglGetPbufferDCARB = nullptr;
PFNWGLRELEASEPBUFFERDCARBPROC s_wglReleasePbufferDCARB = nullptr;
PFNWGLDESTROYPBUFFERARBPROC s_wglDestroyPbufferARB = nullptr;

bool CreatePBuffer(HDC onscreen_dc, int width, int height, HPBUFFERARB* out_pbuffer, HDC* out_dc)
{
  if (!s_wglChoosePixelFormatARB || !s_wglCreatePbufferARB || !s_wglGetPbufferDCARB ||
      !s_wglReleasePbufferDCARB || !s_wglDestroyPbufferARB)
  {
    ERROR_LOG_FMT(VIDEO, "Libretro WGL: Missing WGL_ARB_pbuffer extension");
    return false;
  }

  static constexpr std::array<int, 7 * 2> pf_iattribs = {
      WGL_DRAW_TO_PBUFFER_ARB,
      1,
      WGL_RED_BITS_ARB,
      0,
      WGL_GREEN_BITS_ARB,
      0,
      WGL_BLUE_BITS_ARB,
      0,
      WGL_DEPTH_BITS_ARB,
      0,
      WGL_STENCIL_BITS_ARB,
      0,
      0,
      0};

  static constexpr std::array<float, 1 * 2> pf_fattribs = {};

  int pixel_format = 0;
  UINT num_pixel_formats = 0;
  if (!s_wglChoosePixelFormatARB(onscreen_dc, pf_iattribs.data(), pf_fattribs.data(), 1,
                                 &pixel_format, &num_pixel_formats) ||
      num_pixel_formats == 0)
  {
    ERROR_LOG_FMT(VIDEO, "Libretro WGL: Failed to obtain a compatible pbuffer pixel format");
    return false;
  }

  static constexpr std::array<int, 1 * 2> pb_attribs = {};

  HPBUFFERARB pbuffer =
      s_wglCreatePbufferARB(onscreen_dc, pixel_format, width, height, pb_attribs.data());
  if (!pbuffer)
  {
    ERROR_LOG_FMT(VIDEO, "Libretro WGL: Failed to create pbuffer");
    return false;
  }

  HDC dc = s_wglGetPbufferDCARB(pbuffer);
  if (!dc)
  {
    ERROR_LOG_FMT(VIDEO, "Libretro WGL: Failed to get drawing context from pbuffer");
    s_wglDestroyPbufferARB(pbuffer);
    return false;
  }

  *out_pbuffer = pbuffer;
  *out_dc = dc;
  return true;
}

HGLRC CreateCoreContext(HDC dc, HGLRC share_context)
{
  if (!s_wglCreateContextAttribsARB)
  {
    ERROR_LOG_FMT(VIDEO, "Libretro WGL: Missing WGL_ARB_create_context extension");
    return nullptr;
  }

  static constexpr std::array<std::pair<int, int>, 9> k_versions = {
      {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}}};

  for (const auto& version : k_versions)
  {
    std::array<int, 5 * 2> attribs = {
        WGL_CONTEXT_PROFILE_MASK_ARB,
        WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
#ifdef _DEBUG
        WGL_CONTEXT_FLAGS_ARB,
        WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB | WGL_CONTEXT_DEBUG_BIT_ARB,
#else
        WGL_CONTEXT_FLAGS_ARB,
        WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
#endif
        WGL_CONTEXT_MAJOR_VERSION_ARB,
        version.first,
        WGL_CONTEXT_MINOR_VERSION_ARB,
        version.second,
        0,
        0};

    HGLRC core_context = s_wglCreateContextAttribsARB(dc, share_context, attribs.data());
    if (core_context)
    {
      INFO_LOG_FMT(VIDEO, "Libretro WGL: Created a GL {}.{} core context", version.first,
                   version.second);
      return core_context;
    }
  }

  ERROR_LOG_FMT(VIDEO, "Libretro WGL: Unable to create a core OpenGL context");
  return nullptr;
}
#endif
}  // namespace

void LibretroSetGLCallbacks(const LibretroGLCallbacks& callbacks)
{
  s_callbacks = callbacks;
#if defined(_WIN32)
  if (callbacks.get_proc_address)
  {
    auto get = callbacks.get_proc_address;
    s_wglCreateContextAttribsARB = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        get("wglCreateContextAttribsARB"));
    s_wglChoosePixelFormatARB =
        reinterpret_cast<PFNWGLCHOOSEPIXELFORMATARBPROC>(get("wglChoosePixelFormatARB"));
    s_wglCreatePbufferARB =
        reinterpret_cast<PFNWGLCREATEPBUFFERARBPROC>(get("wglCreatePbufferARB"));
    s_wglGetPbufferDCARB =
        reinterpret_cast<PFNWGLGETPBUFFERDCARBPROC>(get("wglGetPbufferDCARB"));
    s_wglReleasePbufferDCARB =
        reinterpret_cast<PFNWGLRELEASEPBUFFERDCARBPROC>(get("wglReleasePbufferDCARB"));
    s_wglDestroyPbufferARB =
        reinterpret_cast<PFNWGLDESTROYPBUFFERARBPROC>(get("wglDestroyPbufferARB"));
    if (!s_wglCreateContextAttribsARB)
    {
      s_wglCreateContextAttribsARB = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
          wglGetProcAddress("wglCreateContextAttribsARB"));
    }
    if (!s_wglChoosePixelFormatARB)
    {
      s_wglChoosePixelFormatARB = reinterpret_cast<PFNWGLCHOOSEPIXELFORMATARBPROC>(
          wglGetProcAddress("wglChoosePixelFormatARB"));
    }
    if (!s_wglCreatePbufferARB)
    {
      s_wglCreatePbufferARB = reinterpret_cast<PFNWGLCREATEPBUFFERARBPROC>(
          wglGetProcAddress("wglCreatePbufferARB"));
    }
    if (!s_wglGetPbufferDCARB)
    {
      s_wglGetPbufferDCARB = reinterpret_cast<PFNWGLGETPBUFFERDCARBPROC>(
          wglGetProcAddress("wglGetPbufferDCARB"));
    }
    if (!s_wglReleasePbufferDCARB)
    {
      s_wglReleasePbufferDCARB = reinterpret_cast<PFNWGLRELEASEPBUFFERDCARBPROC>(
          wglGetProcAddress("wglReleasePbufferDCARB"));
    }
    if (!s_wglDestroyPbufferARB)
    {
      s_wglDestroyPbufferARB = reinterpret_cast<PFNWGLDESTROYPBUFFERARBPROC>(
          wglGetProcAddress("wglDestroyPbufferARB"));
    }
  }
#endif
}

const LibretroGLCallbacks& LibretroGetGLCallbacks()
{
  return s_callbacks;
}

GLContextLibretro::~GLContextLibretro()
{
#if defined(_WIN32)
  if (m_context && m_owns_context)
    wglDeleteContext(static_cast<HGLRC>(m_context));
  if (m_owns_context && m_pbuffer_handle && m_dc && s_wglReleasePbufferDCARB && s_wglDestroyPbufferARB)
  {
    s_wglReleasePbufferDCARB(static_cast<HPBUFFERARB>(m_pbuffer_handle),
                             static_cast<HDC>(m_dc));
    s_wglDestroyPbufferARB(static_cast<HPBUFFERARB>(m_pbuffer_handle));
  }
#elif defined(HAVE_X11)
  if (m_display && m_context && m_owns_context)
    glXDestroyContext(static_cast<Display*>(m_display), static_cast<GLXContext>(m_context));
#endif
}

bool GLContextLibretro::Initialize(const WindowSystemInfo&, bool, bool)
{
  const auto& callbacks = LibretroGetGLCallbacks();
  if (!callbacks.get_proc_address)
    return false;

  m_opengl_mode = callbacks.is_gles ? Mode::OpenGLES : Mode::OpenGL;
  m_backbuffer_width = callbacks.base_width ? callbacks.base_width : 640;
  m_backbuffer_height = callbacks.base_height ? callbacks.base_height : 528;

#if defined(_WIN32)
  if (!callbacks.native_display || !callbacks.native_context)
    return false;

  HDC share_dc = static_cast<HDC>(callbacks.native_display);
  HGLRC share_context = static_cast<HGLRC>(callbacks.native_context);
  HPBUFFERARB pbuffer = nullptr;
  HDC pbuffer_dc = nullptr;
  if (!CreatePBuffer(share_dc, 32, 32, &pbuffer, &pbuffer_dc))
    return false;

  HGLRC context = nullptr;
  context = CreateCoreContext(pbuffer_dc, share_context);
  if (!context)
  {
    context = wglCreateContext(pbuffer_dc);
    if (context && share_context && !wglShareLists(share_context, context))
    {
      ERROR_LOG_FMT(VIDEO, "Libretro WGL: wglShareLists failed");
      wglDeleteContext(context);
      context = nullptr;
    }
  }

  if (!context)
  {
    s_wglReleasePbufferDCARB(pbuffer, pbuffer_dc);
    s_wglDestroyPbufferARB(pbuffer);
    return false;
  }

  m_pbuffer_handle = pbuffer;
  m_dc = pbuffer_dc;
  m_context = context;
  m_share_dc = callbacks.native_display;
  m_share_context = callbacks.native_context;
#elif defined(HAVE_X11)
  if (callbacks.native_display && callbacks.native_context)
  {
    m_display = callbacks.native_display;
    m_context = callbacks.native_context;
    m_drawable = callbacks.native_drawable;
    m_owns_context = false;
  }
  else
  {
    return false;
  }
#endif

  if (!MakeCurrent())
    return false;

  return true;
}

std::unique_ptr<GLContext> GLContextLibretro::CreateSharedContext()
{
  const auto& callbacks = LibretroGetGLCallbacks();
  if (!callbacks.get_proc_address)
    return nullptr;

#if defined(_WIN32)
  if (!m_share_dc || !m_context)
    return nullptr;

  HDC share_dc = static_cast<HDC>(m_share_dc);
  HGLRC share_context = static_cast<HGLRC>(m_context);
  HPBUFFERARB pbuffer = nullptr;
  HDC pbuffer_dc = nullptr;
  if (!CreatePBuffer(share_dc, 1, 1, &pbuffer, &pbuffer_dc))
    return nullptr;

  HGLRC context = CreateCoreContext(pbuffer_dc, share_context);
  if (!context)
  {
    context = wglCreateContext(pbuffer_dc);
    if (context && share_context && !wglShareLists(share_context, context))
    {
      ERROR_LOG_FMT(VIDEO, "Libretro WGL: wglShareLists failed for shared context");
      wglDeleteContext(context);
      context = nullptr;
    }
  }

  if (!context)
  {
    s_wglReleasePbufferDCARB(pbuffer, pbuffer_dc);
    s_wglDestroyPbufferARB(pbuffer);
    return nullptr;
  }

  auto shared = std::make_unique<GLContextLibretro>();
  shared->m_pbuffer_handle = pbuffer;
  shared->m_dc = pbuffer_dc;
  shared->m_context = context;
  shared->m_share_dc = m_share_dc;
  shared->m_share_context = m_context;
  shared->m_opengl_mode = m_opengl_mode;
  shared->m_is_shared = true;
  return shared;
#else
  return nullptr;
#endif
}

void* GLContextLibretro::GetFuncAddress(const std::string& name)
{
  const auto& callbacks = LibretroGetGLCallbacks();
  if (!callbacks.get_proc_address)
    return nullptr;
  return callbacks.get_proc_address(name.c_str());
}

void GLContextLibretro::UpdateBackbuffer()
{
  const auto& callbacks = LibretroGetGLCallbacks();
  if (!callbacks.get_proc_address)
    return;

  using GetIntegervFunc = void (*)(GLenum pname, GLint* data);
  auto get_integerv = reinterpret_cast<GetIntegervFunc>(callbacks.get_proc_address("glGetIntegerv"));
  if (!get_integerv)
    return;

  GLint viewport[4] = {};
  get_integerv(GL_VIEWPORT, viewport);
  if (viewport[2] > 0 && viewport[3] > 0)
  {
    m_backbuffer_width = viewport[2];
    m_backbuffer_height = viewport[3];
  }
}

void GLContextLibretro::Update()
{
  UpdateBackbuffer();
}

void GLContextLibretro::Swap()
{
  UpdateBackbuffer();
  const auto& callbacks = LibretroGetGLCallbacks();
  if (callbacks.present)
    callbacks.present(m_backbuffer_width, m_backbuffer_height);
}

uintptr_t GLContextLibretro::GetDefaultFramebuffer() const
{
  const auto& callbacks = LibretroGetGLCallbacks();
  if (!callbacks.get_current_framebuffer)
    return 0;
  return callbacks.get_current_framebuffer();
}

bool GLContextLibretro::MakeCurrent()
{
#if defined(_WIN32)
  if (m_context && m_dc)
    return wglMakeCurrent(static_cast<HDC>(m_dc), static_cast<HGLRC>(m_context)) == TRUE;
#elif defined(HAVE_X11)
  if (m_context && m_display && m_drawable)
  {
    return glXMakeCurrent(static_cast<Display*>(m_display),
                          static_cast<GLXDrawable>(m_drawable),
                          static_cast<GLXContext>(m_context)) == True;
  }
#endif
  return true;
}

bool GLContextLibretro::ClearCurrent()
{
#if defined(_WIN32)
  return wglMakeCurrent(nullptr, nullptr) == TRUE;
#elif defined(HAVE_X11)
  if (m_display)
    return glXMakeCurrent(static_cast<Display*>(m_display), None, nullptr) == True;
  return true;
#else
  return true;
#endif
}
