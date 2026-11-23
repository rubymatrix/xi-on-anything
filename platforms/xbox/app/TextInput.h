// The on-screen keyboard without XAML: a CoreTextEditContext the system keyboard types into, opened
// when the sign-in screen asks for it over a field (uwp_bridge.h's UwpTextInput). It starts with the
// field's text; each change hands the whole text back to the field (uwp_replace_text).
#pragma once

namespace TextInput
{
    // Call once, on the UI thread, with the app's window.
    void Attach(winrt::Windows::UI::Core::CoreWindow const& window);
    // Whether the keyboard is up: the controller is the keyboard's then, not the game's.
    bool IsUp();
}
