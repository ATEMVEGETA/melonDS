# Local Wii–DS connection

This unofficial fork adds a native Wii–DS radio bridge to official melonDS 1.1.
It is not an official upstream release. Its companion Dolphin fork is required.

In **Config → Wi-Fi settings**, enable **Connect to Dolphin** before starting
the game. The preference is off by default and is saved per emulator instance
as `Instance*.WiiDS.Automatic` in `melonDS.toml`. Enable **Connect to melonDS**
in the companion Dolphin fork. Either emulator may be opened first; launch,
pause, stop, and restart do not require an external relay or console window.
The games' own connection screens still initiate the actual link.

Automatic discovery does not disable normal DS–DS wireless. The guest selects
the Wii radio only when it sends authentication or association to the
discovered Wii parent. Disconnects clear this routing. A lost selected parent
is delivered once through the core's existing host-loss path, preventing the
former shutdown freeze caused by falling into LocalMP's timed receive.

This multi-DS release uses one exclusive nonblocking IPv4 loopback
socket per melonDS instance, selected from ports 7065 through 7080. One Dolphin
can track up to 15 associated DS clients; each game still imposes its own
player limit. Both Launch new instance and separate processes are supported
by the transport. Enable Connect to Dolphin in every participant before
loading its game. A socket-slot suffix gives each guest a distinct in-memory
firmware MAC without changing the saved firmware or settings.

An M2-capable Dolphin sends round-tagged commands and collects real replies
from every targeted DS before returning a shared failure mask. Missing peers
remain failures; a one-second host watchdog bounds an unfinished round. The
older type-7 single-client transport remains supported for legacy companions,
but do not mix old and new builds in one multiplayer session.
It is not internet play, physical Wi-Fi, a real-console bridge, or a claim of
universal Wii–DS compatibility. Native Download Play code is transferred by
the running Wii game and executed by the DS guest from RAM; the original
inserted cartridge and save remain attached. No cached client ROM, synthetic
game response, or title-specific CPU patch is supplied.

Optional Wii–DS diagnostic logging is off by default. If enabled, scalar
transport events are appended to `wii-ds-bridge.log` in the working directory.
The log contains no game payload, Pokémon data, or RAM dumps. High-frequency
records are sampled after the first 16 events and logging stops at 8 MiB.
Delete the log as needed after troubleshooting.

Developer launch overrides `--wii-ds` (forced Wii backend) and `--no-wii-ds`
(off) take priority over saved settings for that launch. The normal menu
preference uses automatic discovery, not the forced backend.

Earlier experimental builds were verified with My Pokémon Ranch and Pokémon
Diamond/Pearl, and Pokémon Battle Revolution with a correctly paired DS save.
For v0.2.0-beta.1, the tester reported successful four-player Battle Revolution,
one-player disconnect/rejoin while the others stayed connected, and a normal
Ranch transfer/save/shutdown regression. Automated transport checks exercised
eight DS contexts and separate processes. Eight-player retail gameplay and
every other Wii-DS title have not been established by these tests.
Games, BIOS, firmware, NAND, saves, and captured game payloads are not included.

## Local Windows build

Use an MSYS2 UCRT64 toolchain with Qt 6, SDL2, libarchive, zstd, faad2, and ENet.
Put `C:\msys64\ucrt64\bin` first in PATH, then run:

```
cmake -S . -B build-release-qt6 -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_QT6=ON -DPORTABLE=ON
cmake --build build-release-qt6 --parallel 2
```

The source and bridge additions are licensed under GPL-3.0-or-later, as stated
in melonDS's source headers and LICENSE. Preserve upstream and dependency
notices when preparing a distributable package.

The upstream 1.1 tag has an unrelated Qt 5 metatype registration build error
in the cheat dialogs with this toolchain. This candidate uses Qt 6, preserving
the upstream cheat sources rather than importing the custom Playnite fix.
