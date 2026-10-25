# iPhone and iPad: display and controls (design notes)

These notes cover how the iOS build should size the game for landscape phones and iPads, and how
it should be played without a keyboard. Each item says what the code does today and what should
change. Paths are from the repository root. Nothing here is built yet.

## What happens today

- **Resolution:** the frame is drawn at one pixel per point. `gfx_window_flags()` in `gfx_metal.m`
  returns 0, so the window lacks `SDL_WINDOW_HIGH_PIXEL_DENSITY`. An iPhone 18 Pro (874x402 points,
  scale 3) gets an 874x402 drawable, and iOS enlarges it three times, so it looks soft.
- **Back buffer:** the game draws at its own back buffer, by default 1920x1080, and
  `gfx_present` stretches that across the drawable. A 16:9 buffer on a 19.5:9 screen is
  stretched sideways. `fix_aspect` and the nameplate fix (host64.c) use the size that was asked
  for, not the screen's, so they cannot correct for it.
- **Pointer:** touches reach the game as SDL's synthetic left mouse button and motion, because
  nothing handles `SDL_EVENT_FINGER_*`. The pump posts points as client pixels without scaling
  (user32.c, near line 648), so on a phone a tap lands in the top-left part of the 1920x1080
  client. The same touch also feeds the DirectInput mouse delta.
- **Safe area and resizing:** there is no safe-area handling. A live resize reaches the game only
  through the web port and the Linux compositor hook (`modern_window_size` and
  `display_apply`, modern.c).

## Display

### Sizing: port the web port's policy to C

`tools/web/hud.js` `sizes()` already solves sizing for any window:
- **Back buffer:** the window's pixels, capped at a pixel budget.
- **Interface:** about `ui` CSS pixels per interface pixel, at least 640x480 and at most 1200
  lines.
- **Applying it:** the shared C path `modern_window_size` → `display_apply` → `menu_apply` →
  `windows_follow`.

Only the policy is JavaScript. A C version (`host/display_policy.c`) would take four inputs:
- the window's size in points
- its pixel density
- the safe-area insets
- the player's UI scale

It would return two sizes, the back buffer and the menu size. Call it at start and on
`SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`.

With the right size passed in, `fix_aspect` and the nameplates are correct, because they read
the client size the game was given. The desktop SDL host could use the same function later.

Rules for iOS:
- **Native pixels:** turn on `SDL_WINDOW_HIGH_PIXEL_DENSITY` (`gfx_window_flags` under
  `TARGET_OS_IPHONE`) so the drawable is the screen's real size.
- **Pixel budget:** the back buffer is the drawable scaled down to fit a budget.
  - Defaults: about 2.4 MP on phones and 4 MP on iPads (2/3 and 0.7 of native).
  - Phones run hot and on battery, so this is the main performance setting, shown as Config >
    Display > Resolution.
  - CAS sharpening (`present_cas_fs`) already makes up some of the loss when enlarging.
- **Interface:** at least 480 lines.
  - The game lays its windows out in at least 512x448. Below 448 lines, windows anchored to the
    bottom and centred windows overlap.
  - On a phone, the 402-point height forces 480 lines (0.84 points per interface pixel).
  - `native_size()` (d3d8.c) draws the interface at back-buffer resolution, so text stays sharp
    at about 2.5 device pixels per interface pixel.

| Device (landscape) | Points | Scale | Back buffer (budget) | Interface | Note |
|---|---|---|---|---|---|
| iPhone 18 Pro | 874x402 | 3 | 1748x804 (native 2622x1206) | 1044x480 | Dynamic Island inset ~62 pt each side |
| iPhone 18 Pro Max | 956x440 | 3 | 1912x880 | 1042x480 | |
| iPhone 17e | 844x390 | 3 | 1688x780 | 1038x480 | notch |
| iPad mini | 1133x744 | 2 | 2266x1488 | 1133x744 | 1 pt per interface pixel |
| iPad Pro 13" | 1376x1032 | 2 | 2310x1732 (native 2752x2064) | 1376x1032 | UI scale 1.25 gives 1100x826 |

The iPhone sizes come from the simulator's device types. Check each on its device.

### Safe area

`SDL_GetWindowSafeArea` gives the insets:
- **iPhone, landscape:** about 62 pt on each side for the Dynamic Island, and about 21 pt at
  the bottom for the home indicator. The phone may be rotated either way, so both sides need
  the inset.
- **iPad:** only about 20 pt at the bottom.

Two ways to use them:
1. **Letterbox (simplest, first):** draw everything in the safe rectangle and leave black bars.
   - On an iPhone 18 Pro that is 750x381 points (1.97:1).
   - Metal needs a destination rectangle in `gfx_present`, as `present_rect.h` does for Vulkan.
   - Pointer mapping subtracts the offset.
2. **Full-bleed 3D, interface in the safe rectangle (better, later):**
   - The world fills the screen and the menus avoid the cutout.
   - `ui_squeeze` (user32.c) already moves the interface into a centred box for the
     interface shape option. It would need to take any rectangle, not only a centred aspect.

