# Ashita and Windower addons

> [!NOTE]
> In progress. Every official Ashita v4 and Windower 4 addon loads and runs in the addon test host,
> but this hasn't been played in the live game yet.

Ashita v4 and Windower 4 **Lua addons** run unmodified, with those projects' own libraries, on
XI on Anything's own addon host: no DLL injection, no Windows. Each addon gets its own LuaJIT state;
the host provides Ashita's and Windower's Lua APIs over the recompiled game, and draws their ImGui
windows, text and images over the game's frame.

- `tools/setup.py` fetches Ashita's and Windower's addons, libraries and resources (pinned
  versions from their own repositories) into
  `~/Library/Application Support/FFXIRecompile/FFXI/ashita` and `.../windower`, laid out like
  their installs. Put third-party addons in the same folders.
- Load them as you would there: `/addon load <name>`, `//lua load <name>`, or at start from
  `ashita/scripts/default.txt` or `windower/scripts/init.txt`. `/bind`, `/alias` and
  `//exec` work too.
- Addons that patch the game's code (instantchat, macrofix, fastswap and the like) work through
  translated variants of the patched functions, chosen while the addon's bytes are in place.
- **Native plugins** (Ashita and Windower DLLs) don't run: they're 32-bit Windows code. Addons that
  need one (Windower's Timers-based ones, for example) won't either.
- **Which addon makes the game stutter**: `/addon prof` lists the addons by the time they take per
  second of play (`/addon prof <name>` for one addon's events, `/addon prof reset` to start
  counting again). Frames that come late are logged in `host64.log` with the addons' share of them.
- **Errors**: an addon's Lua error goes to the chat log (its traceback to `host64.log`); one that
  keeps failing for 5 seconds, or touches memory outside the game's, is unloaded.

## Where the code is

| Piece | Files |
|---|---|
| Recompiler wraps | `recomp/recomp.py --wraps`, `meta/builds.json` "wraps", `runtime/runtime.h` rt_arg/rt_return |
| Core: addons, states, events, errors, crash guard | `host/addons/core.c`, `host.h`, `addons.h` |
| Game hooks: command line, chat log, packets | `host/addons/hooks.c` |
| Commands, scripts, aliases | `host/addons/cmd.c` |
| Input, binds | `host/addons/keys.c` (+ `user32_event_hook` in user32.c) |
| Overlay: ImGui, text objects, primitives, textures | `host/addons/gui.cpp` (+ `d3d8_set_overlay`, `GfxDraw.scissor`) |
| Ashita's ImGui manager for Lua | `host/addons/gui_lua.cpp`, `tools/gen_imgui_lua.py` |
| Native `xi.*` | `host/addons/lua_xi.c` |
| Runtime every state starts with | `host/addons/lua/xi.lua` |
| Game data (Ashita's IMemoryManager) | `host/addons/game.c`, `game_lua.c`, `lua/ashita_memory.lua` |
| DAT resources | `host/addons/res.c`, `res_lua.c` |
| Ashita / Windower layers | `host/addons/lua/ashita*.lua`, `lua/windower*.lua` |
| Headless test host | `host64 --addon-harness <script>` (`host/addons/harness.c`) |
| Config > Addons (on/off a row each, scrolling; `addons.cfg`) | `host/addons/manage.c` (list, autoload), `host/modern.c` (the page) |
| Fetching the projects' libraries | `tools/addons_fetch.py` (run by `tools/setup.py`) |
