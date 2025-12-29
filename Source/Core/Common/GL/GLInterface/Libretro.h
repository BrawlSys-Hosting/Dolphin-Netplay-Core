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
};

void LibretroSetGLCallbacks(const LibretroGLCallbacks& callbacks);
const LibretroGLCallbacks& LibretroGetGLCallbacks();

class GLContextLibretro final : public GLContext
{
public:
  ~GLContextLibretro() override = default;

  bool IsHeadless() const override { return false; }
  std::unique_ptr<GLContext> CreateSharedContext() override { return nullptr; }
  bool MakeCurrent() override { return true; }
  bool ClearCurrent() override { return true; }
  void Update() override;
  void Swap() override;
  void SwapInterval(int) override {}
  void* GetFuncAddress(const std::string& name) override;
  uintptr_t GetDefaultFramebuffer() const override;

protected:
  bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core) override;

private:
  void UpdateBackbuffer();
};