Recommendation: letterbox first, then full-bleed once the box is generalised. On a phone the 3D
view under the Dynamic Island is worth having.

### iPad specifics

- **Aspect:** 4:3 to 1.43:1 needs nothing special. The 16:9 shape keeps menus as on desktop; Full
  uses the whole width.
- **Resizing:** Stage Manager and Split View resize the window freely. `UIRequiresFullScreen` is
  on today, so neither applies. Keep it for now; turn it off once the resize event drives
  `modern_window_size`.
- **Keyboard and mouse:** a hardware keyboard and trackpad make an iPad play like a Mac. The
  pointer needs the same scaling fix. Pointer lock (`prefersPointerLocked`) would give the
  DirectInput mouse delta for camera drags.

### Input coordinates

All touch and pointer schemes below need coordinates in game pixels: the window point × back
buffer ÷ window points, minus the letterbox offset. Fix this in the pump (user32.c), and turn
off SDL's synthetic mouse (`SDL_HINT_TOUCH_MOUSE_EVENTS=0`) once fingers are handled directly.
Otherwise a menu tap also sends a camera drag.

## Controls

FFXI is controller-native: it shipped on the PS2 and Xbox 360. The game's own gamepad mode can
do everything:
- **Movement and camera:** the left stick moves and the right stick turns the camera.
- **Menus and targets:** the D-pad and face buttons.
- **Macros:** holding a trigger opens the macro palette.

The host already reports every pad as an XInput Xbox 360 pad (`XI_GetState`, dinput.c), with
the game's mapping in ffxi.reg (`padmode000`, `padsin000`). So the most complete touch scheme is
a virtual gamepad, plus direct taps where touch beats a pad.

### Schemes

1. **Physical controller (works now, no work needed):**
   - Xbox, PlayStation, Backbone and other MFi controllers come in through GameController → SDL
     gamepad → XInput.
   - Check it in the simulator (Simulator > I/O > Game Controller) and on a device, then make it
     the recommended way to play.
   - Hide the touch overlay while a controller is connected.
2. **Virtual gamepad overlay (the main touch scheme):**
   - **Left:** a floating stick that appears where the thumb lands, for the left stick.
   - **Right:** dragging on empty screen turns the camera (right stick).
   - **Buttons:** A/B/X/Y, D-pad, LT/RT for the macro palette, Start and Back.
   - **Injection:** a virtual pad behind `pad()`, `input_xpad` and `input_pad_state` (input.c),
     as `web_gamepad` does in `sdl_web.c`. `dinput_xpad_hook` runs only when a real pad exists,
     so it cannot be used.
   - **Drawing:** `d3d8_set_overlay` in back-buffer pixels. ImGui's overlay owns that callback
     today, so the two must chain. The buttons should look like the game's own UI art, which
     `uidraw.c` can already draw.
3. **Direct touch (with scheme 2):**
   - **Gestures:**
     - a tap on the interface is a left click, which the PC client's menus and targeting accept
     - a two-finger tap is a right click (cancel)
     - a pinch is the wheel (zoom)
   - **On iPad**, tapping menus is quicker than the D-pad.
   - **Telling taps from camera drags:** last frame's interface grid (`user32_ui_hit`, d3d8.c)
     already says whether a point is over the interface.
4. **Keys on buttons (Android's `touch_controls`):** buttons that send numpad keys. Cheap, but
   no analog movement or camera, and every action is a separate button. Only a stopgap.

Recommendation: scheme 1 now; then 2 and 3 together as a "touch mode", on by default when no
controller or keyboard is connected; skip scheme 4.

### Text

`SDL_HINT_ENABLE_SCREEN_KEYBOARD=0` is set when the game window opens (user32.c, near line 994),
so the on-screen keyboard never shows in game.

`xi_chat_input_open()` (host/addons/hooks.c) knows when the chat line is open. To fix this:
- Poll it each frame from `present_hook` (host64.c).
- When the line opens, start text input with the screen keyboard. When it closes, stop it.
- Add a chat button to the overlay that opens the line.

Two more needs:
- **Shrink above the keyboard:** while the keyboard is up, the safe area should shrink so the
  chat line stays visible.
- **Fields above the keyboard on sign-in:** the same fix covers the sign-in screen's fields
  (docs/ios.md).

## Order of work

1. **Display:**
   - high pixel density
   - the C sizing policy and resize handling
   - Metal letterbox to the safe area
   - pointer scaling
   - phone and iPad defaults

   Everything else needs the coordinates to be right.
2. **Controllers:** check physical controllers, and hide the overlay while one is connected.
3. **Touch mode:** the virtual pad (input.c), the overlay (chained `d3d8_set_overlay`), finger
   handling and gestures.
4. **Text:** the chat keyboard, and keeping fields above the keyboard.
5. **Later:**
   - full-bleed 3D with the interface in the safe area
   - Split View and Stage Manager
   - iPad pointer lock
   - performance on device
