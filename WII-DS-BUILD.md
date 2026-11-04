# Wii-DS Link Windows build, v0.2.0-beta.3

Upstream base: melonDS 1.1, b86390e4428bf38ce4c1ce0e9ca446d6d25955e8.
Source tag: `wii-ds-link-v0.2.0-beta.3`. Restore pinned submodules recursively.
This is an identity/Mystery Gift fix, not an upstream version upgrade.

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

The exact retail-tested EXE is preserved, including its `WiiDS Mystery Gift
Test` label and embedded aca81fea...-working-mg baseline metadata. It was built
from that base plus this tag's functional identity changes before publication
documentation was added. Release build records identify the matching source.
EXE SHA256:
09f3bd5d275d5adf9a2e5d276df255c5a0eb597f0751a612f0d134305173d2a5.
Rebuilds need not be bit-for-bit identical. No Playnite customization is included.

The exact new pair received Battle Revolution Mystery Gift with Pearl and a
non-default parent address; a second attempt succeeded too. Automated checks
covered nine parent-identity cases, legacy/automatic lifecycle, eight DS
contexts and ten standalone frontend modes. Prior Ranch and four-player
Battle Revolution retail results are from beta.1, not freshly repeated here.
No fixed native macOS binary is included in this Windows release.
