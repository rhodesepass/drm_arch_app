#!/bin/bash
# Cross-compile Python 3.11 for Arch Linux ARM deployment
# Uses Buildroot toolchain, independent of Buildroot's package system
set -e

PYTHON_VERSION="3.11.11"
PYTHON_MAJOR="3.11"

BUILDROOT=$(cd "$(dirname "$0")/.." && pwd)
export PATH=${BUILDROOT}/output/host/bin:$PATH
SYSROOT=${BUILDROOT}/output/host/arm-buildroot-linux-gnueabi/sysroot
CROSS_PREFIX=arm-buildroot-linux-gnueabi

SRCDIR=${BUILDROOT}/epass-arch/python-build
INSTALLDIR=${BUILDROOT}/epass-arch/python-install
TARBALL="Python-${PYTHON_VERSION}.tar.xz"
URL="https://python.org/ftp/python/${PYTHON_VERSION}/${TARBALL}"

echo "============================================"
echo "  Cross-compiling Python ${PYTHON_VERSION}"
echo "============================================"

# --- Step 1: Download ---
mkdir -p ${SRCDIR}
cd ${SRCDIR}

if [ ! -f "${TARBALL}" ]; then
    echo "Downloading Python ${PYTHON_VERSION}..."
    wget -q "${URL}" -O "${TARBALL}"
fi

if [ ! -d "Python-${PYTHON_VERSION}" ]; then
    echo "Extracting..."
    tar xf "${TARBALL}"
elif [ ! -f "Python-${PYTHON_VERSION}/Include/object.h" ]; then
    echo "Source tree is incomplete; re-extracting Python ${PYTHON_VERSION}..."
    rm -rf "Python-${PYTHON_VERSION}"
    tar xf "${TARBALL}"
fi

chmod +x "Python-${PYTHON_VERSION}/configure"

cd "Python-${PYTHON_VERSION}"

# --- Step 2: Build host Python first (needed for cross-compilation) ---
# Python's cross-compile needs a build-host python of the SAME version
HOST_PYTHON="${SRCDIR}/hostpython/bin/python3"
if [ ! -x "${HOST_PYTHON}" ] || ! "${HOST_PYTHON}" --version 2>/dev/null | grep -q "Python ${PYTHON_VERSION}"; then
    echo ""
    echo "=== Building host Python ${PYTHON_VERSION} ==="
    echo "Host Python is missing or invalid; rebuilding..."
    rm -rf "${SRCDIR}/hostpython" build-host
    mkdir -p build-host && cd build-host
    ../configure \
        --prefix="${SRCDIR}/hostpython" \
        --without-ensurepip \
        --disable-test-modules \
        --disable-idle3
    make -j$(nproc)
    make install
    cd ..
    echo "Host Python built: $(${HOST_PYTHON} --version)"
fi

# --- Step 3: Configure for ARM cross-compilation ---
echo ""
echo "=== Configuring target Python ${PYTHON_VERSION} ==="
rm -rf build-target && mkdir build-target && cd build-target

# Cross-compilation requires:
# 1. --host for target arch
# 2. --build for host arch
# 3. --with-build-python pointing to same-version host python
# 4. Various ac_cv overrides for things configure can't test when cross-compiling

CONFIG_SITE="" \
CFLAGS="-O2 --sysroot=${SYSROOT}" \
LDFLAGS="--sysroot=${SYSROOT}" \
../configure \
    --host=${CROSS_PREFIX} \
    --build=$(gcc -dumpmachine) \
    --prefix=/usr \
    --with-build-python="${SRCDIR}/hostpython/bin/python3" \
    --with-system-ffi \
    --without-ensurepip \
    --without-cxx-main \
    --disable-test-modules \
    --disable-idle3 \
    --disable-tk \
    --disable-nis \
    --enable-optimizations=no \
    --enable-shared \
    ac_cv_file__dev_ptmx=yes \
    ac_cv_file__dev_ptc=yes \
    ac_cv_have_long_long_format=yes \
    ac_cv_working_tzset=yes \
    ac_cv_buggy_getaddrinfo=no \
    ac_cv_little_endian_double=yes

# --- Step 4: Build ---
echo ""
echo "=== Building target Python ${PYTHON_VERSION} ==="
make -j$(nproc)

# --- Step 5: Install to staging directory ---
echo ""
echo "=== Installing to ${INSTALLDIR} ==="
rm -rf "${INSTALLDIR}"
make install DESTDIR="${INSTALLDIR}"

# Strip binaries
${CROSS_PREFIX}-strip "${INSTALLDIR}/usr/bin/python${PYTHON_MAJOR}" 2>/dev/null || true
find "${INSTALLDIR}/usr/lib" -name "*.so*" -exec ${CROSS_PREFIX}-strip {} \; 2>/dev/null || true

# Remove unnecessary files to save space
rm -rf "${INSTALLDIR}/usr/lib/python${PYTHON_MAJOR}/test"
rm -rf "${INSTALLDIR}/usr/lib/python${PYTHON_MAJOR}/unittest"
rm -rf "${INSTALLDIR}/usr/lib/python${PYTHON_MAJOR}/tkinter"
rm -rf "${INSTALLDIR}/usr/lib/python${PYTHON_MAJOR}/idlelib"
rm -rf "${INSTALLDIR}/usr/lib/python${PYTHON_MAJOR}/turtle*"
rm -rf "${INSTALLDIR}/usr/lib/python${PYTHON_MAJOR}/ensurepip"
rm -f "${INSTALLDIR}/usr/bin/python${PYTHON_MAJOR}-config"
rm -f "${INSTALLDIR}/usr/bin/python3-config"
find "${INSTALLDIR}" -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true
find "${INSTALLDIR}" -name "*.pyc" -delete 2>/dev/null || true
find "${INSTALLDIR}" -name "*.pyo" -delete 2>/dev/null || true

# Create symlinks
cd "${INSTALLDIR}/usr/bin"
ln -sf python${PYTHON_MAJOR} python3
ln -sf python${PYTHON_MAJOR} python

echo ""
echo "============================================"
echo "  Python ${PYTHON_VERSION} cross-compiled"
echo "  Install dir: ${INSTALLDIR}"
echo "  Size: $(du -sh ${INSTALLDIR} | cut -f1)"
echo "============================================"
echo ""
echo "Files to deploy:"
echo "  ${INSTALLDIR}/usr/bin/python${PYTHON_MAJOR}"
echo "  ${INSTALLDIR}/usr/lib/python${PYTHON_MAJOR}/"
echo "  ${INSTALLDIR}/usr/lib/libpython${PYTHON_MAJOR}*.so*"
