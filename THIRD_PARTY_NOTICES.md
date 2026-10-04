# Third-Party Notices

## Dear ImGui

- Project: https://github.com/ocornut/imgui
- Version: 1.92.0
- License: MIT
- Usage: Server user interface, fetched during CMake configuration.

## MinGW-w64 and GCC runtime libraries

MinGW-built Windows packages redistribute the runtime DLLs required by the
compiled executables:

- `libstdc++-6.dll` and `libgcc_s_seh-1.dll`: GCC runtime libraries, licensed
  under GPL-3.0-or-later with the GCC Runtime Library Exception 3.1 (and
  applicable LGPL components).
- `libwinpthread-1.dll`: MinGW-w64 winpthreads runtime, licensed under MIT and
  BSD-3-Clause-Clear terms.

These DLLs are copied from the selected MinGW toolchain at package time. MSVC
packages do not include them.

## LizardByte Sunshine

- Project: https://github.com/LizardByte/Sunshine
- Release: 2026.914.233613
- License: GPL-3.0
- Usage: Host-side desktop streaming service (`SunshineService` / `sunshine.exe`) on managed student endpoints. Invoked as an external Windows service; not statically or dynamically linked into NSTU binaries, nor redistributed within the installer packages without explicit configuration.

## Moonlight Game Streaming

- Project: https://github.com/moonlight-stream/moonlight-qt
- License: GPL-3.0
- Usage: Client-side desktop streaming viewer (`Moonlight.exe`) on teacher workstations. Managed and launched as an external isolated process under Windows Job Objects; not statically or dynamically linked into NSTU binaries.

Windows, Direct3D, DXGI, Media Foundation, Winsock, and related SDK libraries are
provided by the Windows SDK and are not redistributed as source dependencies.
