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

## Which processors the build targets

On x86-64, a release build targets `x86-64-v3`, which means AVX2, FMA and BMI2, and which every processor from 2013 onwards has. The result therefore runs on any machine that meets that floor, and not merely on the one that compiled it.

Two options change this:

```bash
cmake -D ZELPH_NATIVE_ARCH=ON -B build .          # for this machine only, and faster
cmake -D ZELPH_X86_BASELINE=x86-64-v2 -B build .  # for processors older than 2013
```

`ZELPH_NATIVE_ARCH` hands the compiler everything the building machine offers. That is worth having for a build you run yourself, and it must not be used for anything you pass on: the binary then dies with an illegal instruction on every processor that lacks one of those instructions.

`ctest` holds the built library against the declared floor and fails when it needs more, so a release cannot acquire such a dependency unnoticed.

## Verifying the Build

Test your installation by running the CLI:

```bash
./build/bin/zelph
```

or

```bash
./build/bin/zelph stdlib/examples/english.zph
```
