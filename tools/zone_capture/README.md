# Zone capture, first slice

This diagnostic path exports a **submitted frame**, not a complete zone or ready-to-import Unreal asset. It records source geometry, original D3D8 state, shader inputs/tokens, model-load provenance and ordered target events. The existing renderer remains active.

## Capture a real zone

Build the host normally with these changes. Before starting it, set `FFXI_ZONE_CAPTURE` to an absolute output filename whose parent directory already exists:

```sh
export FFXI_ZONE_CAPTURE=/absolute/captures/south-000.jsonl
```

Once the client has finished zoning and the desired view is stable, write the zone label to the trigger file:

```sh
printf '107\n' > /absolute/captures/south-000.jsonl.go
```

The next Present opens the request; draws until the following Present are captured. Wait for the footer `{"type":"end",...,"complete":true}` before consuming it. The trigger is removed when accepted. To capture another view, rename/move both outputs before retriggering, or restart with a different output path. Existing output files are overwritten on each accepted request. Label Valkurm Dunes with `103`; South Gustaberg is `107`. The label is supplied by the operator, **not verified against the DLL's current zone**.

The C API `d3d8_zone_capture(path, zone)` starts immediately and returns zero on failure or while another capture is active. It must be called on the guest's execution thread with the guest lock held. A direct API call partway through a frame captures only the remainder. The environment trigger is preferable for a full between-Presents window. Payloads are serialized synchronously before guest memory can change; capture stalls are expected and cannot be used for performance measurements.

There are two outputs: the requested JSONL file and `<filename>.bin`. The reader rejects missing/failed footers, inconsistent draw counts, reordered events, mismatched payload lengths and out-of-range references. Captures stop accepting events after 10000 draws or 256 MiB of binary payload; the footer then reports `complete:false`. An unpresented/crashed frame has no success footer.

## Export and inspect

```sh
python3 tools/zone_capture.py /absolute/captures/south-000.jsonl \
  --out build/zone-capture/south-000
```

- `scene.json`: decoded input registers, triangle topology, original state/texture/shader payload references, resource provenance and transforms.
- `scene.bin`: exact captured payload bytes referenced by the scene.
- `region.obj`: fixed-function triangle geometry in original world coordinates/source units, intended for inspection. No assumed Unreal axes/centimeter conversion, generated collision, texture shading, or layer baking is applied. Programmable and pretransformed vertices remain in the scene data and are excluded from this OBJ.
- `layers.json`: original ordered blend/depth/stencil/bias candidates, texture identities, coverage counters and target events. It does **not** infer which coplanar layers share a receiver or prove an operation can be replaced by a decal.

All original attributes are retained: normal, color, all used UV input registers, blend weights/indices and secondary positions if present. Register names/types follow `gfx.h`; packed colors remain integer D3DCOLOR values. Triangle strips alternate winding and fans are expanded; indices are rebased to the captured vertex window. Nontriangle primitives are preserved in scene metadata but do not produce OBJ faces.

## Two-zone replay bootstrap

Record a real visit to each zone with the existing xireplay recorder, at a surveyed position that covers the desired surface layers. The repository contains no recordings for these zones. Generate eight fixed-heading replay cases:

```sh
python3 tools/zone_capture_suite.py \
  --south /absolute/recordings/south.jsonl \
  --valkurm /absolute/recordings/valkurm.jsonl \
  --out build/modern-zone-suite.txt
python3 tools/replay.py shots build/modern-zone-suite.txt --game /absolute/FINAL_FANTASY_XI
```

The generator verifies recording zone metadata and preserves recorded arrival positions. It uses existing `--turn` and `--hold` replay transforms: four headings per zone, no invented coordinates or cross-zone substitution. `shots` uses the existing screenshot capture; trigger diagnostic captures separately at the settled views. Time/weather remain those in each recording. Separate dawn/night/weather recordings are needed for those cases.

These are repeatable player-heading cases, **not yet deterministic camera/input playback**. Keep camera mode and input fixed and compare captured view/projection transforms across repeats. A full camera traversal and coverage survey still requires real-client instrumentation/recording. Do not certify a whole zone from eight frames.

## Schema v1

JSONL begins with `header`, followed by ordered `draw`, `clear`, and `copy_attempt` events, ending in `end`. Binary references are `{offset,size}` byte ranges in the sidecar; numeric arrays are little-endian. No host pointers or raw C struct layouts are serialized.

Draw arrays:

| Field | Layout |
| --- | --- |
| `render_states` | 256 uint32 D3DRS values |
| `texture_stages` | 8 x 32 uint32 D3DTSS values, including sampler state |
| `transforms` | 280 x 16 float32 matrices; view=2, projection=3, world=24 |
| `viewport` | six uint32 D3DVIEWPORT8 fields (last two hold float bits) |
| `material` | 17 uint32 words of D3DMATERIAL8 |
| `lights` | 64 x 27 uint32 words: D3DLIGHT8 followed by enabled |
| `vs_constants`, `ps_constants` | 256 x 4 and 8 x 4 float32 |
| `clip_planes` | 6 x 4 float32 |
| shader fields | uint32 declaration/instruction tokens |
| `layout` | per input register: used, stream, D3DVSDT, byte offset |
| `streams` | stride, resource metadata, exact guest vertex window |
| `indices` | original 16/32-bit indices; add `index_rebase` before using captured vertices |

Objects get monotonically assigned session-local IDs independent of COM-object slot reuse. `origin` stores a model-load serial, transient resource address, model FourCC and up to four parent FourCCs (nearest first), captured at creation. The host's existing build-specific model hooks now retain this context for all models they bracket. Origins outside those hooks remain zero and are counted explicitly. No stable DAT path, persistent instance identity or authoritative zone epoch has been recovered yet.

Texture surface payloads contain original guest texels in the source D3DFORMAT, including available mip/cube surfaces. Repeated unchanged surface data is deduplicated within a capture by a 64-bit FNV hash. `null` means unavailable or GPU-owned, **not black**. Render targets may depend on frames before capture; target draws/clear/copy attempts expose only this frame's operations. `copy_attempt` records a requested copy before the adapter's scaled-target/rectangle checks and does not establish successful execution.

`translated` includes the adapter's blend/depth/fog selections alongside original states. Captures precede scene post effects and UI squeezing; texture-pack inset changes may already have happened upstream. Record build and enhancement settings with every reference run. The header carries image timestamp/size, not a cryptographic client-build fingerprint. The model hook remains build-specific and is not a complete DAT parser.

## Validation

```sh
python3 tools/build_posix.py gfxtest
python3 tests/zone_capture_test.py -v
```

The offscreen C test emits `build/d3d8test/zone.jsonl` and its sidecar through the real COM adapter. It covers dynamic/static buffers, indexed windows, declared multistream geometry, textures, clears/copies and source provenance. Python checks decoded bytes/topology, export, ordered reports, invalid captures and the two-zone suite generator. On Windows use the existing `tools/build.py gfxtest` target before the same Python test.

No live South Gustaberg/Valkurm capture or Unreal import is claimed by these synthetic tests. Asset extraction now uses the [source-first C resource exporter](../../docs/zone-resource-export.md), which has exported both installed zones with source identities and placements. Use this capture path to verify runtime blend classes, ordering and inherited depth state against those assets.
