# Options catalog

Every setting the client has, grouped by what it changes rather than where it lives. In game, the
player-facing ones are under **Config › Advanced** (`host/modern.c`), in nine pages: Display,
Graphics, Shadows, Lighting, Atmosphere, Water, Legacy, Interface and Addons. The tree is at the end.

Sources, as the tables abbreviate them:

- **Menu**: its row under Config › Advanced, as *page › row* (a group heading in brackets)
- **fx**: its key in `fx.txt` (`runtime/portable/gfx_fx.c`), also `FFXI_FX_<KEY>` in the environment; read again while the game runs
- **CLI**: a `host64` flag (`host/host64.c`)
- **App**: an app-bundle default (`app_default`, read when the flag is absent)
- **Env**: an `FFXI_*` environment variable

Defaults are the code's. `fx = 0` (the default) turns every scene effect off; the keys below it keep
their values for when it is on. On Android the rows set through scene effects are not shown, nor
the Lighting and Atmosphere pages.

---

## 1. Display

| Setting | Menu | fx | CLI | App | Default / range |
|---|---|---|---|---|---|
| Window mode | Display › [Window] Window Mode | | | `FFXIWindowMode` | Windowed / Borderless / Full |
| Fullscreen space | | | | `FFXIFullscreenSpace` | |
| Resolution | Display › [Window] Resolution | | | `FFXIResolution` | list |
| UI scale | Display › [Scaling] UI Scale | | | `FFXIMenuResolution` | list |
| Interface shape | Display › [Scaling] Interface Shape | | `--ui-aspect` | `FFXIUIAspect` | off; Full / 16:9 / 4:3 |
| Background (render) resolution | Display › [Scaling] Background Resolution | | | `FFXIBackground` | 2048 / 4096 / 6144 / 8192, next start |
| Frame rate | Display › [Performance] Frame Rate | | `--fps-divisor`, `--fps-limit` | | 30 (as shipped), 60 (default), 120, 144 or Uncapped (the last three only in game builds with frame sites in `meta/builds.json`, from `tools/frame_sites.py`; in others `--fps-divisor 0` is the game's own uncapped pacing, the world as fast as the frames). 120 and 144 run uncapped held to that rate (`--fps-limit n`; `FFXI_FPS_LIMIT=n` overrides); uncapped keeps the world at its speed. `FFXI_VSYNC=0` lets it past the display. The menus before the game keep the pace they have at 30 fps |
| FPS counter | Display › [Performance] FPS Counter | `fps` | | | 1 shown |
| GPU pick (Vulkan) | | | | | `FFXI_GPU` env |

## 2. Image quality and color

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| Anti-aliasing | Graphics › [Quality] Anti-Aliasing | `aa` | 0 off, 1 FXAA |
| Texture filtering | Graphics › [Quality] Texture Filtering | `aniso` | 16 (1/4/8/16) |
| Overall quality | Graphics › [Quality] Overall Quality | | Low / Medium / Max set the scene renderer and post effects (`scene`, `scene_hdr`, `tonemap`, `scene_shadows`, `shadow_softness`, `shadow_soft_q`, `scene_shadow_size`, `scene_lamps`, `scene_lamp_shadows`, `scene_gi`, `scene_gi_q`, `scene_ao`, `scene_ao_q`, `scene_spec`, `post_shafts`, `post_bloom`, `post_dof`) and turn the older effects' lighting off (`light`, `sun`, `ao`, `gi`, `shadow`, `rays`, `bloom`). Low: lit per vertex, nothing else; Medium: per pixel, HDR, soft shadows (Low taps), lamps (2 shadowed), occlusion low, highlights, shafts, bloom; Max: as tools/replay/showcase-fx.txt. Custom when they are none of these |
| Renderer | Graphics › [Quality] Renderer | `scene` | Per Pixel 2, Per Vertex 1, Legacy 0 (the game's draws with the older effects); the modern ones turn the older lighting keys off, Legacy puts them back to their defaults |
| Anti-shimmer (temporal) | | `filter` | 1 on; fixed (fx.txt's line ignored) |
| Temporal blend | | `temporal` | 0.85 |
| Sharpening | Graphics › [Quality] Sharpening | `sharpen` | 0.3 (0–1) |
| Color grading | Graphics › [Color] Color Grading | `grade` | 1.0 (0–1) |
| Saturation | Graphics › [Color] Saturation | `sat` | 1.12 (0.5–1.5) |
| Contrast | Graphics › [Color] Contrast | `contrast` | 0.2 (0–0.5) |

## 3. Lighting and shadows

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| Modern effects (master) | | `fx` | 1 on; fixed (fx.txt's line ignored; `FFXI_FX=0` still turns them off) |
| Per-pixel lighting | Legacy › [Lighting] Modern Lighting | `light` | 2 all lights (0 off, 1 sun) |
| Bounce light | Legacy › [Lighting] Bounce Light | `gi` | 0 off (0–3) |
| Ray tracing | | `rt` | 0 off; fixed (fx.txt's line ignored; `FFXI_FX_RT=1` turns it on) |
| Sun shadow darkness | Legacy › [Shadows] Sun Shadows | `sun` | 0.45 (0–1) |
| Shadow casters | Legacy › [Shadows] Shadow Casters | `sun_casters` | 0 all, 1 characters, 2 world |
| Shadow edges | Legacy › [Shadows] Shadow Edges | `sun_soft` | 0 hard, 0.03 soft |
| Shadow detail (near map) | Legacy › [Shadows] Shadow Detail | `sun_detail` | 4096 (2048/4096/8192; 512–8192 in the file) |
| Indoor shadows | Legacy › [Shadows] Indoor Shadows | `moghouse` | 0 (0–1) |
| The game's own character shadows | | `gameshadows` | 0 auto; fixed (fx.txt's line ignored) |
| Dawn/dusk long shadows | | `sun_dusk` | 1 |
| Sun shadow reach | | `sun_distance` | 100 |
| Near map reach | | `sun_near` | 10 |
| Off-screen caster prime | | `sun_prime` | 300 (0 never) |
| Sun direct strength | | `sun_direct` | 0.5 |
| Sun face / min | | `sun_face`, `sun_min` | 0 / 0 |
| Contact shadow | | `shadow`, `shadow_length`, `contact_steps` | 0.3 / 0.6 / 0 (the steps a frame, 1–16; 0 follows `ao_quality`: 8, 12, 16) |

## 4. Ambient occlusion

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| Strength | Legacy › [Ambient Occlusion] Strength | `ao` | 0.8 (0–1.5) |
| Quality | Legacy › [Ambient Occlusion] Quality | `ao_quality` | 1 medium (10 taps); 0 low (6), 2 high (16); the contact shadows' steps follow it (12; 8, 16) |
| Radius | Legacy › [Ambient Occlusion] Radius | `radius` | 1.0 (0.25–3) |

## 5. Atmosphere

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| Fog density | Legacy › [Glow] Haze | `fog` | 0.004 (0–0.02) |
| Fog falloff / height / max | | `fog_falloff`, `fog_height`, `fog_max` | 0.08 / 2.0 / 0.5 |
| Fog sun glow / phase | | `fog_sun`, `fog_g` | 0.5 / 0.6 |
| God rays | Legacy › [Glow] God Rays | `rays` | 0.6 (0–1.5) |
| Ray decay / length | | `rays_decay`, `rays_length` | 0.965 / 0.85 |
| Bloom | Legacy › [Glow] Bloom | `bloom` | 0.3 (0–2) |
| Bloom threshold | Legacy › [Glow] Bloom Threshold | `threshold` | 0.75 (0.3–1) |
| Post bloom (lamps, spells, sun; with or without `FFXI_FX`, Metal and Vulkan) | Atmosphere › [Bloom] Strength | `post_bloom` | 0 off (1 tuned) |
| Post bloom sky threshold / knee / spread | | `bloom_threshold`, `bloom_knee`, `bloom_radius` | 0.95 / 0.05 / 1 |
| Post bloom emissive weight: effects and lamp glows / sun, moon, sky | | `bloom_emissive_fx`, `bloom_emissive_lights` | 1.2 / 1.5 |
| Depth of field (Metal and Vulkan) | Atmosphere › [Depth of Field] Strength | `post_dof` | 0 off (1 tuned) |
| DOF focus (0 follows the player) / sharp range / ramp / near blur | | `dof_focus`, `dof_range`, `dof_ramp`, `dof_near` | 0 / 50 / 150 / 0 (0–1) |
| Sun shafts (scene renderer with `scene_shadows`, Metal and Vulkan) | Atmosphere › [Sun Shafts] Strength | `post_shafts` | 0 off (1 tuned) |
| Shafts haze density / quality (2 a third of the scene, 16 steps; 1 a fifth, 10) / forward lobe / reach / over frames | | `shafts_density`, `shafts_q`, `shafts_g`, `shafts_dist`, `shafts_temporal` | 1 / 2 / 0.6 / 120 / 0.9 |
| Specular highlights (scene 2 with `scene_shadows`, Metal and Vulkan): strength / lamps' share / wetness / anti-aliasing width | | `scene_spec`, `scene_spec_lamps`, `scene_spec_wet`, `scene_spec_aa` | 0 off (1 tuned) / 1 / 0 (0–1) / 1 |
| HDR scene target (Metal and Vulkan): on / tonemap (0 neutral, 1 highlight shoulder, 2 ACES) / exposure / white point / knee | | `scene_hdr`, `tonemap`, `exposure`, `tone_white`, `tone_knee` | 0 / 0 / 1 / 4 / 0.8 |
| Lamps (scene 2, Metal and Vulkan): every lit lamp per pixel, faded at its range / reach / glow sprite and size | | `scene_lamps`, `scene_lamp_far`, `scene_lamp_glow`, `scene_lamp_glow_size` | 0 off (1: the game's strength) / 120 / 0 / 0.6 |
| Lamps: on/off fade (seconds) / fire flicker / character shadows from the nearest lamps (0-4) and cube size / shadow debug | | `scene_lamp_fade`, `scene_lamp_flicker`, `scene_lamp_shadows`, `scene_lamp_shadow_size`, `scene_lamp_debug` | 1.5 / 0.06 / 4 / 256 / 0 |
| Lamps: the zone casts in those lamps' cubes too (what the game drew that frame; drawn again when the lamps change or every 600 frames) | Shadows › [Lamps] Walls Cast | `scene_lamp_zone` | 1 on (0: characters only) |
| Night (scene renderer, eased through dusk and dawn by the sun's height): the moon's shadows' strength as a share of the sun's / their penumbra's width / the sun, moon and ambient light at midnight / a cool grey shift on them; lamps untouched | | `moon_shadow`, `moon_soft`, `night_exposure`, `night_tint` | 0.7 / 1.5 / 0.6 / 0.35 |
| Lamps fill the sun's and moon's shadows: gone where lamp light is at least 1/n of the sun's or moon's | Shadows › [Night] Lamp Fill | `shadow_fill` | 0.5 (0: never) |
| Soft shadows (scene renderer: sun and moon cascades, characters' layers, lamp cubes), hard at contact and softer away from the caster: how soft (0: the fixed filter) / the sun's angular size in degrees (tripled toward the horizon) / the moon's / a lamp's in units / 1 Low, 2 High taps | | `shadow_softness`, `sun_size`, `moon_size`, `lamp_size`, `shadow_soft_q` | 1 / 2 / 3 / 0.3 / 2 |
| Modern fog (scene renderer) in place of the game's fade to grey: on / density next to the game's fog end (1: 4% of a point's light left there) / thinning with height per unit risen / lamp halos and how far above the game's light their lanterns hang / debug (1 how much it takes, 2 what it adds) | Atmosphere › [Fog] Modern Fog, Density, Ground Fog, Lamp Halos | `fog_modern`, `fog_density`, `fog_thin`, `fog_lamps`, `fog_lamp_lift`, `fog_debug` | 0 / 1 / 0.03 / 1.5 / 1.5 / 0 |
| Modern fog as one volume with the sun shafts (the shafts' march: shadowed sun, the air's own light, lamp halos, ground mist; over frames) / drifting mist's strength (a layer some 1.5 units deep over level ground, wherever it lies; thicker in fog, rain and snow, thinner and faster in wind), size (units), drift (units a second) | Atmosphere › [Fog] Volume, Drifting Mist | `fog_volume`, `fog_ground`, `fog_ground_size`, `fog_drift` | 1 / 0.5 / 8 / 0.6 |
| Testing: the sun held low ahead of the camera (degrees up, degrees turned); the shafts alone | | `scene_sun_test`, `scene_sun_test_turn`, `shafts_debug` | 0 / 0 / 0 |

## 6. Water

| Setting | Menu | fx | Default / range |
|---|---|---|---|
| Modern water on/off | Atmosphere › [Water] Modern Water | `water` | 1 |
| Sky reflection | Atmosphere › [Water] Reflection | `water_reflect` | 0.6 (0–1) |
| Sun highlight | Atmosphere › [Water] Sun Highlight | `water_spec` | 2.0 (0–4) |
| Foam amount | Atmosphere › [Water] Foam | `water_foam` | 0.6 (0–1) |
| Foam depth | | `water_foam_width` | 0.8 |
| Refraction | | `water_refract` | 0.015 |
| Clarity depth | | `water_clarity` | 3.0 |
| Soft edge | | `water_soft` | 0.15 |
| Ripple tilt / density | | `water_ripple`, `water_scale` | 0.25 / 0.6 |

## 7. World detail and distance

| Setting | Menu | fx | CLI | App | Default / range |
|---|---|---|---|---|---|
| World draw distance | Graphics › [World Detail] Draw Distance | `draw` | `--draw-distance k` or `WxC` | `FFXIDrawDistance` | 1 as the game; 0.5–20 CLI, 1–6 menu |
| Character draw distance | Graphics › [World Detail] Character Distance | `draw_entities` | (second half of `WxC`) | | 1–4 menu |
| Level of detail | | `lod` | `--lod near\|game` | `FFXILod` | the command line's (fx.txt's line ignored) |
| Occlusion check | | | | | off (`FFXI_PROBE=gpu`: exact) |

## 8. Interface

| Setting | Menu | CLI | App | Default / range |
|---|---|---|---|---|
| Nameplate shape | Interface › [Nameplates] Shape | `--nameplates fix\|off` | `FFXINameplates` | Fixed (4:3) / Stretched; modern.cfg `nameplates` |
| Nameplate size | Interface › [Nameplates] Size | `--nameplate-scale s\|sxXsy` | `FFXINameplateScale` | 1 (0.5–2 menu, 0.25–4 CLI); modern.cfg `nameplate_scale` |
| Hide Trust | Interface › [Menus: Content] Trust | | | Show |
| Hide Mounts | Interface › [Menus: Content] Mounts | | | Show |
| Hide Geomancy | Interface › [Menus: Content] Geomancy | | | Show |
| Hide Master Levels | Interface › [Menus: Status Pages] Master Levels | | | Show |
| Hide Unity | Interface › [Menus: Status Pages] Unity | | | Show |
| Hide Job Points | Interface › [Menus: Status Pages] Job Points | | | Show |
| Hide Alter Ego Points | Interface › [Menus: Status Pages] Alter Ego Points | | | Show |

## 9. Textures & game data

| Setting | CLI | App / Env | Notes |
|---|---|---|---|
| Texture packs | `--textures <dir>` (×8) | `FFXI_BUNDLED_TEXTURES` | default `<data dir>/textures`, then bundled `assets/textures` (font, slot bar) |
| DAT overlays (XIPivot-style) | `--dats <dir>` (×8) | `FFXIDats` | first folder wins |
| Custom server items | `--cexi off\|items\|full` | `FFXICexi` | CatsEyeXI-style ids |

## 10. Addons

| Setting | Where | Notes |
|---|---|---|
| Per-addon on/off | Advanced › Addons, `addons.cfg` | Ashita, Windower, xi |
| Addons master switch | `FFXI_ADDONS` env | |
| Slow-call log threshold | `FFXI_ADDON_SLOW` env (ms), `/addon prof slow <ms>` | default 100; 0 off |
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
| World | `FFXI_LODLOG`, `FFXI_WATERLOG`, `FFXI_PRIMELOG` |
| Files / net | `FFXI_DATS_TRACE`, `FFXI_NET_TRACE` |
| Recompiler | `FFXI_RECOMP_TRACE`, `FFXI_RECOMP_MISSING` |
| Profiling | `FFXI_PROFILE`, `FFXI_SAMPLE` |

---

## Not in a menu yet

- The tuning keys above with no Menu entry (fog, rays, sun shadows, water): only `fx.txt` reaches
  them. The plan is a tuning page linked from each group.
- **Three draw-distance ranges**: CLI 0.5–20, menu 1–6 (world) / 1–4 (characters).
- **`--aspect`** is described in the `host/host64.c` header comment but not parsed; it looks stale.

## Menu tree

```
Config
└── Advanced
    ├── Display
    │   ├── [Window] ........ Window Mode, Resolution
    │   ├── [Scaling] ....... UI Scale, Interface Shape, Background Resolution
    │   └── [Performance] ... Frame Rate, FPS Counter
    ├── Graphics
    │   ├── [Quality] ....... Overall Quality, Renderer, Anti-Aliasing, Texture Filtering, Sharpening
    │   ├── [Color] ......... High Dynamic Range, Tone Mapping, Exposure, Color Grading, Saturation,
    │   │                     Contrast
    │   └── [World Detail] .. Draw Distance, Character Distance
    ├── Shadows
    │   ├── [Sun and Moon] .. Shadows, Softness, Sun Size, Soft Quality, Detail
    │   ├── [Night] ......... Moon Shadows, Lamp Fill
    │   └── [Lamps] ......... Lamp Shadows, Walls Cast
    ├── Lighting
    │   ├── [Lamps] ......... Lamp Light
    │   ├── [Night] ......... Night Brightness, Night Tint
    │   ├── [Bounce Light] .. Strength, Quality
    │   ├── [Ambient Occlusion] Strength, Quality, Radius
    │   └── [Highlights] .... Strength, Wetness
    ├── Atmosphere
    │   ├── [Fog] ........... Modern Fog, Density, Ground Fog, Lamp Halos, Volume, Drifting Mist
    │   ├── [Sun Shafts] .... Strength, Quality
    │   ├── [Bloom] ......... Strength, Effect Glow, Lamp Glow
    │   └── [Depth of Field]  Strength, Sharp Distance
    ├── Water
    │   └── [Water] ......... Modern Water, Reflection, Sun Highlight, Foam, Surface Lift
    ├── Legacy (the older effects, Renderer: Legacy)
    │   ├── [Lighting] ...... Modern Lighting, Bounce Light
    │   ├── [Shadows] ....... Sun Shadows, Shadow Casters, Shadow Edges, Shadow Detail, Indoor Shadows
    │   ├── [Ambient Occlusion] Strength, Quality, Radius
    │   └── [Glow] .......... God Rays, Bloom, Bloom Threshold, Haze
    ├── Interface
    │   ├── [Nameplates] .... Shape, Size
    │   ├── [Menus: Content]  Trust, Mounts, Geomancy
    │   └── [Menus: Status Pages] Master Levels, Unity, Job Points, Alter Ego Points
    └── Addons .............. one row an installed addon (xi, Ashita, Windower)
```

Set before the game starts, so not in the game's menus: server and sign-in, game data (texture
packs, DAT overlays, `--cexi`), integrations (Discord) and paths (sections 9, 11, 12 and 13), and
the developer switches (section 14).
