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

Windows, Direct3D, DXGI, Media Foundation, Winsock, and related SDK libraries are
provided by the Windows SDK and are not redistributed as source dependencies.
