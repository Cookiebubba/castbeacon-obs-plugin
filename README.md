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
- **It stands down when it isn't needed.** If OBS's *main* stream output is already
  pointed at the local relay (the app fanning out to the platforms itself), a shadow
  output would fight the main output for the same relay path — so the plugin doesn't
  start one. Re-checked on every stream start.

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

Confirm it worked: OBS's log should contain `[castbeacon] loaded (v1.3.1)`. If the log
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

## License

GPLv2 — see [LICENSE](LICENSE). This plugin links libobs and is therefore GPL; the Cast
Beacon app is a separate, proprietary program in a separate process. Neither vendors the
other's code.
