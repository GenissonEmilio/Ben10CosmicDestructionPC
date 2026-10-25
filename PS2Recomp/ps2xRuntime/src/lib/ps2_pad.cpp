#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <string>
#include <vector>

#ifdef _WIN32
bool ps2PadWindowsRumble(uint8_t large, bool small); // ps2_pad_rumble_win.cpp
#endif

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    // Repeatable input for local integration tests. Adapted from the GPL-3.0
    // GodOfWar2PC diagnostic script, without any game-specific PAD2 addresses.
    struct ScriptPress { double from, to, period; uint32_t mask; };
    uint32_t scriptedButtons()
    {
        static const char *text = std::getenv("COSMIC_PAD_SCRIPT");
        if (!text) return 0;
        static const auto presses = [] {
            const std::pair<const char *, uint32_t> buttons[] = {
                {"select", PAD_SELECT}, {"start", PAD_START}, {"up", PAD_UP},
                {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
                {"x", PAD_CROSS}, {"o", PAD_CIRCLE}, {"sq", PAD_SQUARE},
                {"tri", PAD_TRIANGLE}, {"l1", PAD_L1}, {"r1", PAD_R1},
                {"l2", PAD_L2}, {"r2", PAD_R2}, {"lup", 0x10000},
                {"ldown", 0x20000}, {"lleft", 0x40000}, {"lright", 0x80000}};
            std::vector<ScriptPress> result;
            std::string item;
            for (const char *c = text;; ++c) {
                if (*c && *c != ',') { item += *c; continue; }
                const auto colon = item.find(':');
                if (colon != std::string::npos) {
                    const auto when = item.substr(0, colon), name = item.substr(colon + 1);
                    char *end = nullptr;
                    ScriptPress press{};
                    press.from = press.to = std::strtod(when.c_str(), &end);
                    if (end && *end == '-') press.to = std::strtod(end + 1, &end);
                    if (end && *end == '/') press.period = std::strtod(end + 1, &end);
                    for (const auto &[button, mask] : buttons)
                        if (name == button) press.mask = mask;
                    if (end && !*end && press.mask && press.from >= 0 &&
                        press.to >= press.from && press.period >= 0) result.push_back(press);
                }
                item.clear();
                if (!*c) break;
            }
            return result;
        }();
        static const auto started = std::chrono::steady_clock::now();
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        uint32_t pressed = 0;
        for (const auto &press : presses) {
            if (now < press.from || now > press.to + 0.3) continue;
            const double elapsed = now - press.from;
            const double phase = press.period > 0 ? elapsed - press.period * long(elapsed / press.period) : elapsed;
            if ((press.period <= 0 && press.to > press.from) || phase < 0.3) pressed |= press.mask;
        }
        static uint32_t previous = 0;
        if (pressed != previous) std::fprintf(stderr, "[cosmic-pad-script] t=%.1f buttons=%05x\n", now, pressed);
        previous = pressed;
        return pressed;
    }

    void loadGamepadMappings()
    {
#ifdef __linux__
        if (!IsWindowReady())
            return;
        // The Zikway HID / GameSir Nova Lite layout is exposed by SDL's HID
        // backend, but is missing from GLFW's bundled Linux mapping database.
        // Use GLFW's GUID (without SDL's name CRC) and its evdev axis order.
        static const int loaded = SetGamepadMappings(
            "03000000373500004110000011010000,Zikway HID gamepad,"
            "a:b0,b:b1,x:b3,y:b4,back:b10,guide:b12,start:b11,"
            "leftstick:b13,rightstick:b14,leftshoulder:b6,rightshoulder:b7,"
            "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
            "lefttrigger:a5,righttrigger:a4,platform:Linux,");
        (void)loaded;
#endif
    }
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    loadGamepadMappings();
    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    // GOW-Port: GLFW can list racing gear (handbrake, pedals) as gamepads before the real pad. The pad whose
    // button was pressed last becomes the active one (press edges, so idle devices never take over);
    // until then, the first available slot.
    static int s_activeGamepad = -1;
    for (int i = 0; i < 4; ++i)
    {
        if (i == s_activeGamepad || !IsGamepadAvailable(i))
            continue;
        for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
            if (IsGamepadButtonPressed(i, button))
            {
                s_activeGamepad = i;
                break;
            }
    }
    if (s_activeGamepad >= 0 && !IsGamepadAvailable(s_activeGamepad))
        s_activeGamepad = -1;
    int kGamepad = s_activeGamepad;
    for (int i = 0; i < 4 && kGamepad < 0; ++i)
        if (IsGamepadAvailable(i))
            kGamepad = i;
    const bool useGamepad = kGamepad >= 0;
    static int s_loggedGamepad = -2;
    if (kGamepad != s_loggedGamepad)
    {
        s_loggedGamepad = kGamepad;
        std::fprintf(stderr, "[pad] gamepad slot %d: %s\n", kGamepad, useGamepad ? GetGamepadName(kGamepad) : "none (keyboard)");
    }
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    else
    {
        if (IsKeyDown(KEY_UP))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_ONE))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT) || IsKeyDown(KEY_THREE))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
        data[6] = IsKeyDown(KEY_A) ? 0 : IsKeyDown(KEY_D) ? 255 : 128;
        data[7] = IsKeyDown(KEY_W) ? 0 : IsKeyDown(KEY_S) ? 255 : 128;
        data[4] = IsKeyDown(KEY_J) ? 0 : IsKeyDown(KEY_L) ? 255 : 128;
        data[5] = IsKeyDown(KEY_I) ? 0 : IsKeyDown(KEY_K) ? 255 : 128;
    }

    const uint32_t scripted = scriptedButtons();
    btns &= ~uint16_t(scripted);
    if (scripted & 0x10000) data[7] = 0;
    if (scripted & 0x20000) data[7] = 255;
    if (scripted & 0x40000) data[6] = 0;
    if (scripted & 0x80000) data[6] = 255;

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);

    if (kGamepad != m_activeGamepad)
    {
        m_activeGamepad = kGamepad;
        applyRumble(false); // the new gamepad takes over the current actuator state
    }
    else
        applyRumble(true);
    return true;
}

void PSPadBackend::setActuators(int port, bool small, uint8_t large)
{
    if (port != 0 || (small == m_small && large == m_large))
        return;
    m_small = small;
    m_large = large;
    applyRumble(false);
}

// refresh = periodic call from readState: only backends whose rumble times out need it resent.
void PSPadBackend::applyRumble(bool refresh)
{
#ifdef _WIN32
    // raylib's GLFW backend has no vibration: XInput and Sony HID pads directly. Both keep the motors running until changed.
    if (refresh)
        return;
    ps2PadWindowsRumble(m_large, m_small);
#else
    // Other platforms (SDL, Switch): raylib's rumble, which stops after at most 2 s, so it is resent every second.
    if (m_activeGamepad < 0 || !IsWindowReady())
        return;
    const bool on = m_small || m_large;
    const double now = GetTime();
    if (refresh && (!on || now - m_rumbleSentAt < 1.0))
        return;
    m_rumbleSentAt = now;
    SetGamepadVibration(m_activeGamepad, m_large / 255.0f, m_small ? 1.0f : 0.0f, on ? 2.0f : 0.1f); // zero motors with any duration stop it
#endif
}
