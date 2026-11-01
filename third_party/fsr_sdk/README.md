# FSR SDK

The C API of AMD's FSR SDK (FidelityFX SDK) 2.4.0, from
[GPUOpen-LibrariesAndSDKs/FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) at tag
`v2.4.0` (commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`), `Kits/FidelityFX/api/include/` (`ffx_api.h`,
`ffx_api_types.h`, `ffx_api_loader.h`, `dx12/ffx_api_dx12.h`) and `Kits/FidelityFX/upscalers/include/ffx_upscale.h`.
MIT license (the SDK's `Kits/FidelityFX/docs/license.md` lists these files, and its signed DLLs, under it):

> Copyright (C) Advanced Micro Devices, Inc. All rights reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation
> the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
> to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of
> the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
> THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
> CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
> IN THE SOFTWARE.

One change: `ffx_upscale.h` includes `ffx_api.h` and `ffx_api_types.h` from this folder rather than from
`../../api/include/`.

The DLLs (`amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, from the tag's
`Kits/FidelityFX/signedbin/`) are not here: `tools/build.py` `fsr_dlls` fetches them beside `host64.exe`.
`runtime/portable/gfx_d3d12.c` loads them at run time (`fsr_sdk_load`).
