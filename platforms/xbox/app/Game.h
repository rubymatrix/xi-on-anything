// Runs xi-on-anything's game host (host64's main, linked in as host_main from build/uwp/ffxi_uwp.lib)
// on its own thread. It shows its own sign-in screen first (host/signin.c) and draws everything on the
// app's window. Its log, and everything the app says, go to LocalState\host64.log.
#pragma once

namespace Game
{
    // Call once, on the UI thread. When the game ends the app closes, as the game would; when it
    // cannot start, or ends with an error, the app says so first.
    void Start();
}
