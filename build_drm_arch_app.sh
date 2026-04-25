#!/bin/bash
set -e

BUILDROOT=$(cd "$(dirname "$0")/.." && pwd)

echo "=== Building drm_arch_app through Buildroot ==="
make -C "${BUILDROOT}" drm_arch_app-reconfigure

DRM_APP_BIN="${BUILDROOT}/output/target/usr/bin/drm_arch_app"
READELF="${BUILDROOT}/output/host/bin/arm-buildroot-linux-gnueabi-readelf"

if [ ! -x "${DRM_APP_BIN}" ]; then
    echo "ERROR: drm_arch_app was not installed at ${DRM_APP_BIN}"
    exit 1
fi

if [ ! -x "${READELF}" ]; then
    READELF=readelf
fi

NEEDED_LIBS=$("${READELF}" -d "${DRM_APP_BIN}" | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')

require_needed() {
    local lib="$1"
    if ! printf '%s\n' "${NEEDED_LIBS}" | grep -qx "${lib}"; then
        echo "ERROR: built drm_arch_app is missing required ABI dependency: ${lib}"
        exit 1
    fi
}

for required_lib in \
    libevdev.so.2 \
    libdrm.so.2 \
    libpng16.so.16 \
    libMemAdapter.so \
    libvdecoder.so \
    libVE.so \
    libvideoengine.so \
    libcdc_base.so \
    libcdx_parser.so \
    libcdx_stream.so \
    libcdx_base.so \
    libcdx_common.so \
    libssl.so.1.1 \
    libcrypto.so.1.1; do
    require_needed "${required_lib}"
done

echo ""
echo "=== drm_arch_app compiled successfully ==="
echo "Binary: $(ls -la "${DRM_APP_BIN}")"
echo "ABI dependencies:"
printf '  %s\n' ${NEEDED_LIBS}
