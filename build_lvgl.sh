#!/bin/bash
set -e

cat <<'EOF'
build_lvgl.sh is legacy and no longer builds in the Arch mainline.

The Arch mainline GUI is drm_arch_app:
  ./epass-arch/build_drm_arch_app.sh
  or Buildroot: make drm_arch_app

The old framebuffer lvgl-ui source is retained only for reference.
EOF