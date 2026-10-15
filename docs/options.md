# Options catalog

Every setting the client has today, grouped by what it changes rather than where it lives.
Sources, as the tables abbreviate them:

- **Menu** — in-game Config page (`host/modern.c`: Display, Modern, Menus, Addons)
- **fx** — key in `fx.txt` (`runtime/portable/gfx_fx.c`), also `FFXI_FX_<KEY>` in the environment; reloaded live
- **CLI** — `host64` flag (`host/host64.c`)
- **App** — app-bundle default key (`app_default`, read when the flag is absent)
- **Env** — `FFXI_*` environment variable

Defaults are the code's. `fx = 0` (the default) turns every scene effect off; the keys below it keep their values for when it is on.

---

## 1. Display

How big, where, and how fast the game is drawn.

| Setting | Menu | fx | CLI | App | Default / range |
|---|---|---|---|---|---|
| Window mode | Display › Window Mode | | | `FFXIWindowMode` | Windowed / Borderless / Full |
| Fullscreen space | | | | `FFXIFullscreenSpace` | |
| Resolution | Display › Resolution | | | `FFXIResolution` | list |
| UI scale | Display › UI Scale | | | `FFXIMenuResolution` | list |
| Background (render) resolution | Display › Background Resolution | | | `FFXIBackground` | 4096 / 6144 / 8192, next start |
| Frame rate | Modern › Frame Rate | | `--fps-divisor` | | 1 = 60 fps (default here), 2 = 30 as shipped |
| FPS counter | Display › FPS Counter | `fps` | | | 1 shown |
| GPU pick (Vulkan) | | | | | `FFXI_GPU` env |

## 2. Image quality

Whole-screen sharpness and color. Work with or without the effects unless noted.

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| Anti-aliasing | Display › Anti-Aliasing | `aa` | 0 off, 1 FXAA |
| Texture filtering | Display › Texture Filtering | `aniso` | 16 (1/4/8/16) |
| Anti-shimmer (temporal) | Modern › Anti-Shimmer | `filter` | 1 on |
| Temporal blend | — | `temporal` | 0.85 |
| Sharpening | Modern › Sharpening | `sharpen` | 0.3 (0–1) |
| Color grading | Modern › Color Grading | `grade` | 1.0 (0–1) |
| Saturation | — | `sat` | 1.12 |
| Contrast | — | `contrast` | 0.2 |

## 3. Lighting & shadows

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| **Master: Modern Effects** | Modern › Modern Effects | `fx` | 0 off |
| Per-pixel lighting | Modern › Per-Pixel Lighting | `light` | 0 off, 1 sun, 2 all lights |
| Sun shadow darkness | Modern › Sun Shadows | `sun` | 0.45 (0–1) |
| Shadow casters | Modern › Shadow Casters | `sun_casters` | 0 all, 1 characters, 2 world |
| Shadow edges | Modern › Shadow Edges | `sun_soft` | 0 hard, 0.03 soft |
| Shadow detail (near map) | Modern › Shadow Detail | `sun_detail` | 4096 (2048/4096/8192; 512–8192 in file) |
| Indoor shadows | Modern › Indoor Shadows | `moghouse` | 0 (0–1) |
| Game's own blob shadows | — (Config › Shadows follows) | `gameshadows` | 0 auto, 1 always, 2 never |
| Dawn/dusk long shadows | — | `sun_dusk` | 1 |
| Sun shadow reach | — | `sun_distance` | 100 |
| Near map reach | — | `sun_near` | 10 |
| Off-screen caster prime | — | `sun_prime` | 300 (0 never) |
| Sun direct strength | — | `sun_direct` | 0.5 |
| Sun face / min | — | `sun_face`, `sun_min` | 0 / 0 |
| Contact shadow | — | `shadow`, `shadow_length` | 0.3 / 0.6 |

## 4. Atmosphere

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| Fog density | Modern › Fog | `fog` | 0.004 (0–0.02) |
| Fog falloff / height / max | — | `fog_falloff`, `fog_height`, `fog_max` | 0.08 / 2.0 / 0.5 |
| Fog sun glow / phase | — | `fog_sun`, `fog_g` | 0.5 / 0.6 |
| God rays | Modern › God Rays | `rays` | 0.6 (0–1.5) |
| Ray decay / length | — | `rays_decay`, `rays_length` | 0.965 / 0.85 |
| Bloom | Modern › Bloom | `bloom` | 0.3 (0–2) |
| Bloom threshold | — | `threshold` | 0.75 |

## 5. Ambient occlusion

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| AO strength | Modern › Ambient Occlusion | `ao` | 0.8 (0–1.5) |
| AO quality | Modern › AO Quality | `ao_quality` | 0 low (6 taps), 1 med (10), 2 high (16) |
| AO radius | — | `radius` | 1.0 |

## 6. Water

None of these are in a menu yet.

