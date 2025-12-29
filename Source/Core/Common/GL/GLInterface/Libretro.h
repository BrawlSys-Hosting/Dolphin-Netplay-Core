// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>

#include "Common/GL/GLContext.h"

struct LibretroGLCallbacks
{
  using GetProcAddress = void* (*)(const char* name);
  using GetFramebuffer = uintptr_t (*)();
  using PresentCallback = void (*)(unsigned width, unsigned height);

  GetProcAddress get_proc_address = nullptr;
  GetFramebuffer get_current_framebuffer = nullptr;
  PresentCallback present = nullptr;
  unsigned base_width = 0;
  unsigned base_height = 0;
  bool is_gles = false;
  void* native_display = nullptr;
  void* native_context = nullptr;
  uintptr_t native_drawable = 0;
};

void LibretroSetGLCallbacks(const LibretroGLCallbacks& callbacks);
const LibretroGLCallbacks& LibretroGetGLCallbacks();

class GLContextLibretro final : public GLContext
{
public:
  ~GLContextLibretro() override;

  bool IsHeadless() const override { return false; }
  std::unique_ptr<GLContext> CreateSharedContext() override;
  bool MakeCurrent() override;
  bool ClearCurrent() override;
  void Update() override;
  void Swap() override;
  void SwapInterval(int) override {}
  void* GetFuncAddress(const std::string& name) override;
  uintptr_t GetDefaultFramebuffer() const override;

protected:
  bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core) override;

private:
  void UpdateBackbuffer();
  bool m_owns_context = true;

#if defined(_WIN32)
  void* m_dc = nullptr;
  void* m_context = nullptr;
  void* m_pbuffer_handle = nullptr;
  void* m_share_dc = nullptr;
  void* m_share_context = nullptr;
#elif defined(HAVE_X11)
  void* m_display = nullptr;
  void* m_context = nullptr;
  uintptr_t m_drawable = 0;
#endif
};
