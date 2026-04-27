# epass-arch

`epass-arch` is the Arch Linux ARM runtime integration and GUI workspace for
ArkEPass. It contains the maintained `drm_arch_app` source tree, the runtime
deployment assets used on the device, and the SD image assembly scripts used to
build the current Arch-based system image.

This project depends on the parent Buildroot tree for the toolchain, kernel,
U-Boot, target libraries, and package integration. `epass-arch` owns the
runtime-facing part of the system: app behavior, systemd startup chain, shared
data layout, USB gadget behavior, and Arch SD image assembly.

## Documentation Index

This README is the documentation entry point for the project-owned documents in
`epass-arch/`.

Excluded from this index:

- `AGENTS.md`
- vendored documentation under `third_party/`
- bundled upstream/package documentation under `python-build/` and
  `python-install/`

### Project Overview

- [epass-arch README](README.md)

### GUI Runtime

- [drm_arch_app README](drm_arch_app/README.md)
- [Application Structure](drm_arch_app/docs/application_structure.md)
- [Overlay Layer Development Guide](drm_arch_app/docs/overlay_dev_note.md)

### UI Design / Export Tooling

- [EEZ Studio Project README](ui_design/epass_eez/README.md)
- [Font File Generation](ui_design/epass_eez/font_generate/README.md)
- [Icon Define Generation](ui_design/epass_eez/icon_header_gen/README.md)

## What This Project Covers

Current responsibilities in this directory:

- `drm_arch_app` GUI built on DRM + LVGL + CedarX
- shared-data layout for `/assets`, `/app`, `/dispimg`, and `/root/themes`
- third-party app scan/import infrastructure under `drm_arch_app/src/apps`
- persistent USB mode switching: `mtp`, `rndis`, `serial`, `none`
- firstboot hardware and screen selection
- GUI preflight, fallback, boot animation handoff, and runtime boot markers
- `srgn_config` launch path from the device UI/runtime
- Arch SD image assembly through `build-sdcard-arch.sh`

## Repository Layout

```text
epass-arch/
├── build-sdcard-arch.sh
├── build_drm_arch_app.sh
├── build_lvgl.sh
├── build_python311.sh
├── deploy/
├── drm_arch_app/
├── python-build/
├── third_party/
└── ui_design/
```

### `drm_arch_app/`

The maintained device-side GUI application.

- DRM/LVGL rendering and CedarX media playback
- main screens, settings, operator views, maintenance tooling
- third-party app loader/importer and IPC helpers
- theme handling, asset caching, and device-side utilities
- generated EEZ UI sources under `generated_ui/`

Useful entry points:

- [drm_arch_app/README.md](drm_arch_app/README.md)
- [drm_arch_app/docs/application_structure.md](drm_arch_app/docs/application_structure.md)
- [drm_arch_app/docs/overlay_dev_note.md](drm_arch_app/docs/overlay_dev_note.md)

### `deploy/`

Runtime deployment assets for the Arch image.

This directory contains the system policy that turns the app into a bootable
device image:

- `drm-arch-app.service` and runner scripts
- boot animation start/stop handoff
- GUI preflight and fallback handling
- shared-data mount and bind setup
- USB gadget control and persistent mode restore
- firstboot, resize, bootenv, screen detect, and device helpers

### `build-sdcard-arch.sh`

Builds the Arch SD image by combining:

- parent Buildroot outputs
- Arch Linux ARM rootfs tarball
- runtime deploy files
- `drm_arch_app`
- device resources and helper binaries

### `ui_design/epass_eez/`

Source of truth for the EEZ Studio UI project.

- edit the EEZ project here
- export generated code into `drm_arch_app/generated_ui/`

Useful entry points:

- [ui_design/epass_eez/README.md](ui_design/epass_eez/README.md)
- [ui_design/epass_eez/font_generate/README.md](ui_design/epass_eez/font_generate/README.md)
- [ui_design/epass_eez/icon_header_gen/README.md](ui_design/epass_eez/icon_header_gen/README.md)

Do not hand-edit the generated C files in `generated_ui/`.

### `third_party/lvgl/`

Vendored LVGL source used by the GUI build.

### `python-build/`

Support files for bundling a Python runtime into the Arch image. The generated
`python-install/` output is local build state and should not be committed.

## Build and Iteration

All commands below assume the parent Buildroot tree already contains this
directory as `epass-arch/`.

### 1. Bootstrap the parent Buildroot tree

Run from the Buildroot root:

```bash
make rhodesisland_epass_defconfig
make -j$(nproc)
```

This produces the toolchain, kernel, U-Boot, target libraries, and package
outputs consumed by `epass-arch`.

### 2. Rebuild the GUI application

Preferred path:

```bash
./epass-arch/build_drm_arch_app.sh
```

This is the normal iteration loop for:

- `drm_arch_app/src/*`
- `drm_arch_app/generated_ui/*`
- local runtime-facing app logic

You can also rebuild through Buildroot:

```bash
make drm_arch_app
```

### 3. Standalone app build with the generated toolchain

From the Buildroot root:

```bash
source output/host/environment-setup
cmake -S epass-arch/drm_arch_app -B epass-arch/drm_arch_app/build -DCMAKE_BUILD_TYPE=Debug
cmake --build epass-arch/drm_arch_app/build -j$(nproc)
```

### 4. Build the Arch SD image

Place the Arch Linux ARM rootfs tarball in the parent Buildroot root, then run:

```bash
sudo ./epass-arch/build-sdcard-arch.sh
```

The result is `sdcard-arch.img`.

## UI Workflow

UI design changes should follow this path:

1. Edit the EEZ Studio project in `ui_design/epass_eez/`
2. Export the UI to `drm_arch_app/generated_ui/`
3. Rebuild `drm_arch_app`

Do not patch generated UI code directly unless the change is intentionally a
generated-code fix.

## Runtime Notes

Key runtime behaviors owned by this project:

- shared-data partition is mounted and bound into runtime paths such as
  `/assets`, `/app`, `/dispimg`, and `/root/themes`
- USB gadget state is applied through the unified `usbctl.sh` flow and restored
  at boot by `epass-usb-mode`
- GUI startup is guarded by preflight checks and fallback handlers
- firstboot selection writes the device and screen choice used by later boot
  stages
- `srgn_config` can be launched from the GUI/runtime path without dropping back
  to the old legacy shell flow

## Publishing Notes

For GitHub publishing, do not commit local build state or Windows metadata:

- `drm_arch_app/build/`
- `python-install/`
- local editor/cache files
- `*:Zone.Identifier`

Generated UI exports under `drm_arch_app/generated_ui/` and the EEZ project
under `ui_design/epass_eez/` are part of the project and should be kept in
sync.
