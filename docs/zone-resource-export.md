# Source-first zone export

The primary asset path now starts at the game's **file/resource representation**, before draw submission or visibility culling. `host/zone_resource.c` provides bounded offline C counterparts of the relevant translated loader operations. `tools/export_zone.py` resolves the installed DAT, calls that C reader, and assembles a versioned scene manifest. It does not launch the client, require a server, or execute the gameplay DLL. Existing draw capture remains a renderer-validation tool.

## Use

The current reader profile is pinned to the installed **2026-09-03** client and its prepared unpacked image/translated C. Run the existing prepare/build workflow first if those generated files are absent. The tool verifies the retail client hash against `meta/builds.json` and checks unpacked PE metadata. Other client builds are rejected until their source offsets are reviewed.

```sh
python3 tools/export_zone.py \
  --game '/absolute/FINAL FANTASY XI' \
  --zone 107 --out build/zones/south-gustaberg --inspect-obj
python3 tools/export_zone.py \
  --game '/absolute/FINAL FANTASY XI' \
  --zone 103 --out build/zones/valkurm-dunes --inspect-obj
```

The Python CLI builds the C reader and existing `host/datui.c` image decoder as a small shared library with clang; `XI_CC` can override the compiler. This workflow was validated on macOS. `--library` accepts a separately compiled shared library. No new native runtime or engine dependency is introduced into the game host.

Outputs:

- `zone.json`: file/resource hierarchy; models and ordered submeshes; placements; near/middle/far model references; source transforms, LOD distances, flags and light references; original spatial-tree/group indices; texture identities and render-rule evidence.
- `resources.bin`: every resource payload, including decoded MZB/MMB resources and all other unchanged payloads. Unknown resource types remain available rather than being discarded.
- `triangles.bin`: uint32 triangle lists, with strips expanded using alternating winding and degenerate faces removed. Original uint16 indices remain in `resources.bin`.
- `source.dat`: exact original DAT, with its SHA-256 in the manifest.
- `textures/*.tga`: inspection images decoded by the existing C image reader. Original encoded textures are also retained. The image reader doubles legacy alpha; these images are explicitly marked as previews, not a finalized modern albedo/alpha policy.
- Optional `zone.obj`/`zone.mtl`: expanded near-LOD placement geometry with texture references, in source coordinates, at animation phase zero. This is an inspection view. It does not emulate ordered transparency, vertex-color lighting, dynamic water, runtime weather or depth bias. UE must import the manifest's reusable meshes/instances, rather than using this flattened OBJ as the asset architecture.

IDs are `dat:<file-id>/chunk:<file-offset>/part:<index>/batch:<index>` and analogous placement IDs. They are deterministic within a source version and independent of guest addresses, GPU handles, frame order or camera position. The manifest's source hash scopes them across game patches. Repeated names are retained as distinct resources; ambiguity is reported instead of silently selecting an arbitrary object.

## Traced boundary

Authoritative local translated C is in `generated/all/funcs_026.c`; original addresses below identify source operations without checking in game-derived bytes.

| Source boundary | What the reader retains |
| --- | --- |
| `f_10176ed0`, call to `f_101792f0` | Model resource FourCC, parent context, encoded chunk size and payload passed to model construction. |
| `f_1016fcd0`, `1016fcf7–1016fd45` | First-stage MMB decode; 24-bit length, version and key-index rules. |
| `f_10170e80` | Conditional second-stage MMB 8-byte swaps, including original key-update timing. |
| `f_1016fcd0`, `1016fe8e`, `101700dd–1017029c` | Model part table, source part/submesh order, exact raw flags, texture IDs, source vertex windows, topology and indices. Empty part slots are skipped without terminating the table. |
| `f_1016fcd0`, `1016fdb0–1016fe32` | Type-1 triangle strips versus other source triangle lists; 36-byte ordinary and 48-byte animated vertices. Animation deltas remain in the source attributes. |
| `f_10179430`, `10179445–101794e1` | MZB block inversion, with **length minus eight** as used in the actual translated client. |
| `f_10179430`, `1017986e–1017988a`, `1017a23f` | Placement name decoding and 100-byte source placement records for the supported version. |
| `f_1017afe0` | Near/middle/far suffix lookup and fallback behavior; `_` alpha-wrap and `#` clamp modes. |
| `f_10179430`, calls to `f_1017c820`/`f_1017af30` | Spatial tree and group indices, retained as file indices rather than rewritten guest pointers. |
| `f_1017b510`, `1017b7bf–1017b80b` | Default alpha comparison GREATER/reference 0x60 and SRCALPHA/INVSRCALPHA blend factors. |
| `f_1017c9c0`, `1017dad1–1017db60` | Transparent submeshes enable blending, disable depth writes and select the transparent bias global; opaque submeshes reverse those selections. |
| `f_101781d0` | Runtime bias initialization: opaque zero, transparent eight or one depending on the device branch. Numeric D3D bias still requires translation to UE's depth convention. |

