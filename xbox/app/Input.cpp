#include "pch.h"

#include "Input.h"

#include "TextInput.h"
#include "uwp_bridge.h"

using namespace winrt;
using namespace Windows::Gaming::Input;
using namespace Windows::System;
using namespace Windows::UI::Core;

namespace
{
    // --- the keyboard: VirtualKey -> SDL_Scancode (USB HID usage) ---------------------------------------
    int scancode_of(VirtualKey k)
    {
        int v = (int)k;
        if (v >= 'A' && v <= 'Z')
            return 4 + (v - 'A');
        if (v >= '1' && v <= '9')
            return 30 + (v - '1');
        if (v == '0')
            return 39;
        if (v >= (int)VirtualKey::F1 && v <= (int)VirtualKey::F12)
            return 58 + (v - (int)VirtualKey::F1);
        if (v >= (int)VirtualKey::NumberPad1 && v <= (int)VirtualKey::NumberPad9)
            return 89 + (v - (int)VirtualKey::NumberPad1);
        switch (k)
        {
        case VirtualKey::NumberPad0: return 98;
        case VirtualKey::Enter: return 40;
        case VirtualKey::Escape: return 41;
        case VirtualKey::Back: return 42;
        case VirtualKey::Tab: return 43;
        case VirtualKey::Space: return 44;
        case VirtualKey::CapitalLock: return 57;
        case VirtualKey::Snapshot: return 70;
        case VirtualKey::Scroll: return 71;
        case VirtualKey::Pause: return 72;
        case VirtualKey::Insert: return 73;
        case VirtualKey::Home: return 74;
        case VirtualKey::PageUp: return 75;
        case VirtualKey::Delete: return 76;
        case VirtualKey::End: return 77;
        case VirtualKey::PageDown: return 78;
        case VirtualKey::Right: return 79;
        case VirtualKey::Left: return 80;
        case VirtualKey::Down: return 81;
        case VirtualKey::Up: return 82;
        case VirtualKey::NumberKeyLock: return 83;
        case VirtualKey::Divide: return 84;
        case VirtualKey::Multiply: return 85;
        case VirtualKey::Subtract: return 86;
        case VirtualKey::Add: return 87;
        case VirtualKey::Decimal: return 99;
        case VirtualKey::LeftControl: case VirtualKey::Control: return 224;
        case VirtualKey::LeftShift: case VirtualKey::Shift: return 225;
        case VirtualKey::LeftMenu: case VirtualKey::Menu: return 226;
        case VirtualKey::RightControl: return 228;
        case VirtualKey::RightShift: return 229;
        case VirtualKey::RightMenu: return 230;
        default: break;
        }
        switch (v) // the OEM keys (US layout)
        {
        case 0xBD: return 45; // - _
        case 0xBB: return 46; // = +
        case 0xDB: return 47; // [
        case 0xDD: return 48; // ]
        case 0xDC: return 49; // backslash
        case 0xBA: return 51; // ;
        case 0xDE: return 52; // '
        case 0xC0: return 53; // `
        case 0xBC: return 54; // ,
        case 0xBE: return 55; // .
        case 0xBF: return 56; // /
        default: return 0;
        }
    }

    // --- the controller: Windows.Gaming.Input -> SDL's buttons and axes --------------------------------------
    void poll_gamepad()
    {
        UwpPad pad{};
        auto pads = Gamepad::Gamepads();
        if (pads.Size())
        {
            GamepadReading r = pads.GetAt(0).GetCurrentReading();
            pad.connected = 1;
            struct { GamepadButtons from; int to; } const MAP[] = {
                { GamepadButtons::A, 0 }, { GamepadButtons::B, 1 }, { GamepadButtons::X, 2 }, { GamepadButtons::Y, 3 },
                { GamepadButtons::View, 4 }, { GamepadButtons::Menu, 6 }, { GamepadButtons::LeftThumbstick, 7 },
                { GamepadButtons::RightThumbstick, 8 }, { GamepadButtons::LeftShoulder, 9 },
                { GamepadButtons::RightShoulder, 10 }, { GamepadButtons::DPadUp, 11 }, { GamepadButtons::DPadDown, 12 },
                { GamepadButtons::DPadLeft, 13 }, { GamepadButtons::DPadRight, 14 },
            };
            for (auto const& m : MAP)
                if ((r.Buttons & m.from) == m.from)
                    pad.buttons |= 1u << m.to;
            auto axis = [](double v) { return (int16_t)std::clamp(v * 32767.0, -32768.0, 32767.0); };
            pad.axes[0] = axis(r.LeftThumbstickX);
            pad.axes[1] = axis(-r.LeftThumbstickY); // SDL: down is positive
            pad.axes[2] = axis(r.RightThumbstickX);
            pad.axes[3] = axis(-r.RightThumbstickY);
            pad.axes[4] = axis(r.LeftTrigger);
            pad.axes[5] = axis(r.RightTrigger);
        }
        // While the on-screen keyboard is up the controller types into it: none of it reaches the
        // game, nor do the buttons that closed it until they are let go.
        static uint32_t held_back;
        if (TextInput::IsUp())
        {
            held_back = ~0u;
            memset(pad.axes, 0, sizeof pad.axes);
        }
        held_back &= pad.buttons;
        pad.buttons &= ~held_back;
        uwp_gamepad(&pad);
    }

