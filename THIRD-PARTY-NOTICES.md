# Third-party notices

Zelda: Inside The Dream contains no Nintendo content. It includes or uses the components below, each under its own
license; the full texts are in the files named here, and in the packages.

| Component | What it does here | License | Source | Full text |
| --- | --- | --- | --- | --- |
| LAS Recomp | the base of this project: the recompilation of the game for PC | MIT, Copyright (c) 2026 Peiikai | https://github.com/peiikai/LASRecomp | LICENSE (above) |
| Dear ImGui | the menus | MIT, Copyright (c) 2014-2025 Omar Cornut | https://github.com/ocornut/imgui | third_party/imgui/LICENSE.txt |
| LAS-Randomizer | the randomizer | MIT, Copyright (c) 2024 Owen_Splat | https://github.com/Owen-Splat/LAS-Randomizer | third_party/LAS-Randomizer/LICENSE.txt |
| Ryujinx (parts, inherited from LAS Recomp) | the audio engine (its reverb is a port of Ryujinx's code), and its shader instruction decoders used as the reference for the bit layout | MIT, Copyright (c) Ryujinx Team and Contributors | (its notice) | third_party/ryujinx/LICENSE.txt |
| Snapdragon Game Super Resolution 2 | upscaling | BSD-3-Clause, Copyright (c) 2023 Qualcomm Innovation Center, Inc. | https://github.com/SnapdragonGameStudios/snapdragon-gsr | third_party/sgsr2/LICENSE |
| SMAA | anti-aliasing | MIT-style, Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria and others | https://github.com/iryoku/smaa | third_party/smaa/LICENSE.txt |
| Kenney Input Prompts | the D-pad pictures of the HUD | CC0 1.0 | https://kenney.nl | third_party/kenney/LICENSE.txt |
| Anton, Barlow, Baloo 2, BIZ UDPGothic | fonts | SIL Open Font License 1.1 | Google Fonts | third_party/fonts/*/OFL*.txt |
| DejaVu fonts | fonts | DejaVu Fonts License (Bitstream Vera) | https://dejavu-fonts.github.io | third_party/fonts/DejaVu/LICENSE |
| OpenXR headers | PC VR | Apache-2.0 OR MIT (SPDX line of each header) | https://github.com/KhronosGroup/OpenXR-SDK | third_party/openxr |
| FFmpeg | decodes the game's movies (dynamic libraries, LGPL-only configuration: `tools/build_deps_*.sh`) | LGPL-2.1-or-later | https://ffmpeg.org | in the packages |
| SDL3 | windows, input, sound | zlib | https://libsdl.org | in the packages |
| OpenXR loader | PC VR | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK-Source | in the packages |
| shaderc | shader compilation | Apache-2.0 | https://github.com/google/shaderc | in the packages |
| zig | the compiler that rebuilds the game's code on the player's computer | MIT | https://ziglang.org | in the packages |

Every component found in the Windows and Mac packages is in this list; the Linux package is checked the same way.
