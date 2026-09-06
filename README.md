# Catalyst v0.1.0

Catalyst is a high-performance rendering framework for C++ designed to simplify the development of applications. It provides a robust set of features, including 2D and 3D rendering, UI components, and cross-platform support.

The goal of Catalyst is to offer developers a powerful yet easy-to-use toolkit for building visually rich applications. Whether you're creating games, simulations, desktop applications, or data visualization tools, Catalyst has you covered.

## Features

- High-performance 2D and 3D rendering
- Easy-to-use UI components
- Cross-platform support (Windows, macOS, Linux, iOS, Android)
- Modular architecture for easy extension
- Comprehensive documentation and examples

## Getting Started

To get started with Catalyst, follow these steps:

1. Clone the repository:

   ```bash
   git clone https://github.com/yourusername/Catalyst.git
   ```

2. Navigate to the project directory:

   ```bash
    cd Catalyst
    ```

3. Build the project using CMake:

    ```bash
    cmake -S . -B build
    cmake --build build --config Release
    ```

4. Run the example application:

    ```bash
    ./build/examples/audio_playback/Release/catalyst_audio_playback.exe
    ```

### Multi-compiler builds (Windows)

This repo includes a root [CMakePresets.json](CMakePresets.json) so you can quickly validate builds across multiple compilers.

- List presets:

  ```bash
  cmake --list-presets
  ```

- Build MSVC (Visual Studio):

  ```bash
  cmake --preset msvc-x64
  cmake --build --preset msvc-x64-debug
  cmake --build --preset msvc-x64-release
  ```

  Note about the MSVC generator:

  - The `msvc-x64` preset uses a specific CMake *Visual Studio* generator (e.g. `Visual Studio 18 2026`).
    If your installed Visual Studio version does not match the generator in the preset, CMake may report that it
    “could not find any instance of Visual Studio”.
  - If you change the generator, you must use a fresh build directory (or run `cmake --fresh --preset msvc-x64`) to
    avoid “generator does not match the generator used previously” errors.
  - To see which generator names are available on your machine:

    ```powershell
    cmake --help
    ```

- Build Clang-CL (Ninja):

  ```bash
  cmake --preset clangcl-x64-debug
  cmake --build --preset clangcl-x64-debug

  cmake --preset clangcl-x64-release
  cmake --build --preset clangcl-x64-release
  ```

- Build the whole matrix (script):

  ```powershell
  ./scripts/build-all-presets.ps1
  # or: ./scripts/build-all-presets.ps1 -Config Debug
  # or: ./scripts/build-all-presets.ps1 -Config Release
  ```

### Test presets

Test-enabled configure/build presets are provided with the `-tests` suffix.

- Build + run tests (Windows):

  ```powershell
  ./scripts/build-all-presets.ps1 -Tests -RunTests
  ```

- Or run a specific test preset directly:

  ```bash
  # after building one of the *-tests build presets
  ctest --preset clangcl-x64-debug-tests
  ```

### Cross-platform presets (Linux/macOS)

Presets are included for Linux (GCC/Clang) and macOS (Clang). These use the Ninja generator.
On each host OS, only the relevant presets are shown (Windows won’t list Linux/macOS presets, etc.).

Examples:

```bash
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug

cmake --preset macos-clang-release-tests
cmake --build --preset macos-clang-release-tests
ctest --preset macos-clang-release-tests
```

### Build-all scripts

Scripts are provided to auto-run all *available* presets on the current host:

- Windows: [scripts/build-all-available-presets.ps1](scripts/build-all-available-presets.ps1)
  - Build: `./scripts/build-all-available-presets.ps1`
  - Build + test: `./scripts/build-all-available-presets.ps1 -RunTests`
- Linux: [scripts/build-all-available-presets-linux.sh](scripts/build-all-available-presets-linux.sh)
  - Build: `./scripts/build-all-available-presets-linux.sh`
  - Build + test: `./scripts/build-all-available-presets-linux.sh --run-tests`
- macOS: [scripts/build-all-available-presets-macos.sh](scripts/build-all-available-presets-macos.sh)
  - Build: `./scripts/build-all-available-presets-macos.sh`
  - Build + test: `./scripts/build-all-available-presets-macos.sh --run-tests`

Linux/macOS wrappers call the shared runner: [scripts/build-all-available-presets.py](scripts/build-all-available-presets.py)

Note: on Linux/macOS you may need to set the executable bit once:

```bash
chmod +x scripts/build-all-available-presets-linux.sh scripts/build-all-available-presets-macos.sh
```

## CMake Options

Catalyst is modular: you can link individual modules, or link the monolithic umbrella library.

- **Monolithic**: `CATALYST_BUILD_ALL` (default: `ON`)
  - Builds the `catalyst` target (aliases: `catalyst::catalyst`, `catalyst::all`) which links all enabled modules.
