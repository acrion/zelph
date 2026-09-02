# Building zelph

You do not need to build zelph to use it. The [Quick Start Guide](quickstart.md) includes prebuilt binaries for Linux, macOS and Windows; build from source only when you want to change the engine, and see [Contributing](https://github.com/acrion/zelph/blob/main/CONTRIBUTING.md) for what a change has to bring with it.

You need:

- C++ compiler (supporting at least C++20)
- CMake 3.25.2+
- Git

## Build Instructions

1. Clone the repository with all submodules:

    ```bash
    git clone --recurse-submodules https://github.com/acrion/zelph.git
    ```

2. Configure the build (Release mode):

    ```bash
    cmake -D CMAKE_BUILD_TYPE=Release -B build .
    ```

3. Build the project (for MSVC, add `--config Release`):

    ```bash
    cmake --build build
    ```

## Verifying the Build

Test your installation by running the CLI:

```bash
./build/bin/zelph
```

or

```bash
./build/bin/zelph stdlib/examples/english.zph
```
