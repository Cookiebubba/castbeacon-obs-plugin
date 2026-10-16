# Cast Beacon OBS Plugin

A small, faceless OBS Studio plugin (GPLv2) that gives the Cast Beacon app a copy of
OBS's program feed.

When you press **Start Streaming** in OBS, the plugin adds a second, hidden RTMP output
to Cast Beacon's local relay at `rtmp://127.0.0.1:1935/service`. The relay fans that out
as HLS to the overflow TVs and serves the operator's low-latency monitor. Your broadcast
to Twitch/YouTube/Facebook is never touched.

There is **no UI** — nothing to click in any OBS menu. The plugin is configured entirely
by the Cast Beacon app.

## What it does

- **Its own encoder, its own latency.** The overflow feed gets a dedicated video encoder
  cloned from your broadcast encoder, then overridden with a short keyframe interval
  (default 1s) so the TVs and the monitor stay responsive regardless of the broadcast's
  keyframe settings. Audio is shared with the broadcast (audio has no keyframes), so this
  costs one extra video encode. If a dedicated encoder can't be created (e.g. an NVENC
  session limit) it falls back to sharing the broadcast encoder.
- **Optional independent bitrate / resolution / framerate**, read from the app's
  settings file at every stream start.
- **It stands aside when OBS sends straight to Cast Beacon.** If OBS's *main* stream
  output points at this PC's relay, the operator has chosen to feed Cast Beacon directly
  (the app fanning out to the platforms itself), so the plugin starts no shadow output —
  on **any** path. On the same path it would be a second publisher (the relay allows one
  per path, so the two would knock each other off); on another path it would push OBS's
  program into `service` behind the operator's back, which can set off that stream's
  autostart or platform pushes. It stands aside when **both** hold:
  1. the output's host is **this PC**: loopback (`127.0.0.1`, `::1`, `localhost`), the
     unspecified address (`0.0.0.0`, `::`), any of this PC's own network addresses (IPv4 or
     IPv6, e.g. `192.168.1.20`), or this PC's hostname. Any other name is looked up — only
     when point 2 already holds; a name that doesn't resolve counts as another machine;
  2. the port is the relay's RTMP port (`1935`, or the scheme's default when none is
     given).

  The check runs on a worker thread, never OBS's UI thread, and the shadow output is only
  started once it has answered. A lookup gets 3 seconds: past that, the host counts as
  another machine *for now* and the shadow starts — and if the answer, when it comes, says
  this PC, the shadow is stopped.

  The path (server path + stream key, query ignored) is still compared, but only for the
  log; a path too long to hold is "unknown" and never changes the verdict. Re-checked on
  every stream start; the decision is logged once per change
  (`[castbeacon] main output check: ...`), naming the host and port and saying
  "path service", "other path" or "path unknown" — OBS's stream key, path, URL and
  credentials are never logged.

  **Not covered:** WHIP. The check reads RTMP-family URLs (`rtmp`, `rtmps`, `rtmpt`, …);
  OBS's WHIP output to this PC's relay (HTTP, a different port) is not recognised, so the
  shadow output runs alongside it.

OBS performs all encoding; this plugin only asks OBS to create and configure an encoder.

## Install (Windows)

Download the installer from the [latest release](https://github.com/Cookiebubba/castbeacon-obs-plugin/releases)
and run it. **Close OBS first** — the installer will ask you to. It needs
**administrator rights** (see below).

It installs to `C:\ProgramData\obs-studio\plugins\castbeacon`.

> ⚠️ **1.3.0 installed to the wrong place.** It used
> `%APPDATA%\obs-studio\plugins\castbeacon`, which **OBS does not scan on Windows** — the
> plugin simply never loaded, with no error and no log line. The per-user plugin folder is
> a macOS/Linux convention; on Windows OBS's external plugin root is `%ProgramData%`
> (`AddExtraModulePaths()` in `frontend/widgets/OBSBasic.cpp` calls `GetProgramDataPath`
> there, `GetAppConfigPath` everywhere else). Fixed in **1.3.1** — upgrade, and the
> installer cleans up the misplaced folder for you.

**Why administrator?** `%ProgramData%` is machine-wide. Its default permissions let a
standard user *create* files but not overwrite or delete files another account created,
which is exactly what an upgrade does — so the installer asks for elevation up front
rather than working today and failing on the next upgrade.

### Upgrading from 1.3.0 or earlier

Version 1.3.0 renamed the module from `castpilot` to `castbeacon`. OBS loads **every**
plugin folder it finds, so an old `castpilot` folder left alongside the new one means two
copies of the plugin, two feeds publishing to one relay path, and a connection that drops
every few seconds.

The installer handles all of it. Before copying anything it deletes:

- `C:\ProgramData\obs-studio\plugins\castpilot` — the one that matters. OBS scans here, so
  a hand-made install of the old module keeps it loading alongside the new one.
- `%APPDATA%\obs-studio\plugins\castpilot` and `...\castbeacon` — inert on Windows, but
  1.3.0's installer left one on every machine that ran it.

It also **detects a copy inside OBS's own program folder** (e.g.
`C:\Program Files\obs-studio\obs-plugins\64bit\castpilot.dll`), names the exact paths, and
offers to remove it — that one causes the same double load and is the reason the installer
needs elevation anyway.

