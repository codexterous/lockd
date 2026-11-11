# Contributing

Use the [build instructions](BUILDING.md), make your change, and run the relevant
[tests](TESTING.md). Command-line CMake is the primary interface; editors use the same targets and
dependencies.

## Editor setup

CLion is the preferred IDE. The repository includes shared run configurations for the daemon, native
and Python tests, clang-tidy, wheel builds, and Windows installers. CMake presets provide the build
profiles; other editors can use the same command-line build and test targets.

Open the repository as a CMake project and select a preset matching your compiler and architecture.
CMake exports `compile_commands.json` in the build directory for editors that need a compilation
database.

In CLion, select an imported CMake profile and build normally. On Windows, use the compiler
environment matching the profile's architecture.

The **Testing** folder provides direct native test runs. Use CTest from the command line for the
tests registered with CMake. **Python Testing** runs the tests against your installed extension;
build the daemon and install the wheel first. See [Python
development](#python-development-in-a-cmake-ide) for the wheel targets and `.venv` setup.
[BUILDING.md](BUILDING.md#building-installers) covers the installer build configurations.

## Formatting

Use four spaces, a 100-column margin, and eight-space continuation indentation in C++. Keep up to
two existing blank lines. Put function and class opening braces on a new line, leave namespace
contents unindented, and attach pointer and reference markers to the type (`T*`, `const T&`). Keep
adjacent method declarations together in headers. Python uses an 88-column margin. Markdown prose
wraps at 100 columns, with exceptions for tables, code examples, and long links.

C and C++ contributions must follow `.clang-format`. Use it in both your editor and command-line
checks; don't maintain a separate personal formatting style. Enable reformat-on-save for C/C++
sources and headers. Before submitting, run `clang-format --style=file --dry-run --Werror` on the
files you changed and resolve formatting differences. Review the diff, especially manually arranged
declarations and grammars. Include ordering and comment wrapping are preserved.

The shared CLion **Project** scheme enables Clang-Format using the IDE's bundled executable. In
**Settings / Editor / Code Style / C/C++**, select **Clang-Format**. Under **Tools / Actions on
Save**, enable **Reformat code**, select C/C++ sources and headers, and choose **Whole file**.
Actions on Save is a local workspace setting, so each contributor must enable it.

Other editors must use clang-format with `style=file` and format-on-save enabled. `.editorconfig`
supplies indentation and margins to editors with EditorConfig support. Wrapped parameters and
arguments use eight-space continuation indentation, with a break after the opening parenthesis when
the list wraps and the closing parenthesis on its own line. Short calls stay on one line.
clang-format applies this setting to both function signatures and calls, not arbitrary parenthesized
expressions.

### Running clang-format

CLion's **Formatting / clang-format (whole project)** run configuration formats every project C/C++
source and header, including nonignored new files, without building first. It uses the project
Python interpreter and skips ignored build output, dependencies, and private files. The same command
works from a terminal:

```sh
python scripts/clang_format.py
python scripts/clang_format.py --check
```

The helper uses standalone LLVM on Windows or `clang-format` on `PATH`. Pass `--clang-format PATH`
to select another installation.

Install clang-format 20 or newer and make it available on `PATH`. Check its version first; an older
copy bundled with Visual Studio or an IDE may not understand this configuration. From the repository
root, preview a file without changing it, check it, or format it in place:

```sh
clang-format --version
clang-format --style=file source/mutex.cpp
clang-format --style=file --dry-run --Werror source/mutex.cpp
clang-format --style=file -i source/mutex.cpp
```

For a deliberate formatting-only commit, format tracked C and C++ sources. This avoids generated
files and downloaded dependencies. In PowerShell:

```powershell
git ls-files -- '*.c' '*.cpp' '*.h' '*.hpp' | ForEach-Object {
    clang-format --style=file -i -- $_
    if ($LASTEXITCODE -ne 0) { throw "clang-format failed for $_" }
}
```

On Linux or FreeBSD:

```sh
git ls-files -z -- '*.c' '*.cpp' '*.h' '*.hpp' | xargs -0 clang-format --style=file -i --
```

Use `--dry-run --Werror` instead of `-i` to check those files without modifying them. Keep the
formatting commit separate from behavior changes and review the diff before committing.

### Running Ruff

Python contributions must pass Ruff's formatting and lint checks. Install the developer requirements
into your virtual environment from the repository root (requires pip 25.1 or newer):

```sh
python -m pip install --group dev
```

This installs development tools without building or reinstalling pylockd. Ruff uses `pyproject.toml`
for its configuration: Python 3.11 compatibility, 88-column formatting, and preserved string quotes.

Compatibility checks run Ruff lint and formatting checks on all tracked Python files for every
commit and pull request. Release packaging applies the same checks. Either failure blocks packaging;
CI checks formatting without rewriting files.

Use `ruff format` for formatting and `ruff check` for lint diagnostics. Keep lint fixes separate
from the formatting-only commit; don't apply `--fix` or `--unsafe-fixes` as part of that commit.

To format all tracked Python files in PowerShell, with the virtual environment activated:

```powershell
$pythonFiles = @(git ls-files -- '*.py')
python -m ruff format -- $pythonFiles
if ($LASTEXITCODE -ne 0) { throw 'Ruff formatting failed' }
python -m ruff format --check -- $pythonFiles
if ($LASTEXITCODE -ne 0) { throw 'Ruff formatting check failed' }
python -m ruff check -- $pythonFiles
if ($LASTEXITCODE -ne 0) { throw 'Resolve the Ruff lint findings' }
```

On Linux or FreeBSD:

```sh
git ls-files -z -- '*.py' | xargs -0 python -m ruff format --
git ls-files -z -- '*.py' | xargs -0 python -m ruff format --check --
git ls-files -z -- '*.py' | xargs -0 python -m ruff check --
```

These commands exclude untracked files, generated output, and downloaded dependencies. Add new
source files to Git before including them in a full formatting pass. For routine work, pass the
files you changed instead. Review `git diff` and run the relevant tests before committing formatting
changes.

## Throwing exceptions

When throwing `lld::exception`, include `<liblockd/detail/source_path.hpp>` and use
`LOCKD_SOURCE_FILE`, `__LINE__`, and `__FUNCTION__` for source metadata. `LOCKD_SOURCE_FILE` removes
the CMake-provided repository root at compile time, including in headers, so exceptions don't expose
build-machine directories or process paths when thrown. Choose the appropriate `et_` category,
provide a useful error message, and catch exceptions by reference.

## Before submitting a change

Keep changes focused and follow the surrounding C++ style. Add tests for changed behavior, including
failure and cleanup paths where relevant. Run CTest and clang-tidy; Python changes also need the
installed-extension tests.

Include the configuration and results with your change, and say which platforms you haven't tested.
Don't weaken warnings or assertions to make a build pass. Update the relevant documentation when
behavior changes.

All project C++ targets use strict warnings in every build profile: GCC/Clang builds use
`-Wall -Wextra -Wpedantic -Werror`, and MSVC builds use `/W4 /WX`. This includes the daemon, client
libraries, native tests, and Python bindings. Contributions must compile without warnings under
these settings; new targets must use the [shared policy](cmake/lockd_build.cmake).

Keep necessary warning suppressions around only the third-party headers that require them, using
compiler guards and matching diagnostic push/pop directives. Document the diagnostic and why the
exception is needed; don't disable it across a target.

### Preparing for release

For release checks, format the changed C and C++ sources and headers, then verify them with
`clang-format --style=file --dry-run --Werror`. Run Ruff formatting and lint checks on changed
Python files.

Run clang-tidy on changed C and C++ translation units using the matching `compile_commands.json`,
the shared `.clang-tidy` policy, and warnings as errors. Check changed headers through the including
translation units needed to cover the affected code and platform branches. Include the checked
platforms and any coverage gaps in the release review.

The `clang_tidy` and `clang_tidy_full` targets rescan their configured components, including
unchanged files; use them for full scans.

### Signing Windows releases

The Release workflow signs the portable daemon and native test executables for all three Windows
architectures, rebuilds the installers with those signed payloads, then signs the installers. It
uses SHA-256 file digests and RFC 3161 timestamps. SignTool and Windows Authenticode verification
must pass before publication, and release checksums are generated after signing.

The signing job uses the GitHub environment `lockd`. Configure these environment secrets:
`AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, and `AZURE_SUBSCRIPTION_ID`. Configure these environment
variables: `AZURE_SIGNING_ACCOUNT_NAME`, `AZURE_SIGNING_ENDPOINT`, and
`AZURE_CERTIFICATE_PROFILE_NAME`.

The Azure application needs a federated credential matching this repository's OIDC subject for the
`lockd` environment and the Artifact Signing Certificate Profile Signer role on the signing account
or certificate profile. Authentication uses OIDC without a client secret. See Microsoft's
[OIDC setup instructions](https://github.com/Azure/artifact-signing-action/blob/main/docs/OIDC.md).

### Publishing wheels to PyPI

Push the commit and let **Compatibility** finish, then run **Release** from GitHub Actions for that
same commit's branch. Release reuses successful checks and tested artifacts for the same commit and
assigned version. Missing or expired checks run before publication.

**Publish wheels to PyPI** uploads the validated `tested_python` artifact after release assembly and
Windows signing succeed. This includes all 30 wheels: CPython 3.11 through 3.15 for Windows and
manylinux, each on x86, x86_64, and ARM64. The publishing job downloads those files without
rebuilding or renaming them. Ordinary compatibility runs never publish to an index.

The repository Actions secret `PYPI_API_TOKEN` must contain an API token issued by the configured
package index with permission to publish `pylockd`. The upload URL is configured in the Release
workflow. The first upload of a new project requires an account-wide token; afterward, replace it
with a project-scoped token. API-token authentication uses the username `__token__`. See [PyPI's
publishing instructions](https://docs.pypi.org/publishing/publishing-to-pypi/).

The distribution name comes from `pyproject.toml`; the extension is imported as `pylockd`. PyPI
keeps uploaded filenames immutable. Retrying a partially completed publishing job skips existing
files and uploads the remaining wheels. To publish changed files, push a new commit and use its
newly assigned version. GitHub release publication remains a separate job in Release.

## Clang-tidy

Install LLVM 20 or newer and make `clang-tidy` available on `PATH`. Destructor override inspections
are excluded because LLVM 20 rejects the required explicit `virtual` with `override`. Ordinary
methods still receive override inspections. The opt-in virtual-call analyzer is excluded because
older LLVM versions diagnose Boost.Process destructor internals in system headers. On Windows, CMake
prefers standalone LLVM in the standard Program Files installation directory over Visual Studio's
bundled copy on `PATH`. Install it with `winget install --id LLVM.LLVM --exact --source winget`. Run
analysis from the same developer environment used for compilation.

CI uses the runner's preinstalled standalone clang-tidy on Windows and LLVM 23 on Linux, and logs
the selected executable's path and version.

Use `-DLOCKD_CLANG_TIDY_EXECUTABLE=/path/to/clang-tidy` to select a particular installation.
Explicit and cached selections take precedence over discovery. CLion's editor inspections have a
separate executable setting under **Languages & Frameworks > C/C++ > Clang-Tidy**; select the
standalone executable there too. The shared CMake analysis targets use the executable selected by
CMake.

For a full scan, configure the native tests and Python bindings, including matching Python and Boost
`python` development dependencies:

```sh
cmake --preset x86_64-opt-shared -DBUILD_TESTING=ON -DLOCKD_BUILD_PYTHON=ON \
    -DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON
cmake --build output/obj/x86_64/opt/shared --target clang_tidy_full
```

For that full scan in CLion, select the **clang-tidy** configuration and use **Build** with that
profile. Both routes use the repository's `.clang-tidy` policy.

Include checks require direct dependencies and flag unused headers. Use public Boost entry points
and `windows.h`, rather than adding their implementation headers to satisfy an inspection. Mark
intentional support includes with `// IWYU pragma: keep` and umbrella exports with
`// IWYU pragma: export`; explain any non-obvious ordering or template support beside the include.
The shared policy excludes known implementation headers supplied through those public entry points.

The full scan covers the daemon, client libraries, native tests, and Python bindings. It fails if a
component is missing. It checks only the current platform's enabled code, so Windows and POSIX scans
are both needed.

Review suggestions before applying them; don't run bulk `--fix` changes.

### Analysis options

Use `clang_tidy` instead of `clang_tidy_full` to scan just the components in the selected
configuration. Both targets recheck unchanged files.

Scans use two workers by default. Set `-DLOCKD_CLANG_TIDY_JOBS=4`, for example, to change that; the
outer build's `-j` does not control analysis parallelism. More workers need more memory.

Debug builds also check changed sources automatically when clang-tidy is available with a supported
CMake/generator combination. `-DLOCKD_CLANG_TIDY=OFF` disables those automatic checks without
removing the explicit targets. Automatic checks need CMake 3.25+ with Ninja or Makefiles.

If discovery fails, set `-DLOCKD_CLANG_TIDY_EXECUTABLE=<path>`. For a static-runtime profile, the
full scan can use binding commands from a matching shared profile with Python enabled. If the native
profiles omit Python, build `pylockd_wheel` first. The full scan can then use the most recent wheel
database with the same checkout, architecture, and compiler. Native sources retain the selected
profile's commands; all components remain required.

Scripts under `scripts/release/` support CI and release packaging. They aren't needed to install,
build, or test the project normally.

## Building Python from source

Install CMake 3.24+, a C++17 compiler, Python development files, and Boost `python` 1.82+ built for
the same Python version and architecture. On Windows, use a Visual Studio developer shell. From the
repository root:

```sh
python -m pip install .
```

To produce a wheel instead:

```sh
python -m pip install build
python -m build --wheel --outdir lib/pylockd/output/x86_64/opt/static
```

Unix builds use installed dependencies. Windows uses [vcpkg](BUILDING.md#windows), but its Boost
`python` library must match your interpreter; vcpkg's Python version may differ from yours. Use
`-Ccmake.define.Boost_ROOT=<prefix>` with the build command to select another Boost installation.

For an editable install, install `scikit-build-core>=1.0,<2`, then run
`python -m pip install --no-build-isolation -e .`. Reinstall after changing C++.

## Python development in a CMake IDE

Configure a shared-runtime profile with matching Python and Boost `python`:

```sh
cmake --preset x86_64-opt-shared -DLOCKD_BUILD_PYTHON=ON
cmake --build output/obj/x86_64/opt/shared --target pylockd
```

Set `-DPython3_EXECUTABLE=<path>` if CMake selects the wrong interpreter. Native builds otherwise
discover Python optionally; missing Python dependencies don't prevent a normal daemon build. Use
`-DLOCKD_BUILD_PYTHON=OFF` to omit it.

On Windows x86_64, the default `AUTO` setting can also reuse the wheel recipe's cached Boost.Python
SDK when its Boost version matches the native dependencies. The Windows recipes currently use Boost
1.88, matching the vcpkg baseline. To prepare both native Debug and Release variants, run this from
the matching x64 developer environment:

```powershell
./scripts/release/build_boost_python.ps1 -Python ./.venv/Scripts/python.exe -Variant debug,release
cmake --preset x86_64-opt-shared -DLOCKD_BUILD_PYTHON=AUTO
cmake --preset x86_64-dbg-shared -DLOCKD_BUILD_PYTHON=AUTO
```

Reload the CMake project in your editor afterward. The ordinary `pylockd` target supplies the
binding sources' compiler settings; `AUTO` keeps it out of the default native build. Build that
target explicitly when needed. Fully static profiles omit the extension because Python requires the
shared runtime; use a matching shared profile when editing the bindings.

### Building and installing a wheel from the IDE

Create the project's `.venv` with your chosen interpreter and install `build` and
`scikit-build-core>=1.0,<2` there. In CLion, choose **pylockd wheel (install into .venv)** and use
**Build**. This builds and installs the wheel into that virtual environment. Stop Python tests and
debug sessions before replacing an extension DLL on Windows.

The command-line equivalent is:

```sh
cmake --build output/obj/x86_64/opt/shared --target pylockd_wheel_install
```

Choose **pylockd wheel (build)**, or build `pylockd_wheel`, to build a wheel without installing it.
Wheels go to `lib/pylockd/output/<arch>/opt/static/`. Use your interpreter's architecture in the
`--outdir` path when building directly. Intermediate wheel builds use
`lib/pylockd/output/obj/<wheel_tag>/opt/static/`; see [the output
layout](BUILDING.md#custom-dependencies-and-troubleshooting).

These targets reuse matching dependencies from the selected profile or its shared-runtime profile.
On Windows x86_64, if those dependencies don't include Boost `python` for your interpreter, the
targets use the release tool's interpreter-specific SDK cache and build it when missing. The native
profile and Python interpreter must use the same architecture. Both targets default to `.venv`. For
build-only wheels, `LOCKD_PACKAGE_PYTHON` selects another interpreter; without `.venv` or an
explicit selection, CMake discovers an interpreter from your environment. The install target always
uses `.venv`.

Run the [Python tests](TESTING.md#python-tests) after installing the extension.
