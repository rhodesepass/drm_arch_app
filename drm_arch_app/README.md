![app_banner](./docs/assets/drm_app_banner.png)

# drm_arch_app

`drm_arch_app` is the current GUI runtime used by recent ArkEPass Arch-based
firmware images.

It is not a generic Linux desktop app and it is not a standalone firmware
tree. This directory contains the display runtime, content orchestration,
third-party app launcher, and IPC surface that sit inside the larger
ArkEPass system.

## What This Repository Segment Is

Within the ArkEPass stack, the local `epass-arch` tree sits between an
app-only source tree and a full firmware tree:

- [`rhodesepass/drm_app_neo`](https://github.com/rhodesepass/drm_app_neo):
  app-centric repository, best for studying the GUI/player code in isolation.
- [`rhodesepass/buildroot`](https://github.com/rhodesepass/buildroot):
  full firmware tree, best for bootloader/kernel/rootfs/image assembly work.
- `epass-arch` (this local tree): the current Arch runtime subtree that wires
  `drm_arch_app` into systemd, shared-data mounting, USB gadget control,
  first-boot flow, and SD image assembly.

If the question is "which system is more complete?", the answer is
`buildroot`. If the question is "which code reflects the current ArkEPass
Arch runtime most directly?", the answer is `epass-arch`. If the question is
"which repo is easier for understanding the display app itself?", the answer
is `drm_app_neo`.

## System Comparison

| Repository | Scope | What It Does Well | What It Does Not Cover Well | Best Use |
| --- | --- | --- | --- | --- |
| `drm_app_neo` | GUI application source | Focused app reading, DRM/LVGL/player internals, smaller code surface | Does not represent the current Arch runtime, systemd flow, persisted USB mode, shared-data import path | Study the app core |
| `buildroot` | Full firmware tree | Bootloader, kernel, rootfs, packages, board files, image generation, full system truth | Large surface area; slower as an entry point for app/runtime iteration | Full system work |
| `epass-arch` | Current Arch runtime subtree | Current `drm_arch_app`, deploy scripts, runtime services, USB/shared-data/first-boot behavior | Not the full board/firmware stack by itself | Current runtime maintenance |

## Why `drm_arch_app` Matters

This directory is more than a "player":

- It owns the visible GUI runtime on the device.
- It drives a 3-plane display pipeline: video, overlay, and LVGL UI.
- It contains PRTS, the content scheduling layer for operators, transitions,
  loop videos, and overlays.
- It scans and launches third-party apps from `/app`.
- It exposes a local IPC socket used by external apps and helpers.
- It integrates with runtime settings such as brightness, theme selection,
  media path switching, and USB mode requests.

## Runtime Architecture

The current build is tightly coupled to the ArkEPass hardware and display
stack:

- Allwinner/sun4i-style DRM pipeline with a custom atomic commit ioctl
- LVGL for the UI layer
- CedarX-backed media path when `ENABLE_CEDARX=ON`
- Current screen profile pinned to `360x640`

High-level module layout:

- `src/driver/`: DRM backend, vblank-driven display queue, input handling
- `src/render/`: video decode/display, LVGL rendering bridge, animations
- `src/overlay/`: transitions and operator info overlays
- `src/prts/`: Playlist Routing & Transition System
- `src/apps/`: third-party app catalog, launcher, background app tracking, IPC
- `src/ui/`: UI behavior and screen actions
- `generated_ui/`: EEZ Studio export consumed by the build

Further design notes live in:

- [docs/application_structure.md](./docs/application_structure.md)
- [docs/overlay_dev_note.md](./docs/overlay_dev_note.md)

## Startup And Service Boundaries

`drm_arch_app` does not boot in isolation. In the current Arch runtime it is
started by `drm-arch-app.service`, with these important boundaries:

1. First-boot flow gates normal startup.
2. Boot animation starts before the app.
3. `screen-detect.service` runs before the app.
4. `drm-arch-app.service` runs the GUI preflight and launches the app runner.
5. The app marks GUI readiness by creating `/run/epass/gui-alive`.
6. A path unit stops the boot animation after the GUI is alive.
7. Shared-data mount and persisted USB mode restoration happen after the GUI
   service starts, not before it.

That last point matters: the current runtime does **not** wait for
`epass-data-mount.service` or `epass-usb-mode.service` before launching the
GUI. The app starts against the base rootfs paths first, then shared-data bind
mounts and USB gadget restoration are filled in later by runtime services.

The runner also interprets app exit codes:

- `0`: normal exit
- `1`: restart the GUI
- `2`: launch a foreground third-party app via `/tmp/appstart`
- `3`: request poweroff
- `5`: hand off to `srgn_config`, then return to the settings screen

## Runtime Contracts

These paths are part of the current runtime contract:

| Path | Purpose |
| --- | --- |
| `/assets/` | Primary operator/content asset catalog |
| `/dispimg/` | Display-image catalog |
| `/app/` | Installed third-party app catalog |
| `/root/res/` | Built-in UI assets and fallbacks |
| `/root/themes/` | Theme packages |
| `/root/epass_cfg.bin` | Persisted settings store |
| `/tmp/drm_arch_app.sock` | Local IPC socket |
| `/run/epass/drm_arch_app.log` | Persistent app log for the current boot |
| `/run/epass/gui-failure-reason` | Failure summary written by the runner |
| `/run/epass/gui-alive` | GUI-ready marker used by the boot animation handoff |

Current optional scan mode:

- `drm_arch_app sd`
  - enables additional scans from `/sd/assets/` and `/sd/app/`
- `drm_arch_app version`
  - prints build/version metadata and exits

## Content Model

### Operator / PRTS content

Operator content is scanned from subdirectories under `/assets/`. Each package
is expected to contain:

- `epconfig.json`
- referenced loop video assets
- optional intro video assets
- optional icon/overlay assets

At minimum, the parser expects version, uuid, screen, loop media, and valid
transition/overlay fields. The current build only accepts content targeting
`360x640`.

Minimal example:

```json
{
  "version": 1,
  "name": "Example Operator",
  "uuid": "12345678-1234-1234-1234-123456789abc",
  "screen": "360x640",
  "loop": {
    "file": "loop.mp4"
  },
  "intro": {
    "enabled": false,
    "duration": 0
  },
  "transition_in": {
    "type": "none"
  },
  "transition_loop": {
    "type": "none"
  },
  "overlay": {
    "type": "none"
  }
}
```

### Third-party apps

Installed apps are scanned from subdirectories under `/app/`. Each app package
is expected to contain:

- `appconfig.json`
- the referenced executable
- optional icon assets

The parser currently expects:

- `version`
- `uuid`
- `type` (`bg`, `fg`, or `fg_ext`)
- `screens`
- `executable`

Foreground apps are launched through `/tmp/appstart` and the systemd runner.
Background apps are forked and tracked by the app runtime.

Minimal example:

```json
{
  "version": 1,
  "name": "Example App",
  "uuid": "12345678-1234-1234-1234-123456789abc",
  "type": "fg",
  "screens": ["360x640"],
  "description": "Example foreground app",
  "executable": {
    "file": "run.sh"
  },
  "icon": "icon.png"
}
```

The loader also accepts a compatibility form where `executable` is a plain
string instead of an object.

### Shared-data import path

The shared-data partition is mounted at `/mnt/epass-data`. In the current
runtime:

- `/mnt/epass-data/assets` is bind-mounted to `/assets`
- `/mnt/epass-data/display-images` is bind-mounted to `/dispimg`
- `/mnt/epass-data/themes` is bind-mounted to `/root/themes`

`/app` is different:

- `/app` is the live installed app directory on the rootfs
- `/mnt/epass-data/apps-inbox` is the import inbox
- `epass-app-import.sh` copies or unpacks app packages from the inbox into
  `/app`

That distinction is intentional. App inbox content is not executed directly
from the shared-data mount.

## Theme Support

Themes are loaded from `/root/themes`, with the active selection recorded in
`/root/.epass_active_theme`.

Themes can influence:

- primary/secondary UI colors
- background color
- text color
- themed resource overrides

If no valid external theme is found, the runtime falls back to a built-in
theme and built-in fallback assets under `/root/res/fallback/`.

## Build

### Recommended: Build through the surrounding Buildroot tree

From the Buildroot repository root:

```bash
./epass-arch/build_drm_arch_app.sh
```

This path uses the Buildroot package recipe and stages shared LVGL sources into
the package build directory.

### App-only cross-build

From the Buildroot repository root:

```bash
source output/host/environment-setup
cmake -S epass-arch/drm_arch_app -B epass-arch/drm_arch_app/build -DCMAKE_BUILD_TYPE=Debug
cmake --build epass-arch/drm_arch_app/build -j"$(nproc)"
```

Relevant notes:

- direct tree builds expect LVGL at `../third_party/lvgl`
- Buildroot package builds stage LVGL into `third_party/lvgl`
- CedarX can be disabled for limited bring-up work:

```bash
cmake -S epass-arch/drm_arch_app -B epass-arch/drm_arch_app/build -DCMAKE_BUILD_TYPE=Debug -DENABLE_CEDARX=OFF
cmake --build epass-arch/drm_arch_app/build -j"$(nproc)"
```

## EEZ Studio Workflow

`generated_ui/` is build input, but it is not the source of truth.

Use this workflow:

1. Edit the EEZ Studio project in `../ui_design/epass_eez/`
2. Export the generated UI into `generated_ui/`
3. Rebuild `drm_arch_app`

Do not hand-maintain `generated_ui/*` unless you are intentionally repairing an
export issue. Normal UI work should be done from the EEZ project.

## Debugging Entry Points

Useful runtime checks on device:

```bash
systemctl status drm-arch-app.service --no-pager -l
journalctl -b -u drm-arch-app.service -u screen-detect.service -u epass-usb-mode.service --no-pager
tail -n 120 /run/epass/drm_arch_app.log
cat /run/epass/gui-failure-reason
```

Useful helper scripts from the runtime layer:

- `/usr/local/bin/epass-mem-report.sh`
- `/usr/local/bin/epass-usb-report.sh`
- `/usr/local/bin/drm-arch-app-runner.sh`

## Related Directories

- `src/`: maintained application source
- `generated_ui/`: EEZ-exported UI code used by the build
- `docs/`: design notes for the app runtime
- `../deploy/`: systemd units, runtime scripts, USB/shared-data/first-boot logic
- `../third_party/lvgl/`: shared LVGL source used by direct-tree builds

## License And Attribution

This project embeds or links against several upstream components, including
LVGL, libdrm, libevdev, libpng, CedarX-side runtime libraries, and other
small utilities already referenced in the surrounding source tree.

Game-related assets remain the property of their original copyright holders.
