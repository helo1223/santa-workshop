# SantaWorkshop

SantaWorkshop is a Windows map editor with a raylib-powered 3D preview. It reads
LVLX `.dat` levels and CRF models, supports element and movement-route editing,
undo/redo, environment settings, collision and animation previews, particle
effects, and exporting edits as a mod.

For HD compatibility, LVLX versions 2 through 255 are preserved. Versions above
2 use the attachment-key element layout used by v3; the retail loader does not
impose a later-version ceiling. Legacy v0/v1 levels remain read-only because
their missing-trailer defaults are not an authoring contract.

Game resources are not included. Building requires the C/C++ toolchain and
raylib; running the editor also requires a local game installation or extracted
resource tree.

## Prerequisites

- Windows with Visual Studio or Visual Studio Build Tools, the **Desktop
  development with C++** workload, **MSVC v145** toolset, and a Windows SDK.
  The project uses C++20 and targets Windows SDK `10.0`.
- An **x64 MSVC-compatible raylib** library and matching headers. The local
  dependency used to verify these instructions reports raylib **6.0**.
- For the graphical editor, an OpenGL-capable graphics driver.

Place raylib at the following paths before building:

```text
third_party/
  raylib/
    include/
      raylib.h
      raymath.h
      rlgl.h
    lib/
      raylib.lib
```

`third_party/` is Git-ignored, so a fresh checkout does not supply or download
this dependency. Use the x64 static MSVC library, not a MinGW `.a` library or
the separate `raylibdll.lib` import library. The project links `raylib.lib`
and the required Windows system libraries automatically.

## Build

Run commands from the repository root.

### Visual Studio

1. Open `SantaWorkshop.slnx` (or `SantaWorkshop.vcxproj` directly).
2. Select **Debug** or **Release**, with platform **x64**.
3. Choose **Build > Build Solution**.

Use x64: although Win32 configurations exist, they do not configure the raylib
include path or linker dependencies.

### Command line

Open an **x64 Visual Studio Developer Command Prompt**, change to the repository
directory, and run:

```bat
msbuild SantaWorkshop.vcxproj /m /p:Configuration=Debug /p:Platform=x64
msbuild SantaWorkshop.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

Executables are written to `x64/Debug/SantaWorkshop.exe` and
`x64/Release/SantaWorkshop.exe`. Intermediate object files go to
`build/x64/<Configuration>/`.

If MSBuild cannot find v145, install that toolset through the Visual Studio
Installer. Missing `raylib.h` or `raylib.lib` errors mean the dependency layout
above is incomplete or the wrong platform is selected. A Debug `LNK4098` warning
about `MSVCRT` indicates conflicting C runtime libraries; use a raylib build
whose runtime settings match the editor configuration.

## Version

The application version is defined in `version.h`, currently `0.1`. To publish
a new version, update both the display string and the four-part Windows numeric
version (currently `0,1,0,0`), then rebuild. `SantaWorkshop.rc` embeds these values
in the executable; view them by right-clicking `SantaWorkshop.exe` and selecting
**Properties > Details**.

## Run

If your game's resources are packed in a `.pak` file, use
[jeysym's scit-hd-pak-tool](https://github.com/jeysym/scit-hd-pak-tool) to unpack
them first. It provides the `unpak <pak-path> <dir-path>` command; see its README
for usage details. Place the extracted `bin_win32` resource folder in the game
directory so the layout matches the example below. SantaWorkshop reads the
unpacked files directly.

To launch without command-line arguments, copy `SantaWorkshop.exe` from
`x64/Release/` into your **Santa Claus in Trouble game folder**, alongside the
`bin_win32` directory, then double-click it:

```text
Santa Claus in Trouble/
  SantaWorkshop.exe
  bin_win32/
    levels/001.dat
    settings/elements.txt
```

The editor opens the first map automatically. Use **Open map...** to choose
another map or a mod level. If launching through a shortcut, set its **Start in**
directory to the game folder so the default resource paths resolve correctly.

Alternatively, run the editor from another directory by passing a level and its
element definitions, replacing these example paths with your installation:

```bat
x64\Debug\SantaWorkshop.exe "C:\Games\Santa\bin_win32\levels\001.dat" "C:\Games\Santa\bin_win32\settings\elements.txt"
```

Keep the resource tree intact, including its `assets`, `textures`, `effects`,
and `settings` directories. The editor finds the resource root by walking up
from the level and looking for `settings/elements.txt` or
`bin_win32/settings/elements.txt`. The definitions argument is optional when
that discovery succeeds. With no arguments, the editor tries
`bin_win32/levels/001.dat` relative to the working directory.

- **Open map...:** browse for a `.dat` file in any directory, including a mod's
  `levels` folder. Save or undo current edits before switching. Canceling or
  selecting an unreadable map keeps the current level open. Maps inside another
  installation load that installation's shared resources; loose maps use the
  current resource root. The loaded filename appears beside the button.
- **Ctrl+S:** save the current level and environment settings.
- **Ctrl+Z / Ctrl+Y:** undo / redo.
- **F3 / F4:** toggle collision / animation previews.
- **Ctrl+Shift+M:** export edits as a new mod and switch to editing it.
- **Ctrl+Shift+N:** create an empty mod level from the current level's settings.

Letter shortcuts follow your active keyboard layout. For example, **Ctrl+Z**
undoes on both Hungarian and US layouts; **Ctrl+Y** or **Ctrl+Shift+Z** redoes.

Saving writes to the opened level. To work in a separate mod folder, use mod
export first. Exported mods still depend on shared game assets.

For validation without opening the editor window, supply both paths and a flag:

```bat
x64\Debug\SantaWorkshop.exe "C:\Games\Santa\bin_win32\levels\001.dat" "C:\Games\Santa\bin_win32\settings\elements.txt" --validate
x64\Debug\SantaWorkshop.exe "C:\Games\Santa\bin_win32\levels\001.dat" "C:\Games\Santa\bin_win32\settings\elements.txt" --validate-effects
```

## Source layout

| Path | Purpose |
| --- | --- |
| `EditorMain.cpp` | Editor UI, input, history, asset loading, rendering, and lighting |
| `map_picker.cpp`, `map_picker.h` | Native Windows map file picker |
| `lvlx_format.c`, `lvlx_format.h` | Level parsing, serialization, and element definitions |
| `crf_format.c`, `crf_format.h` | CRF model parsing |
| `effect_preview.cpp`, `effect_preview.h` | Particle effect previews |
| `animation_preview.h`, `physics_preview.h` | Animation and collision previews |
| `environment_settings.h`, `environment_cube.h` | Environment documents and cubemap loading |
| `mod_project.h` | Mod creation and export |
