# Componentes de terceros / Third-party notices

Cramion usa estas librerías, cada una con su propia licencia. Los juegos exportados incluyen las que usa el Runtime; sus avisos de copyright deben acompañar al juego (basta con copiar este archivo y la carpeta `licencias/` del paquete junto al ejecutable).

Cramion uses the following libraries, each under its own license. Exported games include the ones the Runtime uses; keep their copyright notices with your game (copying this file and the package's `licencias/` folder next to the executable is enough).

| Componente | Licencia | Origen |
|---|---|---|
| Dear ImGui (docking) | MIT | https://github.com/ocornut/imgui |
| ImGuizmo | MIT | https://github.com/CedricGuillemet/ImGuizmo |
| EnTT | MIT | https://github.com/skypjack/entt |
| Jolt Physics | MIT | https://github.com/jrouwe/JoltPhysics |
| miniaudio | Dominio público (Unlicense) o MIT-0 | https://github.com/mackron/miniaudio |
| ENet | MIT | https://github.com/lsalzman/enet |
| Recast & Detour | zlib | https://github.com/recastnavigation/recastnavigation |
| meshoptimizer | MIT | https://github.com/zeux/meshoptimizer |
| bc7enc (compresión de texturas BC7, `CramionFX/vendor/bc7enc`) | MIT o dominio público (Unlicense) | https://github.com/richgel999/bc7enc_rdo |
| Vulkan Memory Allocator 3.3.0 (asignador de memoria de la GPU, `CramionFX/vendor/vma`) | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| xatlas (UV de lightmap automáticas, solo el editor; `CramionCore/vendor/xatlas`, commit f700c7790aaa030e794b52ba7791a05c085faf0c) | MIT | https://github.com/jpcy/xatlas |
| zstd | BSD-3-Clause | https://github.com/facebook/zstd |
| nlohmann/json | MIT | https://github.com/nlohmann/json |
| assimp | BSD-3-Clause | https://github.com/assimp/assimp |
| ufbx 0.23.1 (cargador de FBX, `CramionFX/vendor/ufbx`, commit 26a482ae66871d7de36eb722aa060bce95bce274) | MIT o dominio público (Unlicense) | https://github.com/ufbx/ufbx |
| stb (commit 4e9b368bfc071e97206ea5c6adba68dd0cbf8897) | Dominio público o MIT | https://github.com/nothings/stb |
| OpenXR SDK (loader) | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK |
| Vulkan SDK / headers | Apache-2.0 | https://vulkan.lunarg.com |
| glslang | BSD-3-Clause y otras (ver su repositorio) | https://github.com/KhronosGroup/glslang |
| shaderc (`shaderc_shared.dll` del Vulkan SDK, compila los `.crshader` en tiempo de ejecución; va con el editor, el paquete y los juegos exportados) | Apache-2.0 | https://github.com/google/shaderc |
| SPIRV-Tools (dentro de `shaderc_shared.dll`: validación y optimización de SPIR-V) | Apache-2.0 | https://github.com/KhronosGroup/SPIRV-Tools |
| DirectX Shader Compiler | LLVM (Apache-2.0 con excepción LLVM) / NCSA | https://github.com/microsoft/DirectXShaderCompiler |
| AMD FidelityFX (FSR 3.1, SDK 1.1.4) | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| AMD FidelityFX FSR 1 (cabeceras de shader `ffx_a.h` y `ffx_fsr1.h`, commit a21ffb8f6c13233ba336352bdff293894c706575) | MIT | https://github.com/GPUOpen-Effects/FidelityFX-FSR |
| NVIDIA DLSS | NVIDIA RTX SDKs License (ver `licencias/NVIDIA-DLSS-LICENSE.txt`) | https://github.com/NVIDIA/DLSS |
| bundletool (solo exportación Android) | Apache-2.0 | https://github.com/google/bundletool |
| llvm-mingw (compilador de los scripts de C++ incluido con el editor, `toolchain/`; no va en los juegos exportados) | Apache-2.0 con excepción LLVM; las cabeceras y librerías de mingw-w64 con las suyas (ver `licencias/llvm-mingw-LICENSE.txt`) | https://github.com/mstorsjo/llvm-mingw |
| WebXR Input Profiles (modelos de los mandos de Meta Quest 3) | MIT | https://github.com/immersive-web/webxr-input-profiles |
| XeGTAO (algoritmo de la oclusión ambiental GTAO con normales dobladas, adaptado en `CramionFX/shaders/gtao.frag`) | MIT | https://github.com/GameTechDev/XeGTAO |

SPIRV-Cross no se enlaza: en `CramionFX/vendor/include` solo hay cabeceras del Vulkan SDK que el motor no incluye.

SPIRV-Cross is not linked: `CramionFX/vendor/include` only holds Vulkan SDK headers the engine does not include.

Estas licencias se aplican solo a esos componentes. El resto de Cramion se rige por [LICENSE](LICENSE).

These licenses apply only to those components. The rest of Cramion is governed by [LICENSE](LICENSE).

## WebXR Input Profiles

La malla de los mandos de Meta Quest 3 (Touch Plus) integrada en el motor sale de `@webxr-input-profiles/assets` 1.0. La licencia no da permiso para usar las marcas de los fabricantes (Meta, Quest).

The built-in Meta Quest 3 (Touch Plus) controller mesh comes from `@webxr-input-profiles/assets` 1.0. The license does not grant permission to use the manufacturers' trademarks (Meta, Quest).

```
MIT License

Copyright (c) 2019 Amazon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Vulkan Memory Allocator

Asigna la memoria de la GPU (buffers y texturas) y lleva el anillo de staging. Va en el editor y en los juegos exportados (lo usa el Runtime).

Allocates GPU memory (buffers and textures) and runs the staging ring. It ships with the editor and with exported games (the Runtime uses it).

```
Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## xatlas

Genera las UV de lightmap (UV2) en el editor al hornear la iluminación. No va en los juegos exportados (el Runtime no lo usa).

Generates the lightmap UVs (UV2) in the editor when baking lighting. It is not shipped with exported games (the Runtime does not use it).

```
MIT License

Copyright (c) 2018-2020 Jonathan Young

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## XeGTAO

La oclusión ambiental GTAO (`CramionFX/shaders/gtao.frag`: horizontes por cortes, integral del coseno y normales dobladas) sigue el código de XeGTAO, reescrito en GLSL para el renderizador de Cramion.

The GTAO ambient occlusion (`CramionFX/shaders/gtao.frag`: per-slice horizons, cosine-weighted integral and bent normals) follows the XeGTAO code, rewritten in GLSL for the Cramion renderer.

```
MIT License

Copyright (C) 2016-2021, Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