| Setting | fx | Default |
|---|---|---|
| Modern water on/off | `water` | 1 |
| Refraction | `water_refract` | 0.015 |
| Clarity depth | `water_clarity` | 3.0 |
| Soft edge | `water_soft` | 0.15 |
| Foam amount / depth | `water_foam`, `water_foam_width` | 0.6 / 0.8 |
| Ripple tilt / density | `water_ripple`, `water_scale` | 0.25 / 0.6 |
| Sky reflection | `water_reflect` | 0.6 |
| Sun highlight | `water_spec` | 2.0 |

## 7. World detail & distance

| Setting | Menu | fx | CLI | App | Default / range |
|---|---|---|---|---|---|
| World draw distance | Modern › Draw Distance | `draw` | `--draw-distance k` or `WxC` | `FFXIDrawDistance` | 1 as game; 0.5–20 CLI, 1–6 menu |
| Character draw distance | Modern › Character Distance | `draw_entities` | (second half of `WxC`) | | 1–4 menu |
| Level of detail | — | `lod` | `--lod near\|game` | `FFXILod` | fx 0 = follow CLI, 1 near, 2 game |

## 8. Interface

| Setting | Menu | CLI | App | Default / range |
|---|---|---|---|---|
| Interface shape | Modern › Interface Shape | `--ui-aspect` | `FFXIUIAspect` | off; Full / 16:9 / 4:3 |
| Nameplate shape fix | — | `--nameplates fix\|off` | `FFXINameplates` | fix |
| Nameplate size | — | `--nameplate-scale s\|sxXsy` | `FFXINameplateScale` | 1 (0.25–4) |
| Hide Trust | Menus › Trust | | | Show |
| Hide Mounts | Menus › Mounts | | | Show |
| Hide Geomancy | Menus › Geomancy (Magic) | | | Show |
| Hide Master Levels | Menus › Master Levels | | | Show |
| Hide Unity | Menus › Unity | | | Show |
| Hide Job Points | Menus › Job Points | | | Show |
| Hide Alter Ego Points | Menus › Alter Ego Points | | | Show |

## 9. Textures & game data

| Setting | CLI | App / Env | Notes |
|---|---|---|---|
| Texture packs | `--textures <dir>` (×8) | `FFXI_BUNDLED_TEXTURES` | default `<data dir>/textures`, then bundled `assets/textures` (font, slot bar) |
| DAT overlays (XIPivot-style) | `--dats <dir>` (×8) | `FFXIDats` | first folder wins |
| Custom server items | `--cexi off\|items\|full` | `FFXICexi` | CatsEyeXI-style ids |

## 10. Addons

| Setting | Where | Notes |
|---|---|---|
| Per-addon on/off | Menu › Addons, `addons.cfg` | Ashita, Windower, xi |
| Addons master switch | `FFXI_ADDONS` env | |
| Addon test harness | `--addon-harness` | dev |

## 11. Server & sign-in

| Setting | CLI | App / Env |
|---|---|---|
| Server / lobby host | `--server` (`--lobby` old name) | `FFXIServer` |
| Launcher session | `--session`, `--auth` | |
| LandSandBoat sign-in | `--user`, `--pass`, `--otp`, `--login-token` | `FFXI_PASSWORD`, `FFXI_LSB_SESSION` |
| Ports | `--authport` 54231, `--dataport` 54230, `--viewport` 54001 | |
| Trust server cert | `--trust on\|off` | |
| Loader version | `--loader-version a.b.c` | |

## 12. Integrations

| Setting | Where |
|---|---|
| Discord presence | `FFXI_DISCORD`, `FFXI_DISCORD_APP_ID`, `FFXI_DISCORD_NAME` |
| In-process control (MCP) | `FFXI_CONTROL=1` |

## 13. Install & paths

| Setting | CLI | App / Env |
|---|---|---|
| Game folder | `--game` | `FFXIGameFolder` |
| Host data folder | `--data-dir` | |
| USER folder (settings, macros) | `--user-dir` | |
| Cache folder | | `FFXI_CACHE_DIR` |
| fx settings file | | `FFXI_FX_FILE` (default `fx.txt` in cache) |
| Registry files | `--reg`, `--reg-overlay`, `--reg-final` | |

## 14. Developer & diagnostics

Not for players.

| Area | Switches |
|---|---|
| Effects | `debug` (fx), `FFXI_FX_DEBUG` |
| Rendering | `FFXI_DRAWLOG`, `FFXI_TEXLOG`, `FFXI_TEXDUMP`, `FFXI_PROBE`, `FFXI_ASYNC_READBACK`, `FFXI_UI_NATIVE`, `FFXI_FPS` |
| GPU validation | `FFXI_VK_VALIDATION`, `FFXI_D3D12_DEBUG` |
| World | `FFXI_LODLOG`, `FFXI_WATERLOG`, `FFXI_PRIMELOG`, `FFXI_ZONE_CAPTURE` |
| Files / net | `FFXI_DATS_TRACE`, `FFXI_NET_TRACE` |
| Recompiler | `FFXI_RECOMP_TRACE`, `FFXI_RECOMP_MISSING` |
| Profiling | `FFXI_PROFILE`, `FFXI_SAMPLE` |

---

