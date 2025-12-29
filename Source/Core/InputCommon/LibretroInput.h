// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace InputCommon
{
using LibretroInputPoll = void (*)();
using LibretroInputState = int16_t (*)(unsigned port, unsigned device, unsigned index, unsigned id);

void SetLibretroInputPoll(LibretroInputPoll cb);
void SetLibretroInputState(LibretroInputState cb);
LibretroInputPoll GetLibretroInputPoll();
LibretroInputState GetLibretroInputState();
}  // namespace InputCommon