    void rumble(uint16_t low, uint16_t high)
    {
        auto pads = Gamepad::Gamepads();
        if (!pads.Size())
            return;
        GamepadVibration v{};
        v.LeftMotor = low / 65535.0;
        v.RightMotor = high / 65535.0;
        pads.GetAt(0).Vibration(v);
    }
}

void Input::WatchControllers()
{
    Gamepad::GamepadAdded([](auto&&, Gamepad const&) {
        fprintf(stderr, "[app] controller connected (%u in all)\n", Gamepad::Gamepads().Size());
    });
    Gamepad::GamepadRemoved([](auto&&, Gamepad const&) {
        fprintf(stderr, "[app] controller disconnected (%u left)\n", Gamepad::Gamepads().Size());
    });
    (void)Gamepad::Gamepads();
}

void Input::Attach(CoreWindow const& w)
{
    uwp_set_rumble(rumble);
    auto b = w.Bounds();
    float scale = (float)Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
    uwp_view_size((int)(b.Width * scale), (int)(b.Height * scale));
    w.SizeChanged([scale](CoreWindow const&, WindowSizeChangedEventArgs const& e) {
        uwp_view_size((int)(e.Size().Width * scale), (int)(e.Size().Height * scale));
    });
    // While the on-screen keyboard is up its text comes through TextInput alone: the console also
    // sends its keys to the window, and passing those on as well would type everything twice.
    w.KeyDown([](CoreWindow const&, KeyEventArgs const& e) {
        if (TextInput::IsUp())
            return;
        if (int sc = scancode_of(e.VirtualKey()))
        {
            uwp_key(sc, 1, e.KeyStatus().WasKeyDown);
            e.Handled(true);
        }
    });
    w.KeyUp([](CoreWindow const&, KeyEventArgs const& e) {
        if (int sc = scancode_of(e.VirtualKey()))
        {
            uwp_key(sc, 0, 0);
            e.Handled(true);
        }
    });
    w.CharacterReceived([](CoreWindow const&, CharacterReceivedEventArgs const& e) {
        if (TextInput::IsUp())
            return;
        uint32_t c = e.KeyCode();
        if (c < 32 || c == 127)
            return; // Enter, Backspace, Tab, Escape arrive as keys
        wchar_t wc[2] = { (wchar_t)c, 0 };
        uwp_text(to_string(wc).c_str());
    });
    w.PointerMoved([scale](CoreWindow const&, PointerEventArgs const& e) {
        static bool logged;
        if (!logged)
            logged = true, fprintf(stderr, "[app] first pointer event: device type %d\n", (int)e.CurrentPoint().PointerDevice().PointerDeviceType());
        auto p = e.CurrentPoint().Position();
        uwp_mouse_move(p.X * scale, p.Y * scale);
    });
    auto buttons = [](PointerEventArgs const& e) {
        static bool was[3];
        auto props = e.CurrentPoint().Properties();
        bool now[3] = { props.IsLeftButtonPressed(), props.IsMiddleButtonPressed(), props.IsRightButtonPressed() };
        for (int i = 0; i < 3; ++i)
            if (now[i] != was[i])
                uwp_mouse_button(i + 1, now[i]), was[i] = now[i]; // SDL: 1 left, 2 middle, 3 right
    };
    w.PointerPressed([buttons](CoreWindow const&, PointerEventArgs const& e) { buttons(e); });
    w.PointerReleased([buttons](CoreWindow const&, PointerEventArgs const& e) { buttons(e); });
    w.PointerWheelChanged([](CoreWindow const&, PointerEventArgs const& e) {
        uwp_mouse_wheel(e.CurrentPoint().Properties().MouseWheelDelta() / 120.0f);
    });
    w.Activated([](CoreWindow const&, WindowActivatedEventArgs const& e) {
        uwp_focus(e.WindowActivationState() != CoreWindowActivationState::Deactivated);
    });
    w.Closed([](CoreWindow const&, CoreWindowEventArgs const&) { uwp_close(); });
    uwp_focus(1);

    // B is also the system's Back: while the game runs it is the game's button, not a navigation
    Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
        [](auto&&, Windows::UI::Core::BackRequestedEventArgs const& e) { e.Handled(true); });

    // the first controller's state, 125 times a second
    std::thread([] {
        while (true)
        {
            poll_gamepad();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }).detach();
}
