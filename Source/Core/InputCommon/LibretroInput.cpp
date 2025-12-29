// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/LibretroInput.h"

#include <atomic>

namespace InputCommon
{
namespace
{
std::atomic<LibretroInputPoll> s_input_poll{nullptr};
std::atomic<LibretroInputState> s_input_state{nullptr};
}  // namespace

void SetLibretroInputPoll(LibretroInputPoll cb)
{
  s_input_poll.store(cb);
}

void SetLibretroInputState(LibretroInputState cb)
{
  s_input_state.store(cb);
}

LibretroInputPoll GetLibretroInputPoll()
{
  return s_input_poll.load();
}

LibretroInputState GetLibretroInputState()
{
  return s_input_state.load();
}
}  // namespace InputCommon
