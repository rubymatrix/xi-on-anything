// The window's input for the game (uwp_bridge.h): its size, keyboard, mouse and focus, and the first
// controller's state 125 times a second, with its rumble.
#pragma once

namespace Input
{
    // Starts listening for controllers; call at launch. Windows lists them a moment after an app
    // first listens, and the game enumerates its pads once, as it starts.
    void WatchControllers();
    // Call once, on the UI thread, before the game starts: the view's size is the game's "desktop".
    void Attach(winrt::Windows::UI::Core::CoreWindow const& window);
}
