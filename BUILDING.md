# Building lockd

Use CMake and a C++17 compiler. Linux, FreeBSD, and other POSIX-like environments use installed
Boost packages; Windows installs its dependencies through vcpkg. Python is optional for native
builds.

## Build and test

Install CMake 3.24+, Ninja, and Boost 1.82+ development packages. The native build needs Boost
`log`, `program_options`, `filesystem`, and `test`, with their dependencies, plus the header-only
`uuid` library. On Windows, follow [Windows build setup](#windows) instead.

From the repository root:

```sh
cmake --preset x86_64-opt-shared
cmake --build output/obj/x86_64/opt/shared --parallel 2
ctest --preset x86_64-opt-shared --output-on-failure
```

Use `arm64-opt-shared` on ARM64. Change the worker count (`--parallel`) to suit your machine. For
the `x86_64-opt-shared` preset, the daemon is written to `output/x86_64/opt/shared/lockd` (with
`.exe` on Windows); the tests are under `utility/lockd_test/output/` with the same profile path. See
[Presets](#presets) for more information on presets and what output paths look like for a given
preset.

See [RUNNING.md](RUNNING.md) to start the daemon, [TESTING.md](TESTING.md) for more test options,
and [PYTHON.md](PYTHON.md) to build the Python binding.

## Presets

Preset names follow `<arch>-<profile>-<linkage>`:

- Architecture: `x86`, `x86_64`, or `arm64`.
- Profile: `dbg` for Debug or `opt` for Release.
- Linkage: `shared` or `static`.

All three architectures have Debug and Release presets for both linkages, with matching CTest
presets. Run `cmake --list-presets` for configure presets or `ctest --list-presets` for test
presets. The compiler and dependencies must match the selected architecture; 32-bit Linux builds
need 32-bit dependencies too.

Shared builds are the usual choice on Linux and FreeBSD. Static builds need matching static
dependencies and compiler runtimes. Windows still depends on Windows system DLLs. Python extensions
need a shared compiler runtime, even when the C++ client is embedded in the extension.

For the FreeBSD port, enable `STATIC` in `make config` to include Boost and system runtimes in the
executables. It is off by default; see the [FreeBSD port
options](FREEBSD.md#port-options-and-tests).

## Windows

Install Visual Studio's C++ tools and Windows SDK, CMake 3.24+, Ninja, and Git. Open a Visual Studio
developer shell for the target architecture: x86 for `x86-*`, x64 for `x86_64-*`, or ARM64 for
`arm64-*`. Then use the same [build and test commands](#build-and-test) above.

To cross-compile ARM64 binaries on an x64 Windows machine, install the MSVC ARM64/ARM64EC build
tools and run `vcvarsall.bat x64_arm64` from Command Prompt. Configure with `arm64-opt-static` or
`arm64-opt-shared` and `-DLOCKD_BUILD_PYTHON=OFF`, using matching ARM64 Boost dependencies. Static
builds use the static runtime and the `arm64-windows-static` vcpkg triplet; shared builds use
`arm64-windows`. Cross-compiled binaries must be tested on an ARM64 Windows machine. On native
ARM64, `ctest --preset arm64-opt-static` or `ctest --preset arm64-opt-shared` runs the suite
normally.

Use your existing [vcpkg
installation](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started) and set `VCPKG_ROOT`
to its checkout directory before configuring. If vcpkg is not installed, follow that setup guide. An
explicit `CMAKE_TOOLCHAIN_FILE` takes precedence.

```powershell
$env:VCPKG_ROOT = 'C:/path/to/your/vcpkg'
cmake --preset x86_64-opt-static
```

The default manifest workflow installs the dependencies declared in `vcpkg.json`, reusing compatible
binaries from your configured binary cache.

To use packages already installed in vcpkg's classic installed tree, select classic mode:

```powershell
cmake --preset x86_64-opt-static -DVCPKG_MANIFEST_MODE=OFF
```

Classic mode consumes `<vcpkg-root>/installed`; it does not install missing packages automatically.
Use your usual `vcpkg install` commands for missing dependencies, with the matching target triplet.
Manifest mode normally uses `<build-dir>/vcpkg_installed`. An explicit `VCPKG_INSTALLED_DIR` selects
another installed tree. These are [vcpkg's standard CMake
settings](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration).

Additional vcpkg arguments can be passed with `VCPKG_INSTALL_OPTIONS`, either as a CMake setting or
through the environment. An explicit CMake setting takes precedence.

Keep required runtime DLLs beside shared-build executables; CMake copies them there for normal vcpkg
builds. See [WINDOWS.md](WINDOWS.md) for running the daemon, installing a service, and event
logging.

### Building installers

Build from an `opt-static` profile:

```powershell
cmake --preset x86_64-opt-static
cmake --build output/obj/x86_64/opt/static --target windows_installer
```

Use `x86-opt-static` for a 32-bit installer or `arm64-opt-static` for an ARM64 installer. The
installer does not currently support shared library builds. The target needs Python and downloads
NSIS if necessary. The installer and portable ZIP are written to `output/<arch>/opt/static/`.

In CLion, select **Windows installer (build)** and use **Build** with the desired profile. **Windows
installer (test)** builds and opens the wizard; completing it installs on your machine. Both entries
follow the selected CMake profile.

## Installation and downstream CMake projects

Install the build and SDK into a chosen prefix:

```sh
cmake --install output/obj/x86_64/opt/shared --prefix /path/to/install
```

Add `--component runtime` to omit the SDK. Install required shared dependencies on the destination
system; these install rules don't bundle them.

C++ applications link `lockd::client`; C applications use `lockd::c`:

```cmake
find_package(lockd CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE lockd::client)
if (MSVC AND lockd_STATIC_RUNTIME)
    set_property(TARGET app PROPERTY MSVC_RUNTIME_LIBRARY "${lockd_MSVC_RUNTIME_LIBRARY}")
endif ()
```

Use separate installation prefixes for static and shared SDKs. For a fully static GCC/Clang
application, also pass `-static` to that application's link step.

## Custom dependencies and troubleshooting

For dependencies outside the usual search paths, configure with
`-DCMAKE_PREFIX_PATH=<installed-prefix>` or `-DBoost_ROOT=<installed-prefix>`. A custom toolchain
can be supplied with `--toolchain <file>`.

Dependency selection defaults to installed packages on Unix and vcpkg on Windows. Override it with
`-DLOCKD_DEPENDENCY_PROVIDER=SYSTEM` or `VCPKG` when needed. Missing system packages must be
installed before configuring.

The `version>=` entries in `vcpkg.json` specify minimum supported versions. The `builtin-baseline`
selects dependency revisions for reproducible builds; it does not track the latest upstream releases
automatically.

When changing toolchains or moving a checkout, use a new build directory or
`cmake --fresh --preset <name>`. To clean compiled files without losing the configuration, run
`cmake --build <build-dir> --target clean`.

Each component owns its compile-time artifacts and release files beneath its own `output/`:

- Compile-time artifacts: `output/obj/<arch>/<dbg|opt>/<static|shared>/`.
- Release files, including wheels: `output/<arch>/<dbg|opt>/<static|shared>/`.

Use the repository's Ninja presets or another single-configuration generator. Each configure selects
one profile; project output directories are named `dbg` or `opt`. Windows linker PDBs are final
build files and sit beside the corresponding executable or DLL.

CMake keeps its internal `CMakeFiles/<target>.dir/` directories beneath the component's object
directory. For example, client-library objects use
`lib/liblockd/output/obj/x86_64/opt/shared/CMakeFiles/liblockd.dir/source/`, while test objects use
`utility/lockd_test/output/obj/x86_64/opt/shared/CMakeFiles/lockd_test.dir/source/`. Daemon objects
and the top-level configuration stay beneath the root `output/obj/` tree. Project-owned release
tools also stay in the root output tree.

Python wheel builds use scikit-build-core's supported ABI placeholder:
`lib/pylockd/output/obj/<wheel_tag>/opt/static/`, for example
`lib/pylockd/output/obj/cp314-cp314-win_amd64/opt/static/`. CMake's wheel targets and release
scripts use subdirectories there to keep their dependencies separate from direct Python builds.
Finished wheels use `lib/pylockd/output/<arch>/opt/static/`. Native Python module objects and
symbols use `lib/pylockd/output/obj/<arch>/<dbg|opt>/<static|shared>/`. The wheel backend's ABI tree
also contains the embedded client library built specifically for that wheel.

For a Debug wheel build, pass both `-Ccmake.build-type=Debug` and
`-Cbuild-dir=lib/pylockd/output/obj/{wheel_tag}/dbg/static` to the Python build frontend.

If building the same profile with another compiler, use both a separate build directory and
`-DLOCKD_OUTPUT_ROOT=<separate-output-directory>` to avoid overwriting binaries. For Python wheel
builds, select a separate backend directory with `-Cbuild-dir=<directory>` too.

Shared build tools and caches use `output/obj/<host-arch>/opt/shared/`. They are shared between
target profiles. CI source extraction, packaging work, and verification files also stay beneath that
tree. Collected release assets use `output/<host-arch>/opt/static/`.

Fully static executables aren't supported on macOS; use `-DLOCKD_STATIC_RUNTIME=OFF` for a static
SDK there. Statically linked Linux executables may still need matching glibc name-resolution
modules.

## Build versions

CMake refreshes the assigned project version when configuring or reloading a Git checkout.
Development builds display a `-dev` suffix. Release source archives carry their version and build
offline without Git.

Reload CMake to pick up a newly assigned version. Don't edit the generated version header. To
configure without fetching version tags, use `-DLOCKD_REFRESH_VERSION=OFF`; offline builds otherwise
fall back to locally available version information.