## Gaps and loose ends

- **Water has no menu rows** — 10 fx keys, only reachable through `fx.txt`.
- **Nameplates and LOD** have CLI/app keys but no menu row.
- **Modern page mixes kinds**: Frame Rate and Interface Shape are display/interface settings; Draw Distance is world detail, not an effect. Display holds FPS Counter and Texture Filtering, which are fx keys.
- **Three draw-distance ranges**: CLI 0.5–20, menu 1–6 (world) / 1–4 (characters).
- **`--aspect`** is described in the `host/host64.c` header comment but not parsed; looks stale.
- Ray tracing / GI (PR #30) and the orbit camera (`feat/orbit-camera`) aren't on main, so they aren't listed here.

## Menu tree

`*` = a row the menus don't have yet. `(key)` = what the row writes (`fx` key, `@` host key, or flag).
Advanced branches hold the tuning keys that only `fx.txt` reaches today.

```
Config
├── Display
│   ├── Window
│   │   ├── Window Mode ............ (@mode)
│   │   └── Resolution ............. (@res)
│   ├── Scaling
│   │   ├── UI Scale ............... (@menu)
│   │   ├── Interface Shape ........ (@ui)
│   │   └── Background Resolution .. (@bg)
│   └── Performance
│       ├── Frame Rate ............. (@fps)
│       └── FPS Counter ............ (fps)
├── Graphics
│   ├── Quality
│   │   ├── Anti-Aliasing .......... (aa)
│   │   ├── Texture Filtering ...... (aniso)
│   │   ├── Anti-Shimmer ........... (filter)
│   │   └── Sharpening ............. (sharpen)
│   ├── Color
│   │   ├── Color Grading .......... (grade)
│   │   ├── Saturation* ............ (sat)
│   │   └── Contrast* .............. (contrast)
│   └── World Detail
│       ├── Draw Distance .......... (draw)
│       ├── Character Distance ..... (draw_entities)
│       └── Level of Detail* ....... (lod)
├── Lighting
│   ├── Modern Effects ............. (fx)  master switch for Lighting + Atmosphere
│   ├── Per-Pixel Lighting ......... (light)
│   ├── Shadows
│   │   ├── Sun Shadows ............ (sun)
│   │   ├── Shadow Casters ......... (sun_casters)
│   │   ├── Shadow Edges ........... (sun_soft)
│   │   ├── Shadow Detail .......... (sun_detail)
│   │   ├── Indoor Shadows ......... (moghouse)
│   │   ├── Character Shadows* ..... (gameshadows)
│   │   └── Advanced*
│   │       ├── Dusk Shadows ....... (sun_dusk)
│   │       ├── Shadow Reach ....... (sun_distance)
│   │       ├── Near Reach ......... (sun_near)
│   │       ├── Off-Screen Casters . (sun_prime)
│   │       ├── Direct Light ....... (sun_direct)
│   │       ├── Face / Min ......... (sun_face, sun_min)
│   │       └── Contact Shadows .... (shadow, shadow_length)
│   └── Ambient Occlusion
│       ├── Strength ............... (ao)
│       ├── Quality ................ (ao_quality)
│       └── Radius* ................ (radius)
├── Atmosphere
│   ├── Fog
│   │   ├── Density ................ (fog)
│   │   └── Advanced* .............. (fog_falloff, fog_height, fog_max, fog_sun, fog_g)
│   ├── God Rays
│   │   ├── Strength ............... (rays)
│   │   └── Advanced* .............. (rays_decay, rays_length)
│   ├── Bloom
│   │   ├── Strength ............... (bloom)
│   │   └── Threshold* ............. (threshold)
│   └── Water*
│       ├── Modern Water ........... (water)
│       ├── Reflection ............. (water_reflect)
│       ├── Sun Highlight .......... (water_spec)
│       ├── Foam ................... (water_foam)
│       └── Advanced ............... (water_refract, water_clarity, water_soft,
│                                     water_foam_width, water_ripple, water_scale)
├── Interface
│   ├── Nameplates*
│   │   ├── Shape .................. (--nameplates)
│   │   └── Size ................... (--nameplate-scale)
│   └── Menus
│       ├── Content
│       │   ├── Trust .............. (@hide0)
│       │   ├── Mounts ............. (@hide7)
│       │   └── Geomancy ........... (@hide1)
│       └── Status Pages
│           ├── Master Levels ...... (@hide3)
│           ├── Unity .............. (@hide4)
│           ├── Job Points ......... (@hide5)
│           └── Alter Ego Points ... (@hide6)
└── Addons
    ├── xi
    ├── Ashita
    └── Windower
```

Kept out of the in-game tree. These belong to the launcher or to developers, and are set before the game starts:

```
Launcher
├── Server & Sign-in ... (--server, --session/--auth, --user/--pass/--otp, ports, --trust)
├── Game Data .......... (--textures, --dats, --cexi)
├── Integrations ....... (Discord)
└── Paths .............. (--game, --data-dir, --user-dir, cache, fx file, --reg*)
Developer .............. (env only, section 14)
```
