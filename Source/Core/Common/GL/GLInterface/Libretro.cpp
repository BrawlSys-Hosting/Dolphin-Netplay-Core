// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLInterface/Libretro.h"

#include "Common/GL/GLExtensions/gl_1_1.h"

namespace
{
LibretroGLCallbacks s_callbacks;
}  // namespace

void LibretroSetGLCallbacks(const LibretroGLCallbacks& callbacks)
{
  s_callbacks = callbacks;
}

const LibretroGLCallbacks& LibretroGetGLCallbacks()
{
  return s_callbacks;
}

bool GLContextLibretro::Initialize(const WindowSystemInfo&, bool, bool)
{
  const auto& callbacks = LibretroGetGLCallbacks();
  if (!callbacks.get_proc_address)
    return false;

  m_opengl_mode = callbacks.is_gles ? Mode::OpenGLES : Mode::OpenGL;
  m_backbuffer_width = callbacks.base_width ? callbacks.base_width : 640;
  m_backbuffer_height = callbacks.base_height ? callbacks.base_height : 528;
  return true;
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
