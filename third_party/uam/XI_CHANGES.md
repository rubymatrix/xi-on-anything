# uam in xi-on-anything

[uam](https://github.com/devkitPro/uam), devkitPro's GLSL-to-Maxwell shader compiler for deko3d, at commit
`5a5afc2bae8b55409ab36ba45be63fcb73f68993` (version 1.1.0), used as a library on the Nintendo Switch by the deko3d
graphics back end: the game's shaders are generated at run time, so they are compiled on the console.

Its own files are under the zlib licence in `LICENSE`; `mesa-imported/` is Mesa's (MIT, its files' own headers).
As that licence asks, the altered files are marked here:

- `source/compiler_iface.h`, `source/compiler_iface.cpp`: `DekoCompiler::OutputDkshMem`, the same bytes as
  `OutputDksh` writes to a file, into a malloc'd buffer instead.
- `source/uam_api.cpp`, `source/uam_api.h` (new): `uam_compile`, one GLSL stage to its DKSH in memory.
- `generated/`: the files uam's meson build makes with bison, flex and Python (the GLSL parser and lexer, the
  preprocessor's, `ir_expression_operation*.h`), made once so the build needs none of those tools.
- `manifest.json`: its sources and flags for `tools/thirdparty.py` (the meson build is kept, unused).
