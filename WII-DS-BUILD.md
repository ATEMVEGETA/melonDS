# Wii-DS Link Windows build, v0.2.0-beta.4

Upstream base: melonDS 1.1, b86390e4428bf38ce4c1ce0e9ca446d6d25955e8.
Source tag: `wii-ds-link-v0.2.0-beta.4`. Restore pinned submodules recursively.
This is a Download Play compatibility update, not an upstream version upgrade.

The compact Windows x64 EXE uses MSYS2 UCRT64 GCC 16.1.0, CMake 4.3.4,
Ninja and Qt 6.8.3. Static dependencies use vcpkg baseline
dd3097e305afa53f7b4312371f62058d2e665320. The manifest, static triplet and
faad2 overlay are included. Dependency code was reused unchanged.

With the UCRT64 toolchain in PATH and the pinned vcpkg checkout available:

```sh
cmake --preset release-mingw-x86_64 -B build-wiids \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_STATIC=ON -DUSE_VCPKG=ON \
  -DUSE_QT6=ON -DPORTABLE=ON -DENABLE_DEBUG_DEPS=OFF \
  -DVCPKG_ROOT=/path/to/vcpkg \
  -DVCPKG_TARGET_TRIPLET=x64-mingw-static-release \
  -DVCPKG_HOST_TRIPLET=x64-mingw-static-release
cmake --build build-wiids --parallel 2
```

Use a platform-appropriate absolute path for VCPKG_ROOT with native Windows
CMake. A complete matching dependency prefix can be reused with
VCPKG_MANIFEST_INSTALL=OFF and VCPKG_INSTALLED_DIR. The project can also build
dynamically with the upstream guide; those are not the compact release EXE.

The public EXE is rebuilt with release branding from the same functional code
as the candidate on which the tester confirmed Driver: San Francisco (PAL)
connectivity. RELEASE-BUILD.json records exact source revisions and EXE hashes;
SHA256SUMS.txt records archive hashes. Automated runtime checks apply to the
publication build; a metadata-only rebuild is not a separate retail test.
Rebuilds need not be bit-for-bit identical. No Playnite customization is included.

The generic beacon encoder uses the declared GameInfo length instead of
padding short post-download advertisements. Automated generated-packet tests
cover all 113 valid user-data lengths, bounded fallback and phase transitions;
restoring the old fixed-size encoder fails the short-record negative control.
Lifecycle, parent-identity and eight-client regressions passed. Optional
diagnostics use bounded buffering; they remain off by default.

Battle Revolution Mystery Gift receipt/repeat was confirmed on beta.3; Ranch
and four-player Battle Revolution results are historical beta.1 coverage, not
fresh complete retail reruns for beta.4. The separate beta.4-macos.1 tester
packages include this Driver fix and passed native automated checks, but are
not Mac gameplay certification. Their build records identify the exact source
pins independently of later documentation-only commits.