The reader obtains the three decode tables from the user's matching unpacked client at the source-referenced addresses. Those table bytes and exported game assets are not checked in. Existing `.gitignore` excludes `build/` and `generated/`.

The readable XIClient reconstruction under the local FFXI-Signet research checkout helped navigate the resource classes and matrix construction. Current translated C was checked for the decode rules, record stride, part traversal, LOD fallback and blend/depth transitions; reconstructed comments are not treated as proof of unimplemented behavior.

## Verified exports

From the locally installed retail data on 2026-10-08:

| Zone | Models | Submeshes | Placements | Source triangles across all models/LODs | Textures | Transparent submeshes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| South Gustaberg | 137 | 403 | 2,602 | 43,649 | 53 | 143 |
| Valkurm Dunes | 222 | 632 | 3,449 | 70,389 | 74 | 239 |

Both exports resolve every placed model/LOD and every **nonempty** texture reference within the zone DAT. Blank texture IDs are valid untextured submeshes. All textures decode to inspection images. The files contain 1,653 and 1,606 spatial nodes respectively; all exported triangle and spatial placement references were bounds checked.

File lookup uses installed FTABLE/VTABLE entries and expansion overrides, rather than assuming `ROM/1/<zone>.DAT`. In this installation South Gustaberg resolves DAT 207 to `ROM/0/124.DAT`; Valkurm resolves DAT 203 to `ROM/0/102.DAT`. Beta-install paths previously mentioned in other research are not valid substitutes for these lookups.

## Scope and next integration

This implements source-level extraction of both test zones' model/layout resources, not a UE renderer. The manifest preserves raw fields even when their modern rendering interpretation is unfinished. Bits 30 and 28, runtime depth comparison/stencil state, per-area lighting, weather, animated effects and resource types outside MZB/MMB are not silently assigned guessed modern behavior. Collision resources are preserved but not installed as replacement gameplay collision.

No camera selection or render-frame capture is involved in obtaining the static models/placements. Additional source work is still required to evaluate dynamic state, external resource dependencies in other zones, and the complete ordered rendering schedule. Original submesh order is retained; it is not claimed to be the entire frame's compositing order.

The next consumer is a UE editor importer: reusable UStaticMesh assets from model/submesh buffers, persistent placement components from layout entries, material recipes from the retained state/flags, and .umap assets. Coordinate/unit conversion, normal conversion, baked-light treatment and ordered depth-layer equivalence remain explicit gates. Lumen should be enabled only after those surfaces are represented correctly in UE materials and scene depth.

## Checks

```sh
python3 tests/zone_resource_test.py -v
clang -Wall -Wextra -Werror -std=c11 -fsyntax-only host/zone_resource.c
```

Six synthetic tests cover malformed sizes/counts, sparse part tables, retained flags/attributes, decode ownership, placement names, triangle-strip winding, LOD fallback, transforms and resource hierarchy. Real-data runs validate both installed zones, including all mesh indices, placement/LOD references, texture identities and spatial references. No packaged UE or live-client visual fidelity claim is made.

Native material texture exports now accompany UI previews as `render_image` TGA files.
`dat_image_native` uses the same bounded C decoder while preserving decoded alpha bytes;
`dat_image` retains its existing UI alpha expansion. The Unreal material stage applies
MODULATE4X alpha itself. Importing the UI preview and multiplying again would clip source
coverage. A synthetic fixture verifies alpha 0/64/128/255 remains unchanged in the native
path and expands to 0/128/255/255 in the existing UI path.