### Installing from the zip instead

Delete `C:\ProgramData\obs-studio\plugins\castpilot` and
`%APPDATA%\obs-studio\plugins\{castpilot,castbeacon}` **first**, then extract the release
zip so you end up with:

```
C:\ProgramData\obs-studio\plugins\castbeacon\bin\64bit\castbeacon.dll
C:\ProgramData\obs-studio\plugins\castbeacon\data\
```

Confirm it worked: OBS's log should contain `[castbeacon] loaded (v1.3.2)`. If the log
mentions `castbeacon` nowhere at all, the files are somewhere OBS does not scan.

**Do not delete `%APPDATA%\obs-studio\plugin_config\castpilot\settings.json`.** That is
the app's settings file, not a leftover: Cast Beacon 2.1.1 and older write only to that
path, and the renamed plugin still reads it as a fallback. Removing it resets your
overflow encode settings. (Settings genuinely do live under `%APPDATA%` — OBS derives
that folder from the module's *file name*, not from where the DLL sits, so moving the
install to `ProgramData` does not move the settings.)

## Settings

The Cast Beacon app writes `settings.json` into the plugin's OBS module config folder.
The plugin reads it fresh at every stream start, so changes apply on the next
stop/start — OBS does not need restarting.

| Path | Written by | Read by 1.3.0 |
|---|---|---|
| `%APPDATA%\obs-studio\plugin_config\castbeacon\settings.json` | Cast Beacon 2.1.2+ | first choice |
| `%APPDATA%\obs-studio\plugin_config\castpilot\settings.json` | Cast Beacon 2.1.1 and older | fallback, when the file above is absent |

```jsonc
{
  "keyint_sec": 1,        // keyframe interval, seconds (default 1 = low latency)
  "bitrate_kbps": 0,      // 0 = match the broadcast encoder
  "width": 0,             // 0,0 = match the canvas (no rescale)
  "height": 0,
  "fps": 30,              // absolute target fps; snapped to an integer divisor of the canvas fps
  "shadow_enabled": true  // absent = true. false = never start the overflow output
}
```

`shadow_enabled: false` is the manual off switch, for operators who feed the relay some
other way and don't want to uninstall the plugin.

## Platform support

Windows x64 is built, packaged and shipped. macOS and Linux compile in CI but are
**unexercised** — no installer flow has been tested there.

## Building

Standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate) layout —
its [wiki](https://github.com/obsproject/obs-plugintemplate/wiki) covers the build system.

| Platform | Tool |
|---|---|
| Windows | Visual Studio 17 2022, CMake 3.30.5 |
| macOS | Xcode 16.0, CMake 3.30.5 |
| Ubuntu 24.04 | CMake 3.28.3, `ninja-build`, `pkg-config`, `build-essential` |

Releases are cut by pushing a **bare** semantic-version tag (`1.3.0` — no `v` prefix) to
`main`. CI builds all platforms, packages the Windows installer, and opens a draft
release with the artifacts attached.

### Unit tests

`tests/` is a CMake project of its own — it needs neither libobs nor the plugin template,
so it builds anywhere a C compiler does. CI runs it on Windows, macOS and Ubuntu, and a
release is not drafted unless it passes.

```
cmake -S tests -B build_tests
cmake --build build_tests --config Release
ctest --test-dir build_tests -C Release --output-on-failure
```

## Changelog

- **1.3.2** — The stand-aside check recognises this PC by any of its own addresses and by
  its hostname, not only by `127.0.0.1` / `localhost` / `::1`: OBS pointed at the relay by
  the PC's LAN address no longer gets a second publisher on the same path (the two
  knocked each other off in a reconnect loop). OBS pointed at this PC's relay on *any*
  path means "OBS sends straight to Cast Beacon", so the shadow stands aside and never
  publishes into `service` behind the operator's back; the path is compared for the log
  only. `0.0.0.0` and `::` count as this PC. The check (and any host-name lookup) runs off
  OBS's UI thread; a lookup slower than 3 s lets the shadow start, and stops it again if the
  answer says this PC. A stuck lookup no longer holds up OBS's exit. Unit tests added
  (`tests/`), including a harness for the lookup with a fake resolver.
- **1.3.1** — Installs to `C:\ProgramData\obs-studio\plugins\castbeacon`, where OBS
  actually scans on Windows.
- **1.3.0** — Module renamed `castpilot` → `castbeacon`; the old settings path is still
  read as a fallback.

## License

GPLv2 — see [LICENSE](LICENSE). This plugin links libobs and is therefore GPL; the Cast
Beacon app is a separate, proprietary program in a separate process. Neither vendors the
other's code.