- **Modules** (all default to `ON`):
  - `CATALYST_BUILD_ANIMATION`
  - `CATALYST_BUILD_AUDIO`
  - `CATALYST_BUILD_CORE`
  - `CATALYST_BUILD_INPUT`
  - `CATALYST_BUILD_MATH`
  - `CATALYST_BUILD_NET`
  - `CATALYST_BUILD_PHYSICS`
  - `CATALYST_BUILD_PLATFORM`
  - `CATALYST_BUILD_RENDERING`
  - `CATALYST_BUILD_RESOURCE`
  - `CATALYST_BUILD_SCENE`
  - `CATALYST_BUILD_UI`
  - `CATALYST_BUILD_UTILS`
- **Extras**:
  - `CATALYST_BUILD_EXAMPLES` (default: `ON`)
  - `CATALYST_BUILD_TESTS` (default: `OFF`)

- **Backend selection**:
  - `CATALYST_INPUT_BACKEND` (default: `auto`) values: `auto`, `win32`, `null`
  - `CATALYST_AUDIO_BACKEND` (default: `auto`) values: `auto`, `win32`, `null`
  - `CATALYST_PLATFORM_BACKEND` (default: `auto`) values: `auto`, `win32`, `null`
  - `CATALYST_RENDERING_BACKEND` (default: `auto`) values: `auto`, `d3d12`, `vulkan`, `null`
    - `vulkan` needs a Vulkan SDK (1.3 or newer) that CMake's `find_package(Vulkan)` can locate, e.g. via the
      `VULKAN_SDK` environment variable, and a driver exposing Vulkan 1.3. Shaders are consumed as SPIR-V;
      `scripts/embed_spirv.py` compiles GLSL with `glslc` and embeds the result in a header.

Example: build only a subset of modules (no monolithic target):

```bash
cmake -S . -B build \
    -DCATALYST_BUILD_ALL=OFF \
    -DCATALYST_BUILD_RENDERING=ON \
    -DCATALYST_BUILD_UI=OFF \
    -DCATALYST_BUILD_CORE=ON
cmake --build build --config Release
```

Example: build monolithic Catalyst without networking:

```bash
cmake -S . -B build -DCATALYST_BUILD_NET=OFF
cmake --build build --config Release
```

Example: force specific backends:

```bash
cmake -S . -B build \
  -DCATALYST_INPUT_BACKEND=win32 \
  -DCATALYST_AUDIO_BACKEND=win32 \
  -DCATALYST_PLATFORM_BACKEND=win32 \
  -DCATALYST_RENDERING_BACKEND=d3d12
cmake --build build --config Release
```

## Includes

- Include everything (umbrella header):
  - `#include <catalyst/catalyst.hpp>`
- Include only a module:
  - `#include <catalyst/physics/physics.hpp>`
  - `#include <catalyst/rendering/rendering.hpp>`

## Components

Catalyst is composed of several key components that can be used independently or together:

- **Renderer**: Handles all rendering operations, supporting both 2D and 3D graphics.
- **UI System**: A retained tree of styled nodes with CSS-like units and a flexbox layout engine, emitting a backend-agnostic draw list. See [docs/ui.md](docs/ui.md) for the architecture and roadmap.
- **Input Handling**: Layout-independent keyboard events, text input, mouse (including raw motion and cursor capture) and gamepads, with an event stream and a polled `input_state` view.
- **Math Library**: Offers a collection of mathematical functions and data structures commonly used in graphics programming.
- **Utilities**: A set of helper functions and classes to simplify common tasks.
- **Platform Abstraction**: Ensures compatibility across different operating systems and hardware.
- **Resource Management**: Efficiently loads and manages assets such as textures, models, and shaders.
- **Scene Graph**: Organizes and manages the hierarchical structure of objects in a scene.
- **Animation System**: Provides tools for creating and managing animations for objects within the framework.
- **Networking Module**: Enables network communication for multiplayer applications or data synchronization.
- **Audio System**: Supports audio playback and manipulation for immersive experiences.
- **Scripting Support**: Integrates scripting languages for dynamic behavior and rapid prototyping.
- **Physics Engine**: Offers basic physics simulation capabilities for realistic object interactions.
- **Plugin System**: Allows for easy extension of the framework through plugins.
- **Debugging Tools**: Includes tools for profiling and debugging applications built with Catalyst.
- **Documentation Generator**: Facilitates the creation of documentation for projects using Catalyst.
- **Testing Framework**: Provides a suite of tools for unit testing and integration testing of applications.

## Documentation

Comprehensive documentation is available in the `docs` directory. You can also generate the latest documentation using Doxygen:

```bash
doxygen Doxyfile
```

This will create HTML documentation in the `docs/html` directory.

You can also view the documentation online at [https://colton_mcgraw.github.io/Catalyst/docs](https://colton_mcgraw.github.io/Catalyst/docs).

## Contributing

Contributions are welcome! Please read the `CONTRIBUTING.md` file for guidelines on how to contribute to the project.

## License

Catalyst is licensed under the MIT License. See the `LICENSE` file for more information.

--
