# Wii-DS Link Windows build, v0.2.0-beta.2

Upstream base: melonDS 1.1, commit
`b86390e4428bf38ce4c1ce0e9ca446d6d25955e8`. The modified source is tagged
`wii-ds-link-v0.2.0-beta.2` in this repository. Initialize upstream submodules
when building a fresh checkout. The normal upstream build guide still applies.

The published Windows x64 executable is a compact static build using MSYS2
UCRT64 GCC 16.1.0, CMake 4.3.4 and Ninja 1.13.2. Its Qt 6.8.3/dependency
versions come from vcpkg baseline
`dd3097e305afa53f7b4312371f62058d2e665320`; the manifest, static triplet and
faad2 overlay port are included in this source. Qt and dependency notices are
included in the binary ZIP. Dependencies were reused from the original
validated binary cache without changing their code.

In a UCRT64 shell with the compiler/CMake/Ninja in PATH, clone the pinned
vcpkg baseline into a separate directory and run:

```sh
cmake --preset release-mingw-x86_64 -B build-wiids \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_STATIC=ON -DUSE_VCPKG=ON \
  -DUSE_QT6=ON -DPORTABLE=ON -DENABLE_DEBUG_DEPS=OFF \
  -DVCPKG_ROOT=/path/to/vcpkg \
  -DVCPKG_TARGET_TRIPLET=x64-mingw-static-release \
  -DVCPKG_HOST_TRIPLET=x64-mingw-static-release \
  '-DMELONDS_VERSION_SUFFIX= - WiiDS Link WiFi Settings Fix' \
  -DMELONDS_EMBED_BUILD_INFO=ON \
  -DMELONDS_GIT_BRANCH=codex/wifi-settings-per-instance \
  -DMELONDS_GIT_HASH=ced7bb74087235f451b113f7cbb23975408bd1c2 \
  -DMELONDS_BUILD_PROVIDER=WiiDSLinkUnofficial
cmake --build build-wiids --parallel 2
```

Use a platform-appropriate absolute Windows path for VCPKG_ROOT if required
by the native Windows CMake. Dependency installation can take a long time on
a fresh machine. With a complete matching dependency prefix, setting
`VCPKG_MANIFEST_INSTALL=OFF` and `VCPKG_INSTALLED_DIR` allows reusing it.

The embedded Git hash above is the complete functional-code commit used to
build this hotfix. The release tag includes that code plus updated publication
documentation; documentation-only changes did not trigger a binary rebuild.
The window title contains `WiFi Settings Fix`. SHA-256:
`a9bf19529ef0fec7778e40af9131c2fbb59334d11698dcd5b8d4cfbaf7a0f931`.
Rebuilds need not be bit-for-bit identical.

The bridge implementation's tests include eight production transport contexts,
separate processes, per-player lifecycle, command/reply correlation, actual
Wi-Fi core hooks, and isolated static-executable startup. User-reported retail
coverage on v0.2.0-beta.1 is four-player Battle Revolution and Ranch regression
flows. This UI hotfix passed actual Qt dialog/controller and TOML persistence
checks, ten standalone frontend modes and the existing eight-client transport
suite. Fresh retail testing of this hotfix has not been confirmed. Capacity 15
and automated eight-client tests do not establish universal compatibility.
