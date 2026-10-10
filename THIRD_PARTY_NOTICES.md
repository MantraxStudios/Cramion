# Componentes de terceros / Third-party notices

Cramion usa estas librerías, cada una con su propia licencia. Los juegos exportados incluyen las que usa el Runtime; sus avisos de copyright deben acompañar al juego (basta con copiar este archivo y la carpeta `licencias/` del paquete junto al ejecutable).

Cramion uses the following libraries, each under its own license. Exported games include the ones the Runtime uses; keep their copyright notices with your game (copying this file and the package's `licencias/` folder next to the executable is enough).

| Componente | Licencia | Origen |
|---|---|---|
| Dear ImGui (docking) | MIT | https://github.com/ocornut/imgui |
| ImGuizmo | MIT | https://github.com/CedricGuillemet/ImGuizmo |
| EnTT | MIT | https://github.com/skypjack/entt |
| Jolt Physics | MIT | https://github.com/jrouwe/JoltPhysics |
| Lua 5.4 | MIT | https://www.lua.org |
| sol2 | MIT | https://github.com/ThePhD/sol2 |
| miniaudio | Dominio público (Unlicense) o MIT-0 | https://github.com/mackron/miniaudio |
| ENet | MIT | https://github.com/lsalzman/enet |
| Recast & Detour | zlib | https://github.com/recastnavigation/recastnavigation |
| meshoptimizer | MIT | https://github.com/zeux/meshoptimizer |
| bc7enc (compresión de texturas BC7, `CramionFX/vendor/bc7enc`) | MIT o dominio público (Unlicense) | https://github.com/richgel999/bc7enc_rdo |
| zstd | BSD-3-Clause | https://github.com/facebook/zstd |
| nlohmann/json | MIT | https://github.com/nlohmann/json |
| assimp | BSD-3-Clause | https://github.com/assimp/assimp |
| stb | Dominio público o MIT | https://github.com/nothings/stb |
| OpenXR SDK (loader) | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK |
| Vulkan SDK / headers | Apache-2.0 | https://vulkan.lunarg.com |
| glslang | BSD-3-Clause y otras (ver su repositorio) | https://github.com/KhronosGroup/glslang |
| DirectX Shader Compiler | LLVM (Apache-2.0 con excepción LLVM) / NCSA | https://github.com/microsoft/DirectXShaderCompiler |
| AMD FidelityFX (FSR) | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| NVIDIA DLSS | NVIDIA RTX SDKs License (ver `licencias/NVIDIA-DLSS-LICENSE.txt`) | https://github.com/NVIDIA/DLSS |
| bundletool (solo exportación Android) | Apache-2.0 | https://github.com/google/bundletool |
| WebXR Input Profiles (modelos de los mandos de Meta Quest 3) | MIT | https://github.com/immersive-web/webxr-input-profiles |

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
