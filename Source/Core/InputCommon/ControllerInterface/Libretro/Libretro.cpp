// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/Libretro/Libretro.h"

#include <memory>
#include <optional>
#include <string>

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/InputBackend.h"
#include "InputCommon/LibretroInput.h"

#include <libretro.h>

namespace ciface::Libretro
{
namespace
{
constexpr int AXIS_MAX = 32767;
constexpr int AXIS_MIN = -32768;

class LibretroButton final : public Core::Device::Input
{
public:
  LibretroButton(unsigned port, unsigned id, std::string name)
      : m_port(port), m_id(id), m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }

  ControlState GetState() const override
  {
    const auto state = InputCommon::GetLibretroInputState();
    if (!state)
      return 0.0;

    return state(m_port, RETRO_DEVICE_JOYPAD, 0, m_id) ? 1.0 : 0.0;
  }

private:
  unsigned m_port;
  unsigned m_id;
  std::string m_name;
};

class LibretroAxis final : public Core::Device::Input
{
public:
  LibretroAxis(unsigned port, unsigned index, unsigned id, int range, std::string name)
      : m_port(port), m_index(index), m_id(id), m_range(range), m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }

  ControlState GetState() const override
  {
    const auto state = InputCommon::GetLibretroInputState();
    if (!state)
      return 0.0;

    const int16_t value = state(m_port, RETRO_DEVICE_ANALOG, m_index, m_id);
    return static_cast<ControlState>(value) / m_range;
  }

private:
  unsigned m_port;
  unsigned m_index;
  unsigned m_id;
  int m_range;
  std::string m_name;
};

class LibretroDevice final : public Core::Device
{
public:
  explicit LibretroDevice(unsigned port) : m_port(port)
  {
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_A, "Button A"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_B, "Button B"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_X, "Button X"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_Y, "Button Y"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_L, "Shoulder L"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_R, "Shoulder R"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_L2, "Trigger L"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_R2, "Trigger R"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_L3, "L3"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_R3, "R3"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_START, "Start"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_UP, "Pad N"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_DOWN, "Pad S"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_LEFT, "Pad W"));
    AddInput(new LibretroButton(m_port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Pad E"));

    AddInput(new LibretroAxis(m_port, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                              RETRO_DEVICE_ID_ANALOG_X, AXIS_MIN, "Left X-"));
    AddInput(new LibretroAxis(m_port, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                              RETRO_DEVICE_ID_ANALOG_X, AXIS_MAX, "Left X+"));
    AddInput(new LibretroAxis(m_port, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                              RETRO_DEVICE_ID_ANALOG_Y, AXIS_MIN, "Left Y+"));
    AddInput(new LibretroAxis(m_port, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                              RETRO_DEVICE_ID_ANALOG_Y, AXIS_MAX, "Left Y-"));
    AddInput(new LibretroAxis(m_port, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                              RETRO_DEVICE_ID_ANALOG_X, AXIS_MIN, "Right X-"));
    AddInput(new LibretroAxis(m_port, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                              RETRO_DEVICE_ID_ANALOG_X, AXIS_MAX, "Right X+"));
    AddInput(new LibretroAxis(m_port, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                              RETRO_DEVICE_ID_ANALOG_Y, AXIS_MIN, "Right Y+"));
    AddInput(new LibretroAxis(m_port, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                              RETRO_DEVICE_ID_ANALOG_Y, AXIS_MAX, "Right Y-"));
  }

  std::string GetName() const override { return "Pad " + std::to_string(m_port + 1); }
  std::string GetSource() const override { return "Libretro"; }
  bool IsVirtualDevice() const override { return true; }

  std::optional<int> GetPreferredId() const override
  {
    return static_cast<int>(m_port);
  }

  int GetSortPriority() const override { return 1000; }

private:
  unsigned m_port;
};
}  // namespace

class InputBackend final : public ciface::InputBackend
{
public:
  InputBackend(ControllerInterface* controller_interface) : ciface::InputBackend(controller_interface)
  {
  }

  void PopulateDevices() override
  {
    GetControllerInterface().PlatformPopulateDevices([this] {
      for (unsigned port = 0; port < 4; ++port)
        GetControllerInterface().AddDevice(std::make_shared<LibretroDevice>(port));
    });
  }

  void UpdateInput(std::vector<std::weak_ptr<ciface::Core::Device>>&) override
  {
    auto poll = InputCommon::GetLibretroInputPoll();
    if (poll)
      poll();
  }
};

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}
}  // namespace ciface::Libretro
