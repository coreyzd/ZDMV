# ZDMV

A small desktop model viewer for browsing folders of 3D models. Loads:

- Wavefront `.obj` with `.mtl` materials and diffuse textures
- Source Engine `.mdl` (v44-49, with its `.vvd` and `.vtx`), textured from
  `.vmt`/`.vtf` materials

C++17, OpenGL 3.3, GLFW, Dear ImGui.

## Build

Needs CMake 3.20+, a C++17 compiler, git, and the usual X11/Wayland/OpenGL
development headers. Everything else (GLFW, Dear ImGui, GLM, tinyobjloader,
stb_image) is downloaded by CMake on the first configure.

    # Debian / Ubuntu
    sudo apt install build-essential cmake git libgl1-mesa-dev libx11-dev libxrandr-dev \
        libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev libwayland-dev

    # Fedora
    sudo dnf install gcc-c++ cmake git mesa-libGL-devel libX11-devel libXrandr-devel \
        libXinerama-devel libXcursor-devel libXi-devel libxkbcommon-devel wayland-devel

    # Arch
    sudo pacman -S base-devel cmake git mesa libx11 libxrandr libxinerama libxcursor libxi \
        libxkbcommon wayland

Then:

    cmake -S . -B build
    cmake --build build -j

If you don't want Wayland (or lack its headers), add `-DGLFW_BUILD_WAYLAND=OFF`
to the first command.

### Windows

With Visual Studio 2022 (the "Desktop development with C++" workload includes
CMake) and git, from a Developer PowerShell:

    cmake -S . -B build
    cmake --build build --config Release

The exe lands in `build\Release\zdmv.exe`. With MinGW-w64 instead, add
`-G "MinGW Makefiles"` to the first command; it links statically, so
`build\zdmv.exe` runs on its own.

To cross-compile from Linux, install `g++-mingw-w64-x86-64-posix` and pass a
toolchain file that sets `CMAKE_SYSTEM_NAME Windows` and the
`x86_64-w64-mingw32-gcc-posix` / `g++-posix` compilers.

On Windows, settings are kept in `%APPDATA%\zdmv`, and `~` in the path box
means your user folder.

## Run

    ./build/zdmv                  # reopens the last folder you browsed
    ./build/zdmv ~/models         # open a folder
    ./build/zdmv ~/models/a.obj   # open a file (and list its folder)

You can also drop a file or folder onto the window.

| Input                       | Action                         |
|-----------------------------|--------------------------------|
| Up / Down                   | Previous / next model          |
| Page Up / Page Down         | Jump 10 models                 |
| Home / End                  | First / last model             |
| Backspace                   | Parent folder                  |
| Left drag                   | Orbit                          |
| Right or middle drag        | Pan                            |
| Scroll                      | Zoom                           |
| F                           | Reset view                     |

Type in the path box and press Enter to jump to a folder (`~` works). The
filter box narrows the model list. Drag the panel's right edge to resize it.

## Source models

Only the highest LOD is shown, with the default bodygroups and skin, posed at
the first frame of the model's first sequence: the way it stands in the game
and in Hammer, which for most props is a quarter turn from how the vertices
are stored. Animation is otherwise not played, flexes are ignored, and
materials use just their `$basetexture` (plus `$translucent`/`$alphatest`).

A model stores material names relative to a game folder, so ZDMV has to work
out where to look. In order:

1. The model's game folder: the parent of the nearest `models` directory
   above the file (`.../hl2/models/props/x.mdl` -> `.../hl2`).
2. Up to three folders above that, where they contain `materials/`.
3. Folders added under **Content paths...** in the viewer, and game folders
   directly inside them.
4. Neighbouring game folders, then game folders nested inside the model's own
   (including `custom/<pack>`).

Names are matched ignoring case. Hover the materials line in the Model panel
to see which texture each material resolved to, or what was missing; the
Content paths dialog lists the folders searched for the current model.

If the model has a `.phy` file, tick **Physics model** to overlay its
collision hulls in cyan. Ragdoll solids are placed on the skeleton in that
same pose.

Content packed in `.vpk` archives is not read, so stock game materials need
to be extracted, or you'll see models untextured.

## Layout

    src/model.h              Format-independent mesh/material data
    src/loaders/loader.*     Loader interface and registry (by file extension)
    src/loaders/obj_loader.cpp
    src/loaders/mdl_loader.cpp
    src/source/content.*     Source content search path, case-insensitive lookup
    src/source/vmt.*         Material (.vmt) reader
    src/source/vtf.*         Texture (.vtf) decoder
    src/source/phy.*         Collision model (.phy) reader
    src/image.*              Texture file -> RGBA
    src/normals.*            Normal regeneration (smooth/flat)
    src/renderer.*           GPU upload and drawing
    src/browser.*            Folder listing
    src/camera.h             Orbit camera
    src/main.cpp             Window, UI, input
    third_party/glad         Generated OpenGL 3.3 core loader

## Adding a format

Implement `ModelLoader` (see `loaders/loader.h`), fill in a `Model`, and
register the loader in `loaders/loader.cpp`. The browser lists any extension
a registered loader claims, so nothing else has to change.
