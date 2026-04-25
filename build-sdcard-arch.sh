#!/bin/bash
# Build complete Arch Linux ARM SD card image for ArkEPass (F1C200s)
# Includes: new kernel, U-Boot, drm_app_neo, all systemd configs
# Usage: sudo ./build-sdcard-arch.sh
set -e

BUILDROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGES=${BUILDROOT}/output/images
DEPLOY=${BUILDROOT}/epass-arch/deploy
ARCH_TAR=${BUILDROOT}/ArchLinuxARM-armv5-latest.tar.gz
DT_SRC=${BUILDROOT}/board/rhodesisland/epass/devicetree/linux
KERNEL_BUILD=${BUILDROOT}/output/build/linux-5.4.99

IMG=${IMG:-${BUILDROOT}/sdcard-arch.img}
IMG_SIZE=${IMG_SIZE:-2048}     # MB total image size
BOOT_SIZE=${BOOT_SIZE:-32}     # MB (zImage + all hardware DTB/DTBO assets)
ROOTFS_SIZE=${ROOTFS_SIZE:-1536}  # MB fixed ext4 rootfs size when EPASSDATA is enabled
ROOTFS_HEADROOM_MB=${ROOTFS_HEADROOM_MB:-128}

BOOT_DEFAULT_DEVICE_REV=${BOOT_DEFAULT_DEVICE_REV:-0.6}
BOOT_DEFAULT_SCREEN=${BOOT_DEFAULT_SCREEN:-laowu}
BOOT_DEVICE_REVS=${BOOT_DEVICE_REVS:-"0.2 0.3 0.5 0.6"}
BOOT_SCREENS=${BOOT_SCREENS:-"laowu hsd boe"}

SD_DISK_ID=0x45504153
SD_ROOT_PARTUUID=45504153-02
BOOT_LABEL=EPASSBOOT
DATA_LABEL=EPASSDATA
DATA_PART_INDEX=1
ROOT_PART_INDEX=2
BOOT_PART_INDEX=3
DOSFSTOOLS_VERSION=4.1
DOSFSTOOLS_TARBALL="${BUILDROOT}/dl/dosfstools-${DOSFSTOOLS_VERSION}.tar.xz"
DOSFSTOOLS_TARBALL_ALT="${BUILDROOT}/dl/dosfstools/dosfstools-${DOSFSTOOLS_VERSION}.tar.xz"
EPASS_DATA_IMAGE_NAME=epass-data.vfat
FB_CONSOLE_LOGO_ARG="fbcon=logo-pos:center"
SYSTEMD_CGROUP_COMPAT_ARG="systemd.unified_cgroup_hierarchy=0"
SD_CMA_ARG="cma=24M"
LEGACY_BOOTARGS_RE='root=/dev/mtdblock[0-9]+|init=/preinit|overlayfsdev=|rootfstype=squashfs'

INCLUDE_DEV_TOOLS=${INCLUDE_DEV_TOOLS:-yes}
INCLUDE_PYTHON=${INCLUDE_PYTHON:-yes}
ENABLE_DROP_CACHE=${ENABLE_DROP_CACHE:-no}
ALLOW_BUILDROOT_OPENSSL=${ALLOW_BUILDROOT_OPENSSL:-no}
HOSTNAME_VALUE=${HOSTNAME_VALUE:-epass}
HOST_UID=${EPASS_HOST_UID:-${SUDO_UID:-$(id -u)}}

ENABLE_BOOT_ANIMATION=${ENABLE_BOOT_ANIMATION:-yes}
ENABLE_FIRSTBOOT=${ENABLE_FIRSTBOOT:-yes}
ENABLE_RESIZE_ROOTFS=${ENABLE_RESIZE_ROOTFS:-no}
ENABLE_SHARED_DATA_PARTITION=${ENABLE_SHARED_DATA_PARTITION:-yes}
ENABLE_SCREEN_DETECT=${ENABLE_SCREEN_DETECT:-yes}
ENABLE_CPU_OVERCLOCK=${ENABLE_CPU_OVERCLOCK:-yes}
ENABLE_DRM_GUI=${ENABLE_DRM_GUI:-yes}
ENABLE_SYSTEMD_TIMESYNCD=${ENABLE_SYSTEMD_TIMESYNCD:-no}
ENABLE_SYSTEMD_LOGIND=${ENABLE_SYSTEMD_LOGIND:-no}
ENABLE_SWAPFILE_RUNTIME=${ENABLE_SWAPFILE_RUNTIME:-no}
PRESEED_FIRSTBOOT=${PRESEED_FIRSTBOOT:-no}
PRESEED_ROOTFS_EXPANDED=${PRESEED_ROOTFS_EXPANDED:-no}
PRESEED_DEVICE_REV=${PRESEED_DEVICE_REV:-${BOOT_DEFAULT_DEVICE_REV}}
PRESEED_SCREEN=${PRESEED_SCREEN:-${BOOT_DEFAULT_SCREEN}}

LOOP=""
BOOT_MNT=""
ROOT_MNT=""
DATA_MNT=""
ROOTLESS_MODE="${EPASS_IMAGE_BACKEND:-}"
ROOTLESS_WORKDIR=""
ROOTLESS_GENROOT=""
BUILD_MODE_TAG=rootful
[ "${ROOTLESS_MODE}" = "rootless" ] && BUILD_MODE_TAG=rootless
BUILD_TMP="${BUILDROOT}/output/epass-build-tmp-${HOST_UID}-${BUILD_MODE_TAG}"
RESIZE_INITRAMFS_IMAGE="${BUILD_TMP}/initramfs-resize.cpio.gz"

fail_build() {
    local msg="$1"
    echo "ERROR: ${msg}"
    sync || true
    if [ -n "${BOOT_MNT}" ] && mountpoint -q "${BOOT_MNT}"; then
        umount "${BOOT_MNT}" 2>/dev/null || true
    fi
    if [ -n "${ROOT_MNT}" ] && mountpoint -q "${ROOT_MNT}"; then
        umount "${ROOT_MNT}" 2>/dev/null || true
    fi
    if [ -n "${DATA_MNT}" ] && mountpoint -q "${DATA_MNT}"; then
        umount "${DATA_MNT}" 2>/dev/null || true
    fi
    if [ -n "${LOOP}" ]; then
        losetup -d "${LOOP}" 2>/dev/null || true
    fi
    cleanup_temp_dir "${BOOT_MNT}"
    cleanup_temp_dir "${ROOT_MNT}"
    cleanup_temp_dir "${DATA_MNT}"
    [ -n "${ROOTLESS_WORKDIR}" ] && rm -rf "${ROOTLESS_WORKDIR}" 2>/dev/null || true
    exit 1
}

cleanup_temp_dir() {
    local dir="$1"

    [ -n "${dir}" ] || return 0
    [ -d "${dir}" ] || return 0
    if mountpoint -q "${dir}" 2>/dev/null; then
        return 1
    fi
    rmdir "${dir}" 2>/dev/null || rm -rf "${dir}" 2>/dev/null || true
}

is_rootless_backend() {
    [ "${ROOTLESS_MODE}" = "rootless" ]
}

enter_rootless_backend() {
    if [ "$(id -u)" -eq 0 ] || is_rootless_backend; then
        return 0
    fi

    [ -x "${BUILDROOT}/output/host/bin/fakeroot" ] \
        || fail_build "Missing host fakeroot: ${BUILDROOT}/output/host/bin/fakeroot"
    [ -x "${BUILDROOT}/output/host/bin/genimage" ] \
        || fail_build "Missing host genimage: ${BUILDROOT}/output/host/bin/genimage"

    echo "INFO: root privileges unavailable; switching to fakeroot + genimage backend"
    exec env EPASS_IMAGE_BACKEND=rootless EPASS_HOST_UID="${HOST_UID}" FAKEROOTDONTTRYCHOWN=1 \
        "${BUILDROOT}/output/host/bin/fakeroot" -- "$0" "$@"
}

prepare_rootless_staging() {
    ROOTLESS_WORKDIR=$(mktemp -d)
    ROOTLESS_GENROOT="${ROOTLESS_WORKDIR}/genroot"
    BOOT_MNT="${ROOTLESS_GENROOT}/bootfs"
    ROOT_MNT="${ROOTLESS_GENROOT}/rootfs"
    DATA_MNT="${ROOT_MNT}/mnt/epass-data"
    if shared_data_partition_enabled; then
        DATA_MNT="${ROOTLESS_GENROOT}/datafs"
        mkdir -p "${DATA_MNT}"
    fi
    mkdir -p "${BOOT_MNT}" "${ROOT_MNT}" "${ROOT_MNT}/mnt/epass-data"
}

write_rootless_genimage_cfg() {
    local cfg="$1"
    local image_name boot_size_k data_size_mb data_size_k root_size_mb root_size_k
    local boot_offset root_offset data_offset

    image_name=$(basename "${IMG}")
    boot_size_k=$((BOOT_SIZE * 1024))
    data_size_mb=$(data_partition_size_mb)
    data_size_k=$((data_size_mb * 1024))
    root_size_mb=$(rootfs_partition_size_mb)
    root_size_k=$((root_size_mb * 1024))
    boot_offset=$(( $(boot_start_sector) * 512 ))
    root_offset=$(( $(root_start_sector) * 512 ))
    data_offset=$(( $(data_start_sector) * 512 ))

    cat > "${cfg}" << EOF
image bootfs.vfat {
	vfat {
		extraargs = "-F 32"
		label = "${BOOT_LABEL}"
	}
	size = ${boot_size_k}K
	mountpoint = "/bootfs"
}

EOF

    if shared_data_partition_enabled; then
        cat >> "${cfg}" << EOF
image ${EPASS_DATA_IMAGE_NAME} {
	vfat {
		extraargs = "-F 32"
		label = "${DATA_LABEL}"
	}
	size = ${data_size_k}K
	mountpoint = "/datafs"
}

EOF
    fi

    cat >> "${cfg}" << EOF
image rootfs.ext4 {
	ext4 {
		use-mke2fs = true
		label = "ROOTFS"
		features = "^metadata_csum"
	}
	size = ${root_size_k}K
	mountpoint = "/rootfs"
}
EOF

    cat >> "${cfg}" << EOF

image ${image_name} {
	hdimage {
		disk-signature = ${SD_DISK_ID}
	}

	partition u-boot {
		image = "u-boot-sunxi-with-spl.bin"
		offset = 0x2000
		size = 1016K
		in-partition-table = false
	}
EOF

    if shared_data_partition_enabled; then
        cat >> "${cfg}" << EOF

	partition epass-data {
		partition-type = 0x0C
		image = "${EPASS_DATA_IMAGE_NAME}"
		offset = ${data_offset}
	}

	partition rootfs {
		partition-type = 0x83
		image = "rootfs.ext4"
		offset = ${root_offset}
	}

	partition boot {
		partition-type = 0x1C
		bootable = true
		image = "bootfs.vfat"
		offset = ${boot_offset}
	}
EOF
    else
        cat >> "${cfg}" << EOF

	partition boot {
		partition-type = 0x0C
		bootable = true
		image = "bootfs.vfat"
		offset = ${boot_offset}
	}

	partition rootfs {
		partition-type = 0x83
		image = "rootfs.ext4"
		offset = ${root_offset}
	}
EOF
    fi

    cat >> "${cfg}" << EOF
}
EOF
}

rewrite_rootless_partition_table() {
    local image="$1"
    local boot_start boot_sectors root_start root_sectors data_start data_sectors
    local sfdisk_bin

    shared_data_partition_enabled || return 0
    sfdisk_bin=$(command -v sfdisk || true)
    [ -n "${sfdisk_bin}" ] \
        || fail_build "Missing host sfdisk required for rootless shared-data image layout"

    boot_start=$(boot_start_sector)
    boot_sectors=$(boot_partition_sectors)
    root_start=$(root_start_sector)
    root_sectors=$(root_partition_sectors)
    data_start=$(data_start_sector)
    data_sectors=$(data_partition_sectors)

    env -u LD_LIBRARY_PATH -u LD_PRELOAD -u FAKEROOTKEY -u FAKED_MODE \
        "${sfdisk_bin}" "${image}" << EOF >/dev/null
label: dos
label-id: ${SD_DISK_ID}
unit: sectors

start=${data_start}, size=${data_sectors}, type=c
start=${root_start}, size=${root_sectors}, type=83
start=${boot_start}, size=${boot_sectors}, type=1c, bootable
EOF
}

build_rootless_sd_image() {
    local cfg toolsdir outdir img_name

    [ -n "${ROOTLESS_WORKDIR}" ] || fail_build "Rootless staging directory is not initialized"
    [ -d "${ROOTLESS_GENROOT}" ] || fail_build "Rootless genimage root is missing"
    [ -x "${BUILDROOT}/output/host/bin/genimage" ] \
        || fail_build "Missing host genimage: ${BUILDROOT}/output/host/bin/genimage"
    [ -x "${BUILDROOT}/output/host/bin/mtools" ] \
        || fail_build "Missing host mtools dispatcher: ${BUILDROOT}/output/host/bin/mtools"
    [ -x "${BUILDROOT}/output/host/sbin/mkfs.fat" ] \
        || fail_build "Missing host mkfs.fat: ${BUILDROOT}/output/host/sbin/mkfs.fat"
    [ -x "${BUILDROOT}/output/host/sbin/mke2fs" ] \
        || fail_build "Missing host mke2fs: ${BUILDROOT}/output/host/sbin/mke2fs"
    [ -x "${BUILDROOT}/output/host/sbin/debugfs" ] \
        || fail_build "Missing host debugfs: ${BUILDROOT}/output/host/sbin/debugfs"
    [ -x "${BUILDROOT}/output/host/sbin/e2fsck" ] \
        || fail_build "Missing host e2fsck: ${BUILDROOT}/output/host/sbin/e2fsck"
    [ -x "${BUILDROOT}/output/host/sbin/tune2fs" ] \
        || fail_build "Missing host tune2fs: ${BUILDROOT}/output/host/sbin/tune2fs"

    toolsdir="${ROOTLESS_WORKDIR}/tools"
    outdir="${ROOTLESS_WORKDIR}/genimage-out"
    cfg="${ROOTLESS_WORKDIR}/genimage-sdcard.cfg"
    img_name=$(basename "${IMG}")

    mkdir -p "${toolsdir}" "${outdir}"
    ln -sf "${BUILDROOT}/output/host/bin/mtools" "${toolsdir}/mcopy"
    ln -sf "${BUILDROOT}/output/host/bin/mtools" "${toolsdir}/mmd"
    write_rootless_genimage_cfg "${cfg}"

    "${BUILDROOT}/output/host/bin/genimage" \
        --rootpath "${ROOTLESS_GENROOT}" \
        --tmppath "${ROOTLESS_WORKDIR}/genimage.tmp" \
        --inputpath "${IMAGES}" \
        --outputpath "${outdir}" \
        --config "${cfg}" \
        --mcopy "${toolsdir}/mcopy" \
        --mmd "${toolsdir}/mmd" \
        --mkdosfs "${BUILDROOT}/output/host/sbin/mkfs.fat" \
        --mke2fs "${BUILDROOT}/output/host/sbin/mke2fs" \
        --debugfs "${BUILDROOT}/output/host/sbin/debugfs" \
        --e2fsck "${BUILDROOT}/output/host/sbin/e2fsck" \
        --tune2fs "${BUILDROOT}/output/host/sbin/tune2fs" \
        || fail_build "genimage failed while assembling the SD image"

    install -D -m 0644 "${outdir}/${img_name}" "${IMG}" \
        || fail_build "Failed to publish final image ${IMG}"
    rewrite_rootless_partition_table "${IMG}"
}

shared_data_partition_enabled() {
    [ "${ENABLE_SHARED_DATA_PARTITION}" = "yes" ]
}

shared_data_resize_supported() {
    shared_data_partition_enabled
}

resize_supported() {
    shared_data_resize_supported
}

boot_start_sector() {
    printf '2048\n'
}

boot_partition_sectors() {
    printf '%s\n' $((BOOT_SIZE * 1024 * 1024 / 512))
}

root_start_sector() {
    printf '%s\n' $(( $(boot_start_sector) + $(boot_partition_sectors) ))
}

root_partition_sectors() {
    printf '%s\n' $(( $(rootfs_partition_size_mb) * 1024 * 1024 / 512 ))
}

data_start_sector() {
    printf '%s\n' $(( $(root_start_sector) + $(root_partition_sectors) ))
}

rootfs_partition_size_mb() {
    if shared_data_partition_enabled; then
        printf '%s\n' "${ROOTFS_SIZE}"
    else
        printf '%s\n' $((IMG_SIZE - BOOT_SIZE - 1))
    fi
}

data_partition_size_mb() {
    if shared_data_partition_enabled; then
        printf '%s\n' $((IMG_SIZE - BOOT_SIZE - ROOTFS_SIZE - 1))
    else
        printf '0\n'
    fi
}

data_partition_sectors() {
    printf '%s\n' $(( $(data_partition_size_mb) * 1024 * 1024 / 512 ))
}

sectors_to_bytes() {
    printf '%s\n' $(( $1 * 512 ))
}

vfat_kblocks_from_sectors() {
    printf '%s\n' $(( $1 / 2 ))
}

ext4_blocks_from_sectors() {
    printf '%s\n' $(( $(sectors_to_bytes "$1") / 4096 ))
}

format_vfat_image_partition() {
    local image="$1"
    local start_sector="$2"
    local sector_count="$3"
    local label="$4"

    mkfs.vfat --offset="${start_sector}" -F 32 -n "${label}" \
        "${image}" "$(vfat_kblocks_from_sectors "${sector_count}")"
}

format_ext4_image_partition() {
    local image="$1"
    local start_sector="$2"
    local sector_count="$3"
    local label="$4"
    local offset_bytes

    offset_bytes=$(sectors_to_bytes "${start_sector}")
    mkfs.ext4 -F -L "${label}" -O ^metadata_csum -b 4096 \
        -E "offset=${offset_bytes}" \
        "${image}" "$(ext4_blocks_from_sectors "${sector_count}")"
}

mount_image_partition() {
    local image="$1"
    local mountpoint="$2"
    local fstype="$3"
    local start_sector="$4"
    local sector_count="$5"
    local offset_bytes size_bytes

    offset_bytes=$(sectors_to_bytes "${start_sector}")
    size_bytes=$(sectors_to_bytes "${sector_count}")
    mount -t "${fstype}" -o "loop,offset=${offset_bytes},sizelimit=${size_bytes}" \
        "${image}" "${mountpoint}"
}

find_dosfstools_mkfs_binary() {
    local src_dir="$1"
    local candidate

    for candidate in \
        "${src_dir}/src/mkfs.fat" \
        "${src_dir}/mkfs.fat" \
        "${src_dir}/src/mkfs.vfat" \
        "${src_dir}/mkfs.vfat"; do
        if [ -x "${candidate}" ]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    return 1
}

prepare_dosfstools_source_tree() {
    local tag="$1"
    local workdir="${BUILD_TMP}/${tag}"
    local src_dir=""
    local candidate
    local tarball=""
    local had_match=no

    rm -rf "${workdir}"
    mkdir -p "${workdir}"

    for candidate in "${DOSFSTOOLS_TARBALL}" "${DOSFSTOOLS_TARBALL_ALT}"; do
        if [ -f "${candidate}" ]; then
            tarball="${candidate}"
            break
        fi
    done

    if [ -n "${tarball}" ]; then
        tar -xf "${tarball}" -C "${workdir}" \
            || fail_build "Failed to unpack ${tarball}"
        src_dir=$(find "${workdir}" -mindepth 1 -maxdepth 1 -type d | head -n 1)
    else
        shopt -s nullglob
        for candidate in "${BUILDROOT}"/output/build/host-dosfstools-* "${BUILDROOT}"/output/build/dosfstools-*; do
            [ -d "${candidate}" ] || continue
            cp -a "${candidate}" "${workdir}/" \
                || fail_build "Failed to stage ${candidate} for mkfs.vfat helper build"
            src_dir="${workdir}/$(basename "${candidate}")"
            had_match=yes
            break
        done
        shopt -u nullglob
        if [ "${had_match}" != "yes" ]; then
            fail_build "Missing dosfstools sources. Run make so Buildroot downloads/builds host-dosfstools."
        fi
    fi

    [ -n "${src_dir}" ] && [ -d "${src_dir}" ] \
        || fail_build "Could not locate unpacked dosfstools sources"
    printf '%s\n' "${src_dir}"
}

build_static_mkfs_vfat() {
    local src_dir out_bin out_bin_tmp
    local cross_prefix="${BUILDROOT}/output/host/bin/arm-buildroot-linux-gnueabi"

    [ -x "${cross_prefix}-gcc" ] || fail_build "Missing cross compiler: ${cross_prefix}-gcc"
    [ -x "${cross_prefix}-strip" ] || fail_build "Missing cross strip: ${cross_prefix}-strip"

    src_dir=$(prepare_dosfstools_source_tree dosfstools-target)
    (
        cd "${src_dir}"
        make distclean >/dev/null 2>&1 || true
        CC="${cross_prefix}-gcc" \
        AR="${cross_prefix}-ar" \
        RANLIB="${cross_prefix}-ranlib" \
        STRIP="${cross_prefix}-strip" \
        CFLAGS="-Os" \
        LDFLAGS="-static" \
        ./configure --host=arm-buildroot-linux-gnueabi --without-udev --enable-compat-symlinks >/dev/null 2>&1 || exit 1
        make -j"$(nproc)" >/dev/null 2>&1 || exit 1
    ) || fail_build "Failed to build static mkfs.vfat helper"

    out_bin_tmp=$(find_dosfstools_mkfs_binary "${src_dir}") \
        || fail_build "Built dosfstools tree is missing mkfs.fat"
    out_bin="${BUILD_TMP}/mkfs.vfat.static"
    install -D -m 0755 "${out_bin_tmp}" "${out_bin}" \
        || fail_build "Failed to install static mkfs.vfat helper"
    "${cross_prefix}-strip" "${out_bin}" 2>/dev/null || true
    printf '%s\n' "${out_bin}"
}
check_staged_rootfs_capacity() {
    local used_mb limit_mb required_mb

    [ -d "${ROOT_MNT}" ] || return 0

    used_mb=$(du -sm "${ROOT_MNT}" 2>/dev/null | awk '{print $1}')
    [ -n "${used_mb}" ] || return 0

    limit_mb=$(rootfs_partition_size_mb)
    required_mb=$((used_mb + ROOTFS_HEADROOM_MB))

    if [ "${required_mb}" -gt "${limit_mb}" ]; then
        fail_build "Staged rootfs uses ${used_mb}MB; rootfs partition is ${limit_mb}MB. Increase ROOTFS_SIZE (need at least ~${required_mb}MB including ext4 overhead)."
    fi
}

sync_tree_to_data_partition() {
    local src="$1"
    local dst_rel="$2"

    [ -n "${DATA_MNT}" ] || return 0
    mkdir -p "${DATA_MNT}/${dst_rel}"
    [ -d "${src}" ] || return 0
    cp -a "${src}/." "${DATA_MNT}/${dst_rel}/"
}

populate_shared_data_partition() {
    shared_data_partition_enabled || return 0
    [ -n "${DATA_MNT}" ] || return 0

    mkdir -p \
        "${ROOT_MNT}/assets" \
        "${ROOT_MNT}/dispimg" \
        "${ROOT_MNT}/root/themes" \
        "${DATA_MNT}/assets" \
        "${DATA_MNT}/display-images" \
        "${DATA_MNT}/themes"

    sync_tree_to_data_partition "${ROOT_MNT}/assets" assets
    sync_tree_to_data_partition "${ROOT_MNT}/dispimg" display-images
    sync_tree_to_data_partition "${ROOT_MNT}/root/themes" themes
}

validate_toggle() {
    local name="$1"
    local value="$2"

    case "${value}" in
        yes|no)
            ;;
        *)
            fail_build "${name} must be yes or no (got: ${value})"
            ;;
    esac
}

boot_env_firstboot_value() {
    if [ "${ENABLE_FIRSTBOOT}" = "yes" ] && [ "${PRESEED_FIRSTBOOT}" != "yes" ]; then
        printf '1\n'
    else
        printf '0\n'
    fi
}

boot_env_resize_pending_value() {
    if ! resize_supported || [ "${PRESEED_ROOTFS_EXPANDED}" = "yes" ]; then
        printf '0\n'
    elif [ "$(boot_env_firstboot_value)" = "1" ]; then
        printf '0\n'
    else
        printf '1\n'
    fi
}

boot_env_resize_stage_value() {
    if [ "$(boot_env_resize_pending_value)" = "1" ]; then
        printf 'partition\n'
    else
        printf 'done\n'
    fi
}

boot_env_boot_mode_value() {
    if [ "$(boot_env_resize_pending_value)" = "1" ]; then
        printf 'resize\n'
    else
        printf 'normal\n'
    fi
}

build_sd_bootargs_normal() {
    printf 'console=ttyS0,115200 root=PARTUUID=%s rootwait rw loglevel=7 %s %s %s\n' \
        "${SD_ROOT_PARTUUID}" "${FB_CONSOLE_LOGO_ARG}" "${SYSTEMD_CGROUP_COMPAT_ARG}" "${SD_CMA_ARG}"
}

build_sd_bootargs_firstboot() {
    printf '%s epass.firstboot=1\n' "$(build_sd_bootargs_normal)"
}

build_sd_bootargs_resize() {
    printf 'console=ttyS0,115200 root=PARTUUID=%s rootwait ro loglevel=7 %s %s %s epass.resize=1 rdinit=/usr/local/bin/epass-resize-init\n' \
        "${SD_ROOT_PARTUUID}" "${FB_CONSOLE_LOGO_ARG}" "${SYSTEMD_CGROUP_COMPAT_ARG}" "${SD_CMA_ARG}"
}

build_sd_bootargs() {
    if [ "$(boot_env_firstboot_value)" = "1" ]; then
        build_sd_bootargs_firstboot
    elif [ "$(boot_env_boot_mode_value)" = "resize" ]; then
        build_sd_bootargs_resize
    else
        build_sd_bootargs_normal
    fi
}

validate_feature_flags() {
    validate_toggle ENABLE_BOOT_ANIMATION "${ENABLE_BOOT_ANIMATION}"
    validate_toggle ENABLE_FIRSTBOOT "${ENABLE_FIRSTBOOT}"
    validate_toggle ENABLE_RESIZE_ROOTFS "${ENABLE_RESIZE_ROOTFS}"
    validate_toggle ENABLE_SHARED_DATA_PARTITION "${ENABLE_SHARED_DATA_PARTITION}"
    validate_toggle ENABLE_SCREEN_DETECT "${ENABLE_SCREEN_DETECT}"
    validate_toggle ENABLE_CPU_OVERCLOCK "${ENABLE_CPU_OVERCLOCK}"
    validate_toggle ENABLE_DRM_GUI "${ENABLE_DRM_GUI}"
    validate_toggle ENABLE_SYSTEMD_TIMESYNCD "${ENABLE_SYSTEMD_TIMESYNCD}"
    validate_toggle ENABLE_SYSTEMD_LOGIND "${ENABLE_SYSTEMD_LOGIND}"
    validate_toggle ENABLE_SWAPFILE_RUNTIME "${ENABLE_SWAPFILE_RUNTIME}"
    validate_toggle PRESEED_FIRSTBOOT "${PRESEED_FIRSTBOOT}"
    validate_toggle PRESEED_ROOTFS_EXPANDED "${PRESEED_ROOTFS_EXPANDED}"

    case "${IMG_SIZE}" in
        ''|*[!0-9]*)
            fail_build "IMG_SIZE must be a positive integer in MB (got: ${IMG_SIZE})"
            ;;
    esac

    case "${BOOT_SIZE}" in
        ''|*[!0-9]*)
            fail_build "BOOT_SIZE must be a positive integer in MB (got: ${BOOT_SIZE})"
            ;;
    esac

    case "${ROOTFS_SIZE}" in
        ''|*[!0-9]*)
            fail_build "ROOTFS_SIZE must be a positive integer in MB (got: ${ROOTFS_SIZE})"
            ;;
    esac

    if [ "${IMG_SIZE}" -le 0 ]; then
        fail_build "IMG_SIZE must be greater than 0 (got: ${IMG_SIZE})"
    fi

    if [ "${BOOT_SIZE}" -le 0 ]; then
        fail_build "BOOT_SIZE must be greater than 0 (got: ${BOOT_SIZE})"
    fi

    if [ "${ROOTFS_SIZE}" -le 0 ]; then
        fail_build "ROOTFS_SIZE must be greater than 0 (got: ${ROOTFS_SIZE})"
    fi

    if [ "$(rootfs_partition_size_mb)" -le 0 ]; then
        fail_build "Calculated rootfs partition size must be greater than 0MB"
    fi

    if shared_data_partition_enabled && [ "$(data_partition_size_mb)" -le 0 ]; then
        fail_build "IMG_SIZE (${IMG_SIZE}MB) must be larger than BOOT_SIZE (${BOOT_SIZE}MB) + ROOTFS_SIZE (${ROOTFS_SIZE}MB) + 1MB"
    fi

    if [ "${ENABLE_DRM_GUI}" = "yes" ] && [ "${ENABLE_SCREEN_DETECT}" != "yes" ]; then
        fail_build "ENABLE_DRM_GUI=yes requires ENABLE_SCREEN_DETECT=yes"
    fi

    if [ "${ENABLE_SCREEN_DETECT}" = "yes" ] && \
       [ "${ENABLE_FIRSTBOOT}" != "yes" ] && \
       [ "${PRESEED_FIRSTBOOT}" != "yes" ]; then
        fail_build "ENABLE_SCREEN_DETECT=yes requires ENABLE_FIRSTBOOT=yes or PRESEED_FIRSTBOOT=yes"
    fi

    if [ "${ENABLE_RESIZE_ROOTFS}" = "yes" ]; then
        fail_build "ENABLE_RESIZE_ROOTFS is no longer supported in the Arch SD image layout"
    fi

    if resize_supported && \
       [ "${ENABLE_FIRSTBOOT}" != "yes" ] && \
       [ "${PRESEED_FIRSTBOOT}" != "yes" ]; then
        fail_build "Resize flow requires ENABLE_FIRSTBOOT=yes or PRESEED_FIRSTBOOT=yes"
    fi

    if [ "${ENABLE_DRM_GUI}" = "yes" ] && \
       [ "${ENABLE_FIRSTBOOT}" != "yes" ] && \
       [ "${PRESEED_FIRSTBOOT}" != "yes" ]; then
        fail_build "ENABLE_DRM_GUI=yes requires ENABLE_FIRSTBOOT=yes or PRESEED_FIRSTBOOT=yes"
    fi

}

verify_drm_app_bin_freshness() {
    local bin="$1"
    local newer_file=""
    local app_root="${BUILDROOT}/epass-arch/drm_arch_app"

    [ -f "${bin}" ] || return 0

    newer_file=$(find \
        "${app_root}" \
        \( -path "${app_root}/build" -o -path "${app_root}/build/*" \) -prune -o \
        -type f \
        \( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
        -newer "${bin}" \
        -print \
        -quit)

    if [ -n "${newer_file}" ]; then
        fail_build "drm_arch_app source is newer than ${bin} (${newer_file}); run ./epass-arch/build_drm_arch_app.sh or make drm_arch_app-reconfigure before assembling the image"
    fi
}

preseed_firstboot_state() {
    mkdir -p "${ROOT_MNT}/etc/epass" "${ROOT_MNT}/etc/epass-firstboot"
    cat > "${ROOT_MNT}/etc/epass/hardware.conf" << HWEOF
DEVICE_REV=${PRESEED_DEVICE_REV}
SCREEN=${PRESEED_SCREEN}
HWEOF
    printf '%s\n' "${PRESEED_SCREEN}" > "${ROOT_MNT}/etc/screen_type"
    : > "${ROOT_MNT}/etc/epass-firstboot/configured"
    echo "  preseeded firstboot state: device_rev=${PRESEED_DEVICE_REV} screen=${PRESEED_SCREEN} resize_pending=$(boot_env_resize_pending_value)"
}

rootfs_has_lib() {
    local lib="$1"
    [ -e "${ROOT_MNT}/usr/lib/${lib}" ] && return 0
    [ -e "${ROOT_MNT}/lib/${lib}" ] && return 0
    return 1
}

is_protected_runtime_lib() {
    case "$(basename "$1")" in
        ld-linux*|libc.so*|libpthread.so*|libdl.so*|librt.so*|libm.so*)
            return 0
            ;;
    esac
    return 1
}

copy_target_lib() {
    local lib="$1"
    local src=""
    local dir

    for dir in "${BUILDROOT}/output/target/usr/lib" "${BUILDROOT}/output/target/lib"; do
        if [ -e "${dir}/${lib}" ]; then
            src="${dir}/${lib}"
            break
        fi
    done

    [ -n "${src}" ] || fail_build "Required runtime library missing from Buildroot target: ${lib}"
    cp -a "${src}" "${ROOT_MNT}/usr/lib/"

    local resolved
    resolved=$(readlink -f "${src}" 2>/dev/null || true)
    if [ -n "${resolved}" ] && [ -f "${resolved}" ] && [ "${resolved}" != "${src}" ]; then
        cp -a "${resolved}" "${ROOT_MNT}/usr/lib/"
    fi

    echo "  $(basename "${src}")"
}

copy_needed_libs() {
    local elf="$1"
    local readelf_bin="${BUILDROOT}/output/host/bin/arm-buildroot-linux-gnueabi-readelf"
    local needed

    [ -x "${readelf_bin}" ] || readelf_bin=readelf

    while IFS= read -r needed; do
        [ -n "${needed}" ] || continue

        if is_protected_runtime_lib "${needed}"; then
            echo "  ${needed}: using Arch runtime"
            continue
        fi
        if rootfs_has_lib "${needed}"; then
            echo "  ${needed}: provided by Arch"
            continue
        fi

        case "${needed}" in
            libssl.so*|libcrypto.so*)
                if [ "${ALLOW_BUILDROOT_OPENSSL}" != "yes" ]; then
                    fail_build "Arch rootfs is missing ${needed}; set ALLOW_BUILDROOT_OPENSSL=yes only after CedarX/OpenSSL ABI validation"
                fi
                ;;
        esac

        copy_target_lib "${needed}"
    done < <("${readelf_bin}" -d "${elf}" | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')
}

copy_optional_runtime_lib() {
    local lib="$1"

    if rootfs_has_lib "${lib}"; then
        echo "  ${lib}: provided by Arch"
        return 0
    fi

    if [ -e "${BUILDROOT}/output/target/usr/lib/${lib}" ] || [ -e "${BUILDROOT}/output/target/lib/${lib}" ]; then
        copy_target_lib "${lib}"
        return 0
    fi

    echo "  ${lib}: not found in Buildroot target"
    return 0
}

fat_install_file() {
    local src="$1"
    local dst="$2"

    [ -f "${src}" ] || fail_build "Missing boot file: ${src}"
    install -D -m 0644 "${src}" "${dst}"
}

compile_dts_file() {
    local src="$1"
    local dst="$2"
    local pp="${dst}.pp"
    local cpp="${BUILDROOT}/output/host/bin/arm-buildroot-linux-gnueabi-cpp"
    local dtc="${KERNEL_BUILD}/scripts/dtc/dtc"

    [ -x "${cpp}" ] || fail_build "Missing target cpp wrapper: ${cpp}"
    [ -x "${dtc}" ] || fail_build "Missing kernel dtc: ${dtc}"
    [ -f "${src}" ] || fail_build "Missing DTS source: ${src}"

    mkdir -p "$(dirname "${dst}")"
    "${cpp}" -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
        -I"${KERNEL_BUILD}/arch/arm/boot/dts" \
        -I"${KERNEL_BUILD}/include" \
        -I"${KERNEL_BUILD}/include/dt-bindings" \
        -I"${BUILDROOT}/board/allwinner/suniv-f1c100s/devicetree/linux" \
        -I"${DT_SRC}/base" \
        -I"${DT_SRC}/screen" \
        "${src}" > "${pp}"
    "${dtc}" -@ -I dts -O dtb -o "${dst}" "${pp}"
    rm -f "${pp}"
}

ensure_boot_dt_assets() {
    local dt_dir="${IMAGES}/dt"
    local rev
    local screen
    local missing=no

    for rev in ${BOOT_DEVICE_REVS}; do
        [ -f "${dt_dir}/base/devicetree-${rev}.dtb" ] || missing=yes
    done
    for screen in ${BOOT_SCREENS}; do
        [ -f "${dt_dir}/screen/${screen}.dtbo" ] || missing=yes
    done

    if [ "${missing}" = "no" ]; then
        local dtc="${KERNEL_BUILD}/scripts/dtc/dtc"
        for rev in ${BOOT_DEVICE_REVS}; do
            if "${dtc}" -I dtb -O dts "${dt_dir}/base/devicetree-${rev}.dtb" 2>/dev/null | grep -Eq "${LEGACY_BOOTARGS_RE}"; then
                echo "DT assets: stale legacy bootargs found, recompiling ${dt_dir}"
                missing=yes
                break
            fi
        done
    fi

    if [ "${missing}" = "no" ]; then
        echo "DT assets: using ${dt_dir}"
        return
    fi

    echo "DT assets: compiling all hardware base DTBs and screen DTBOs"
    mkdir -p "${dt_dir}/base" "${dt_dir}/screen"
    for rev in ${BOOT_DEVICE_REVS}; do
        compile_dts_file \
            "${DT_SRC}/base/devicetree-${rev}.dts" \
            "${dt_dir}/base/devicetree-${rev}.dtb"
    done
    for screen in ${BOOT_SCREENS}; do
        compile_dts_file \
            "${DT_SRC}/screen/${screen}.dts" \
            "${dt_dir}/screen/${screen}.dtbo"
    done
}

write_boot_env() {
    local dst="$1"
    local firstboot="$2"
    local resize_pending="$3"
    local resize_stage="$4"
    local boot_mode="$5"

    mkdir -p "$(dirname "${dst}")"
    cat > "${dst}" << BOOTENVEOF
device_rev=${BOOT_DEFAULT_DEVICE_REV}
screen=${BOOT_DEFAULT_SCREEN}
epass_firstboot=${firstboot}
epass_resize_pending=${resize_pending}
epass_resize_stage=${resize_stage}
epass_boot_mode=${boot_mode}
BOOTENVEOF
    validate_boot_env_file "${dst}"
}

validate_boot_env_file() {
    local env_file="$1"
    local line

    [ -s "${env_file}" ] || fail_build "Boot env is empty or missing: ${env_file}"
    while IFS= read -r line || [ -n "${line}" ]; do
        line=${line%$'\r'}
        [ -z "${line}" ] && continue
        case "${line}" in
            device_rev=*|screen=*|epass_firstboot=*|epass_resize_pending=*|epass_resize_stage=*|epass_boot_mode=*)
                ;;
            *)
                fail_build "Unsafe boot env entry in ${env_file}: ${line}"
                ;;
        esac
        case "${line}" in
            *setenv*|*bootargs*|*sdroot*|*bootcmd*|*distro_bootcmd*|*root=/dev/mtdblock*|*init=/preinit*|*overlayfsdev=*|*rootfstype=squashfs*)
                fail_build "Forbidden content in boot env ${env_file}: ${line}"
                ;;
        esac
    done < "${env_file}"
}

check_dtb_no_legacy_bootargs() {
    local dtb="$1"
    local dtc="${KERNEL_BUILD}/scripts/dtc/dtc"
    local dts

    [ -x "${dtc}" ] || fail_build "Missing kernel dtc: ${dtc}"
    dts=$("${dtc}" -I dtb -O dts "${dtb}" 2>/dev/null) \
        || fail_build "Failed to decompile DTB for bootargs check: ${dtb}"
    if printf '%s\n' "${dts}" | grep -Eq "${LEGACY_BOOTARGS_RE}"; then
        fail_build "Legacy NAND bootargs found in DTB: ${dtb}"
    fi
}

check_boot_dt_assets() {
    local rev

    for rev in ${BOOT_DEVICE_REVS}; do
        check_dtb_no_legacy_bootargs "${IMAGES}/dt/base/devicetree-${rev}.dtb"
    done
}

check_uboot_sd_bootargs() {
    local uboot="${IMAGES}/u-boot-sunxi-with-spl.bin"

    strings "${uboot}" | grep -Fq "bootargs_sd=$(build_sd_bootargs_normal)" \
        || fail_build "U-Boot image missing minimal SD bootargs; run make uboot-rebuild"
    strings "${uboot}" | grep -Fq "bootargs_sd_firstboot=$(build_sd_bootargs_firstboot)" \
        || fail_build "U-Boot image missing firstboot SD bootargs; run make uboot-rebuild"
    strings "${uboot}" | grep -Fq "bootargs_sd_resize=$(build_sd_bootargs_resize)" \
        || fail_build "U-Boot image missing resize SD bootargs; run make uboot-rebuild"
    strings "${uboot}" | grep -Fq "bootargs_sd_debug=console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 root=PARTUUID=${SD_ROOT_PARTUUID} rootwait rw loglevel=7 ${FB_CONSOLE_LOGO_ARG} ${SYSTEMD_CGROUP_COMPAT_ARG} ${SD_CMA_ARG}" \
        || fail_build "U-Boot image missing debug SD bootargs; run make uboot-rebuild"
    strings "${uboot}" | grep -Fq "ramdiskfile=initramfs-resize.cpio.gz" \
        || fail_build "U-Boot image missing resize initramfs filename; run make uboot-rebuild"
    strings "${uboot}" | grep -Fq "sdbootpart=3" \
        || fail_build "U-Boot image still boots SD from partition 1; run make uboot-rebuild"
    strings "${uboot}" | grep -Fq "env import -t -r" \
        || fail_build "U-Boot image missing text CRLF-safe boot.env import; run make uboot-rebuild"
    strings "${uboot}" | grep -Fq 'bootz ${kernaddr} ${bootzrdarg} ${fdtaddr}' \
        || fail_build "U-Boot image missing dynamic initramfs bootz invocation; run make uboot-rebuild"
    if strings "${uboot}" | grep -Fq 'setenv bootargs ${bootargs_common}'; then
        fail_build "U-Boot image still contains legacy long bootargs expansion; run make uboot-rebuild"
    fi
    if strings "${uboot}" | grep -Fq "bootargs_common="; then
        fail_build "U-Boot image still contains legacy bootargs_common; run make uboot-rebuild"
    fi
}

declare -A INITRAMFS_SEEN=()

initramfs_readelf_bin() {
    local readelf_bin="${BUILDROOT}/output/host/bin/arm-buildroot-linux-gnueabi-readelf"
    [ -x "${readelf_bin}" ] || readelf_bin=readelf
    printf '%s\n' "${readelf_bin}"
}

initramfs_dest_path() {
    local src="$1"
    shift

    local root
    for root in "$@"; do
        [ -n "${root}" ] || continue
        case "${src}" in
            "${root}"/*)
                printf '%s\n' "${src#${root}/}"
                return 0
                ;;
        esac
    done

    return 1
}

initramfs_find_dep() {
    local needle="$1"
    shift

    local root
    for root in "$@"; do
        [ -n "${root}" ] || continue
        if [[ "${needle}" = /* ]] && [ -e "${root}${needle}" ]; then
            printf '%s\n' "${root}${needle}"
            return 0
        fi
        for dir in "${root}/lib" "${root}/usr/lib" "${root}/usr/libexec" "${root}/usr/bin" "${root}/usr/sbin"; do
            if [ -e "${dir}/${needle}" ]; then
                printf '%s\n' "${dir}/${needle}"
                return 0
            fi
        done
    done
    return 1
}

rootfs_tool_path() {
    local tool="$1"
    local dir

    for dir in /usr/bin /usr/sbin /bin /sbin; do
        if [ -x "${ROOT_MNT}${dir}/${tool}" ]; then
            printf '%s\n' "${ROOT_MNT}${dir}/${tool}"
            return 0
        fi
    done
    return 1
}

initramfs_copy_path() {
    local src="$1"
    local initroot="$2"
    shift 2

    local rel mode resolved link_target
    [ -e "${src}" ] || fail_build "Initramfs source missing: ${src}"
    rel=$(initramfs_dest_path "${src}" "$@") \
        || fail_build "Unable to map initramfs path ${src} into archive"
    if [ -n "${INITRAMFS_SEEN["${rel}"]:-}" ]; then
        return 0
    fi
    INITRAMFS_SEEN["${rel}"]=1

    if [ -L "${src}" ]; then
        mkdir -p "$(dirname "${initroot}/${rel}")"
        link_target=$(readlink "${src}")
        ln -sf "${link_target}" "${initroot}/${rel}"
        resolved=$(readlink -f "${src}" 2>/dev/null || true)
        [ -n "${resolved}" ] && [ "${resolved}" != "${src}" ] && initramfs_copy_path "${resolved}" "${initroot}" "$@"
        return 0
    fi

    mode=0644
    [ -x "${src}" ] && mode=0755
    install -D -m "${mode}" "${src}" "${initroot}/${rel}"

    initramfs_copy_binary_deps "${src}" "${initroot}" "$@"
}

initramfs_copy_binary_deps() {
    local src="$1"
    local initroot="$2"
    shift 2

    local readelf_bin interp dep dep_src
    readelf_bin=$(initramfs_readelf_bin)
    "${readelf_bin}" -h "${src}" >/dev/null 2>&1 || return 0
    interp=$("${readelf_bin}" -l "${src}" 2>/dev/null | sed -n 's@.*Requesting program interpreter: \(.*\)]@\1@p')
    if [ -n "${interp}" ]; then
        dep_src=$(initramfs_find_dep "${interp}" "$@") \
            || fail_build "Unable to resolve initramfs interpreter ${interp} for ${src}"
        initramfs_copy_path "${dep_src}" "${initroot}" "$@"
    fi

    while IFS= read -r dep; do
        [ -n "${dep}" ] || continue
        dep_src=$(initramfs_find_dep "${dep}" "$@") \
            || fail_build "Unable to resolve initramfs dependency ${dep} for ${src}"
        initramfs_copy_path "${dep_src}" "${initroot}" "$@"
    done < <("${readelf_bin}" -d "${src}" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')
}

validate_resize_initramfs_path() {
    local extract_dir="$1"
    local path="$2"

    [ -e "${extract_dir}/${path}" ] || [ -L "${extract_dir}/${path}" ] \
        || fail_build "Resize initramfs missing required path: ${path}"
}

validate_resize_initramfs_link() {
    local extract_dir="$1"
    local path="$2"
    local target="$3"

    [ -L "${extract_dir}/${path}" ] \
        || fail_build "Resize initramfs expected symlink ${path}"
    [ "$(readlink "${extract_dir}/${path}")" = "${target}" ] \
        || fail_build "Resize initramfs symlink ${path} should point to ${target}"
}

validate_resize_initramfs_binary() {
    local extract_dir="$1"
    local installed_rel="$2"
    local src="$3"
    shift 3

    local dep_rel readelf_bin interp dep dep_src
    validate_resize_initramfs_path "${extract_dir}" "${installed_rel}"

    readelf_bin=$(initramfs_readelf_bin)
    "${readelf_bin}" -h "${src}" >/dev/null 2>&1 || return 0

    interp=$("${readelf_bin}" -l "${src}" 2>/dev/null | sed -n 's@.*Requesting program interpreter: \(.*\)]@\1@p')
    if [ -n "${interp}" ]; then
        dep_src=$(initramfs_find_dep "${interp}" "$@") \
            || fail_build "Unable to resolve runtime interpreter ${interp} for ${src}"
        dep_rel=$(initramfs_dest_path "${dep_src}" "$@") \
            || fail_build "Unable to map runtime interpreter ${dep_src} into initramfs"
        validate_resize_initramfs_path "${extract_dir}" "${dep_rel}"
    fi

    while IFS= read -r dep; do
        [ -n "${dep}" ] || continue
        dep_src=$(initramfs_find_dep "${dep}" "$@") \
            || fail_build "Unable to resolve runtime dependency ${dep} for ${src}"
        dep_rel=$(initramfs_dest_path "${dep_src}" "$@") \
            || fail_build "Unable to map runtime dependency ${dep_src} into initramfs"
        validate_resize_initramfs_path "${extract_dir}" "${dep_rel}"
    done < <("${readelf_bin}" -d "${src}" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')
}

validate_resize_initramfs() {
    local out_image="$1"
    local busybox_src="$2"
    local bootenv_bin="$3"
    local blkid_bin="$4"
    local sfdisk_bin="$5"
    local mkfs_vfat_bin="${6:-}"
    local extract_dir bad_path bad_rel
    local buildroot_runtime

    extract_dir=$(mktemp -d)
    gzip -dc "${out_image}" | (cd "${extract_dir}" && cpio -idm 2>/dev/null) \
        || {
            rm -rf "${extract_dir}"
            fail_build "Failed to inspect resize initramfs ${out_image}"
        }

    validate_resize_initramfs_link "${extract_dir}" "bin" "usr/bin"
    validate_resize_initramfs_link "${extract_dir}" "lib" "usr/lib"
    validate_resize_initramfs_link "${extract_dir}" "sbin" "usr/bin"
    validate_resize_initramfs_link "${extract_dir}" "usr/sbin" "bin"
    validate_resize_initramfs_path "${extract_dir}" "init"
    validate_resize_initramfs_path "${extract_dir}" "usr/bin/busybox"
    validate_resize_initramfs_path "${extract_dir}" "bin/sh"
    validate_resize_initramfs_path "${extract_dir}" "usr/local/bin/epass-resize-init"
    validate_resize_initramfs_path "${extract_dir}" "usr/local/bin/epass-bootenv"
    validate_resize_initramfs_path "${extract_dir}" "usr/bin/blkid"
    validate_resize_initramfs_path "${extract_dir}" "usr/bin/sfdisk"
    if [ -n "${mkfs_vfat_bin}" ]; then
        validate_resize_initramfs_path "${extract_dir}" "usr/bin/mkfs.vfat"
    fi

    bad_path=$(find "${extract_dir}" \
        \( -path "${extract_dir}/home/*" -o -path "${extract_dir}/tmp/tmp.*" -o -path "${extract_dir}/tmp/tmp.*/*" \) \
        -print -quit)
    [ -z "${bad_path}" ] || {
        rm -rf "${extract_dir}"
        fail_build "Resize initramfs contains host path entry: ${bad_path#${extract_dir}/}"
    }

    validate_resize_initramfs_binary "${extract_dir}" "usr/bin/busybox" "${busybox_src}" \
        "${ROOT_MNT}" "${BUILDROOT}/output/target"
    validate_resize_initramfs_binary "${extract_dir}" "usr/local/bin/epass-bootenv" "${bootenv_bin}" \
        "${ROOT_MNT}"
    validate_resize_initramfs_binary "${extract_dir}" "usr/bin/blkid" "${blkid_bin}" \
        "${ROOT_MNT}"
    validate_resize_initramfs_binary "${extract_dir}" "usr/bin/sfdisk" "${sfdisk_bin}" \
        "${ROOT_MNT}"

    for bad_rel in lib/ld-linux.so.3 lib/libc.so.6 lib/libdl.so.2 lib/libm.so.6 \
        lib/libpthread.so.0 lib/libresolv.so.2 lib/librt.so.1; do
        if [ -e "${extract_dir}/${bad_rel}" ] && [ -e "${BUILDROOT}/output/target/${bad_rel}" ] && \
            cmp -s "${extract_dir}/${bad_rel}" "${BUILDROOT}/output/target/${bad_rel}"; then
            rm -rf "${extract_dir}"
            fail_build "Resize initramfs still uses Buildroot runtime artifact ${bad_rel}"
        fi
    done

    for buildroot_runtime in "${BUILDROOT}"/output/target/lib/*-2.30.so; do
        [ -e "${buildroot_runtime}" ] || continue
        bad_rel="lib/$(basename "${buildroot_runtime}")"
        if [ -e "${extract_dir}/${bad_rel}" ]; then
            rm -rf "${extract_dir}"
            fail_build "Resize initramfs still contains Buildroot glibc artifact ${bad_rel}"
        fi
    done

    rm -rf "${extract_dir}"
}

build_resize_initramfs() {
    local initroot
    local out_image="${RESIZE_INITRAMFS_IMAGE}"
    local busybox_src="${BUILDROOT}/output/target/bin/busybox"
    local bootenv_bin="${ROOT_MNT}/usr/local/bin/epass-bootenv"
    local blkid_bin sfdisk_bin mkfs_vfat_bin=""

    [ -x "${busybox_src}" ] || fail_build "Missing busybox for resize initramfs: ${busybox_src}"
    [ -x "${bootenv_bin}" ] || fail_build "Missing epass-bootenv for resize initramfs: ${bootenv_bin}"
    blkid_bin=$(rootfs_tool_path blkid) || fail_build "Arch rootfs is missing required resize tool blkid"
    sfdisk_bin=$(rootfs_tool_path sfdisk) || fail_build "Arch rootfs is missing required resize tool sfdisk"
    if shared_data_partition_enabled; then
        mkfs_vfat_bin=$(build_static_mkfs_vfat)
    fi

    initroot=$(mktemp -d)
    INITRAMFS_SEEN=()
    mkdir -p "${initroot}/dev" "${initroot}/mnt/boot" "${initroot}/proc" \
        "${initroot}/run" "${initroot}/sys" "${initroot}/tmp" \
        "${initroot}/usr" "${initroot}/usr/bin" "${initroot}/usr/lib" \
        "${initroot}/usr/local/bin"
    ln -s usr/bin "${initroot}/bin"
    ln -s usr/lib "${initroot}/lib"
    ln -s usr/bin "${initroot}/sbin"
    ln -s bin "${initroot}/usr/sbin"

    install -D -m 0755 "${busybox_src}" "${initroot}/usr/bin/busybox"
    INITRAMFS_SEEN["usr/bin/busybox"]=1
    initramfs_copy_binary_deps "${busybox_src}" "${initroot}" \
        "${ROOT_MNT}" "${BUILDROOT}/output/target"
    ln -sf busybox "${initroot}/usr/bin/sh"
    ln -sf busybox "${initroot}/usr/bin/mount"
    ln -sf busybox "${initroot}/usr/bin/umount"
    ln -sf busybox "${initroot}/usr/bin/mkdir"
    ln -sf busybox "${initroot}/usr/bin/sleep"
    ln -sf busybox "${initroot}/usr/bin/cat"
    ln -sf busybox "${initroot}/usr/bin/grep"
    ln -sf busybox "${initroot}/usr/bin/sync"
    ln -sf busybox "${initroot}/usr/bin/reboot"

    initramfs_copy_path "${blkid_bin}" "${initroot}" "${ROOT_MNT}"
    initramfs_copy_path "${sfdisk_bin}" "${initroot}" "${ROOT_MNT}"
    if [ -n "${mkfs_vfat_bin}" ]; then
        install -D -m 0755 "${mkfs_vfat_bin}" "${initroot}/usr/bin/mkfs.vfat"
    fi
    for cfg in /etc/blkid.conf; do
        if [ -f "${ROOT_MNT}${cfg}" ]; then
            install -D -m 0644 "${ROOT_MNT}${cfg}" "${initroot}${cfg}"
        fi
    done
    install -D -m 0755 "${DEPLOY}/usr/local/bin/epass-resize-init.sh" \
        "${initroot}/usr/local/bin/epass-resize-init"
    install -D -m 0755 "${bootenv_bin}" "${initroot}/usr/local/bin/epass-bootenv"
    ln -sf usr/local/bin/epass-resize-init "${initroot}/init"

    (
        cd "${initroot}"
        find . -print | cpio -o -H newc 2>/dev/null | gzip -9 > "${out_image}"
    ) || fail_build "Failed to build resize initramfs"

    validate_resize_initramfs "${out_image}" \
        "${busybox_src}" \
        "${bootenv_bin}" \
        "${blkid_bin}" \
        "${sfdisk_bin}" \
        "${mkfs_vfat_bin}"
    install -D -m 0644 "${out_image}" "${IMAGES}/initramfs-resize.cpio.gz"

    rm -rf "${initroot}"
}

copy_boot_dtbs() {
    local rev
    local screen
    local primary_dtb="${IMAGES}/dt/base/devicetree-${BOOT_DEFAULT_DEVICE_REV}.dtb"
    local firstboot

    ensure_boot_dt_assets
    check_boot_dt_assets
    [ -f "${primary_dtb}" ] || fail_build "Default DTB missing after build: ${primary_dtb}"

    mkdir -p \
        "${BOOT_MNT}/epass/dt/base" \
        "${BOOT_MNT}/epass/dt/screen" \
        "${BOOT_MNT}/dt/base" \
        "${BOOT_MNT}/dt/screen"

    for rev in ${BOOT_DEVICE_REVS}; do
        fat_install_file \
            "${IMAGES}/dt/base/devicetree-${rev}.dtb" \
            "${BOOT_MNT}/epass/dt/base/devicetree-${rev}.dtb"
        fat_install_file \
            "${IMAGES}/dt/base/devicetree-${rev}.dtb" \
            "${BOOT_MNT}/dt/base/devicetree-${rev}.dtb"
    done
    for screen in ${BOOT_SCREENS}; do
        fat_install_file \
            "${IMAGES}/dt/screen/${screen}.dtbo" \
            "${BOOT_MNT}/epass/dt/screen/${screen}.dtbo"
        fat_install_file \
            "${IMAGES}/dt/screen/${screen}.dtbo" \
            "${BOOT_MNT}/dt/screen/${screen}.dtbo"
    done

    fat_install_file "${primary_dtb}" "${BOOT_MNT}/devicetree.dtb"
    fat_install_file "${primary_dtb}" "${BOOT_MNT}/dt/devicetree.dtb"
    fat_install_file "${primary_dtb}" "${BOOT_MNT}/dt/base/devicetree.dtb"
    firstboot=$(boot_env_firstboot_value)
    write_boot_env "${BOOT_MNT}/epass/boot.env" \
        "${firstboot}" \
        "$(boot_env_resize_pending_value)" \
        "$(boot_env_resize_stage_value)" \
        "$(boot_env_boot_mode_value)"

    BOOT_DEVICE_REV=${BOOT_DEFAULT_DEVICE_REV}
    BOOT_SCREEN=${BOOT_DEFAULT_SCREEN}
    echo "Default boot DTB: devicetree-${BOOT_DEVICE_REV}.dtb, screen=${BOOT_SCREEN}"
}
echo "============================================"
echo "  ArkEPass Arch Linux SD Card Image Builder"
echo "============================================"

validate_feature_flags
enter_rootless_backend "$@"
echo "Feature switches: boot_animation=${ENABLE_BOOT_ANIMATION} firstboot=${ENABLE_FIRSTBOOT} resize_rootfs=${ENABLE_RESIZE_ROOTFS} shared_data_partition=${ENABLE_SHARED_DATA_PARTITION} rootfs_size=${ROOTFS_SIZE} sd_cma=24M screen_detect=${ENABLE_SCREEN_DETECT} cpu_overclock=${ENABLE_CPU_OVERCLOCK} drm_gui=${ENABLE_DRM_GUI} systemd_timesyncd=${ENABLE_SYSTEMD_TIMESYNCD} systemd_logind=${ENABLE_SYSTEMD_LOGIND} runtime_swapfile=${ENABLE_SWAPFILE_RUNTIME} preseed_firstboot=${PRESEED_FIRSTBOOT} preseed_rootfs_expanded=${PRESEED_ROOTFS_EXPANDED}"

# --- Sanity checks ---
if [ "$(id -u)" -ne 0 ] && ! is_rootless_backend; then
    echo "ERROR: Must run as root (need losetup/mount)"
    echo "Usage: sudo $0"
    exit 1
fi

LOCK_DIR=/tmp/epass-arch-build-sdcard-${HOST_UID}-${BUILD_MODE_TAG}
LOCK_FILE=${LOCK_DIR}/lock
mkdir -p "${LOCK_DIR}"
exec 9>"${LOCK_FILE}"
if ! flock -n 9; then
    echo "ERROR: another build-sdcard-arch.sh instance is already running"
    exit 1
fi
rm -rf "${BUILD_TMP}"
mkdir -p "${BUILD_TMP}"

if is_rootless_backend; then
    for tool in flock install cpio gzip; do
        command -v "${tool}" >/dev/null 2>&1 \
            || fail_build "Missing required host tool for rootless build: ${tool}"
    done
    [ -x "${BUILDROOT}/output/host/bin/fakeroot" ] \
        || fail_build "Missing host fakeroot: ${BUILDROOT}/output/host/bin/fakeroot"
else
    # Install required tools if missing
    for tool in flock sfdisk losetup mkfs.vfat mkfs.ext4 install cpio gzip; do
        if ! command -v "${tool}" >/dev/null 2>&1; then
            echo "Installing missing tool: ${tool}"
            apt-get install -y dosfstools e2fsprogs util-linux cpio gzip 2>/dev/null || true
        fi
    done
fi

for f in "${IMAGES}/zImage" "${IMAGES}/u-boot-sunxi-with-spl.bin" "${ARCH_TAR}"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Missing file: $f"
        echo "Run './rebuild-kernel.sh' and './rebuild-uboot.sh' first."
        exit 1
    fi
done

check_uboot_sd_bootargs

if is_rootless_backend; then
    echo ""
    echo "=== Step 2: Preparing rootless image staging ==="
    prepare_rootless_staging
    echo "Image assembly backend: fakeroot + genimage"
else
    # --- Step 2: Create image ---
    echo ""
    echo "=== Step 2: Creating ${IMG_SIZE}MB image ==="
    dd if=/dev/zero of=${IMG} bs=1M count=${IMG_SIZE} status=progress

    BOOT_START=$(boot_start_sector)
    BOOT_SECTORS=$(boot_partition_sectors)
    ROOT_START=$(root_start_sector)
    ROOT_SIZE_MB=$(rootfs_partition_size_mb)
    ROOT_SECTORS=$(root_partition_sectors)
    DATA_START=$(data_start_sector)
    DATA_SECTORS=$(data_partition_sectors)

    # Partition layout: p1 FAT32 shared data, p2 ext4 rootfs, p3 FAT32 boot.
    # Physical order stays boot -> rootfs -> data so p1 can grow on first boot.
    if shared_data_partition_enabled; then
        sfdisk ${IMG} << SFDISK_EOF
label: dos
label-id: ${SD_DISK_ID}
unit: sectors

start=${DATA_START}, size=${DATA_SECTORS}, type=c
start=${ROOT_START}, size=${ROOT_SECTORS}, type=83
start=${BOOT_START}, size=${BOOT_SECTORS}, type=1c, bootable
SFDISK_EOF
    else
        sfdisk ${IMG} << SFDISK_EOF
label: dos
label-id: ${SD_DISK_ID}
unit: sectors

start=${BOOT_START}, size=${BOOT_SECTORS}, type=c
start=${ROOT_START}, size=${ROOT_SECTORS}, type=83
SFDISK_EOF
    fi
    echo "Partition table written."

    # --- Step 3: Write U-Boot SPL (raw at 8KB offset) ---
    echo ""
    echo "=== Step 3: Writing U-Boot SPL ==="
    dd if=${IMAGES}/u-boot-sunxi-with-spl.bin of=${IMG} bs=1024 seek=8 conv=notrunc

    # --- Step 4: Mount partitions ---
    echo ""
    echo "=== Step 4: Mounting partitions ==="
    PARTITION_ACCESS_MODE=image-offset
    LOOP=$(timeout 30 losetup -f --show -P "${IMG}" 2>/dev/null || true)
    if [ -n "${LOOP}" ]; then
        BOOT_PART=${LOOP}p1
        ROOT_PART=${LOOP}p2
        DATA_PART=""
        if shared_data_partition_enabled; then
            DATA_PART=${BOOT_PART}
            BOOT_PART=${LOOP}p3
        fi

        for _ in $(seq 1 50); do
            if shared_data_partition_enabled; then
                [ -b "${BOOT_PART}" ] && [ -b "${ROOT_PART}" ] && [ -b "${DATA_PART}" ] && break
            else
                [ -b "${BOOT_PART}" ] && [ -b "${ROOT_PART}" ] && break
            fi
            sleep 0.1
        done

        if [ -b "${BOOT_PART}" ] && [ -b "${ROOT_PART}" ] \
            && { ! shared_data_partition_enabled || [ -b "${DATA_PART}" ]; }; then
            PARTITION_ACCESS_MODE=loop-device
            echo "Loop device: ${LOOP}"
        else
            echo "INFO: loop partition nodes unavailable for ${LOOP}; using file offsets"
            losetup -d "${LOOP}" 2>/dev/null || true
            LOOP=""
        fi
    else
        echo "INFO: losetup -P unavailable; using file offsets"
    fi

    if [ "${PARTITION_ACCESS_MODE}" = "loop-device" ]; then
        mkfs.vfat -F 32 -n "${BOOT_LABEL}" "${BOOT_PART}"
        mkfs.ext4 -L ROOTFS -O ^metadata_csum "${ROOT_PART}"
        if shared_data_partition_enabled; then
            mkfs.vfat -F 32 -n "${DATA_LABEL}" "${DATA_PART}"
        fi
    else
        format_vfat_image_partition "${IMG}" "${BOOT_START}" "${BOOT_SECTORS}" "${BOOT_LABEL}"
        format_ext4_image_partition "${IMG}" "${ROOT_START}" "${ROOT_SECTORS}" ROOTFS
        if shared_data_partition_enabled; then
            format_vfat_image_partition "${IMG}" "${DATA_START}" "${DATA_SECTORS}" "${DATA_LABEL}"
        fi
    fi

    BOOT_MNT=$(mktemp -d)
    ROOT_MNT=$(mktemp -d)
    DATA_MNT="${ROOT_MNT}/mnt/epass-data"

    if [ "${PARTITION_ACCESS_MODE}" = "loop-device" ]; then
        mount "${BOOT_PART}" "${BOOT_MNT}"
        mount "${ROOT_PART}" "${ROOT_MNT}"
    else
        mount_image_partition "${IMG}" "${BOOT_MNT}" vfat "${BOOT_START}" "${BOOT_SECTORS}"
        mount_image_partition "${IMG}" "${ROOT_MNT}" ext4 "${ROOT_START}" "${ROOT_SECTORS}"
    fi
    mkdir -p ${DATA_MNT}
    if shared_data_partition_enabled; then
        DATA_MNT=$(mktemp -d)
        if [ "${PARTITION_ACCESS_MODE}" = "loop-device" ]; then
            if ! mount "${DATA_PART}" "${DATA_MNT}"; then
                echo "WARNING: host cannot mount ${DATA_PART}; shared data will be seeded on first boot"
            fi
        else
            if ! mount_image_partition "${IMG}" "${DATA_MNT}" vfat "${DATA_START}" "${DATA_SECTORS}"; then
                echo "WARNING: host cannot mount shared data partition from ${IMG}; shared data will be seeded on first boot"
            fi
        fi
    fi
fi

# --- Step 5: Populate boot partition ---
echo ""
echo "=== Step 5: Populating boot partition ==="
fat_install_file "${IMAGES}/zImage" "${BOOT_MNT}/zImage"
copy_boot_dtbs
# uEnv.txt - kept for environments that import it before booting from SD.
cat > ${BOOT_MNT}/uEnv.txt << UENVEOF
device_rev=${BOOT_DEVICE_REV:-${BOOT_DEFAULT_DEVICE_REV}}
screen=${BOOT_SCREEN:-${BOOT_DEFAULT_SCREEN}}
epass_firstboot=$(boot_env_firstboot_value)
epass_resize_pending=$(boot_env_resize_pending_value)
epass_resize_stage=$(boot_env_resize_stage_value)
epass_boot_mode=$(boot_env_boot_mode_value)
fdtfile=devicetree.dtb
bootargs=$(build_sd_bootargs)
UENVEOF
echo "Boot partition contents:"
find ${BOOT_MNT} -maxdepth 5 -type f -printf '%P\n' | sort

# --- Step 6: Extract Arch rootfs ---
echo ""
echo "=== Step 6: Extracting Arch Linux ARM rootfs ==="
echo "This may take a few minutes..."
bsdtar -xpf ${ARCH_TAR} -C ${ROOT_MNT} 2>/dev/null || tar --warning=no-unknown-keyword -xzpf ${ARCH_TAR} -C ${ROOT_MNT}
echo "Arch rootfs extracted."

# --- Step 7: Deploy our configs ---
echo ""
echo "=== Step 7: Deploying EPass configs ==="

# systemd services
mkdir -p ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants
mkdir -p ${ROOT_MNT}/etc/systemd/system/basic.target.wants
mkdir -p ${ROOT_MNT}/etc/sysctl.d
mkdir -p ${ROOT_MNT}/etc/profile.d
mkdir -p ${ROOT_MNT}/usr/local/bin
mkdir -p ${ROOT_MNT}/etc/epass ${ROOT_MNT}/etc/epass-firstboot ${ROOT_MNT}/boot

cp ${DEPLOY}/etc/systemd/system/usb-rndis.service ${ROOT_MNT}/etc/systemd/system/
cp ${DEPLOY}/etc/systemd/system/usb-mtp.service ${ROOT_MNT}/etc/systemd/system/
cp ${DEPLOY}/etc/systemd/system/epass-usb-mode.service ${ROOT_MNT}/etc/systemd/system/
cp ${DEPLOY}/etc/systemd/system/zram-swap.service ${ROOT_MNT}/etc/systemd/system/
cp ${DEPLOY}/etc/systemd/system/epass-swapfile-setup.service ${ROOT_MNT}/etc/systemd/system/
cp ${DEPLOY}/etc/systemd/journald.conf ${ROOT_MNT}/etc/systemd/
cp ${DEPLOY}/etc/sysctl.d/swap.conf ${ROOT_MNT}/etc/sysctl.d/
cp ${DEPLOY}/etc/vimrc ${ROOT_MNT}/etc/
cp ${DEPLOY}/etc/profile.d/epass-term.sh ${ROOT_MNT}/etc/profile.d/
# Utility scripts
cp ${DEPLOY}/usr/local/bin/test-rndis.sh ${ROOT_MNT}/usr/local/bin/
cp ${DEPLOY}/usr/local/bin/epass-mem-report.sh ${ROOT_MNT}/usr/local/bin/
cp ${DEPLOY}/usr/local/bin/epass-usb-report.sh ${ROOT_MNT}/usr/local/bin/
cp ${DEPLOY}/usr/local/bin/epass-swapfile-setup.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/test-rndis.sh
chmod +x ${ROOT_MNT}/usr/local/bin/epass-mem-report.sh
chmod +x ${ROOT_MNT}/usr/local/bin/epass-usb-report.sh
chmod +x ${ROOT_MNT}/usr/local/bin/epass-swapfile-setup.sh
chmod +x ${ROOT_MNT}/etc/profile.d/epass-term.sh
if [ "${INCLUDE_DEV_TOOLS}" = "yes" ]; then
    cp ${DEPLOY}/usr/local/bin/neofetch ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/neofetch
fi

# Enable services
ln -sf /etc/systemd/system/zram-swap.service ${ROOT_MNT}/etc/systemd/system/basic.target.wants/
ln -sf /etc/systemd/system/epass-usb-mode.service ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/
rm -f ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/epass-swapfile-setup.service
if [ "${ENABLE_SWAPFILE_RUNTIME}" = "yes" ]; then
    ln -sf /etc/systemd/system/epass-swapfile-setup.service ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/
    echo "  epass-swapfile-setup.service: enabled"
else
    echo "  epass-swapfile-setup.service: disabled (zram only)"
fi
mkdir -p ${ROOT_MNT}/etc/systemd/system/getty.target.wants
mkdir -p ${ROOT_MNT}/etc/systemd/system/serial-getty@ttyS0.service.d
if [ "${ENABLE_DRM_GUI}" = "yes" ]; then
    rm -f ${ROOT_MNT}/etc/systemd/system/getty.target.wants/getty@tty1.service
    echo "  getty@tty1: disabled (DRM GUI owns display)"
else
    echo "  getty@tty1: kept"
fi
cp ${DEPLOY}/etc/systemd/system/serial-getty@ttyS0.service.d/epass-fixed-local.conf \
   ${ROOT_MNT}/etc/systemd/system/serial-getty@ttyS0.service.d/
ln -sf /usr/lib/systemd/system/serial-getty@.service \
   ${ROOT_MNT}/etc/systemd/system/getty.target.wants/serial-getty@ttyS0.service
echo "  epass-usb-mode.service: enabled"
echo "  usb-mtp.service: installed but disabled (persistent USB mode service owns USB mode)"

# Cache drop timer: copied for maintenance, disabled by default to avoid media cold-load stalls.
cp ${DEPLOY}/etc/systemd/system/drop-cache.service ${ROOT_MNT}/etc/systemd/system/
cp ${DEPLOY}/etc/systemd/system/drop-cache.timer ${ROOT_MNT}/etc/systemd/system/
if [ "${ENABLE_DROP_CACHE}" = "yes" ]; then
    mkdir -p ${ROOT_MNT}/etc/systemd/system/timers.target.wants
    ln -sf /etc/systemd/system/drop-cache.timer ${ROOT_MNT}/etc/systemd/system/timers.target.wants/
    echo "  drop-cache.timer: enabled"
else
    echo "  drop-cache.timer: disabled (ENABLE_DROP_CACHE=${ENABLE_DROP_CACHE})"
fi

# Disable unnecessary services (save ~10MB RAM)
for svc in systemd-resolved systemd-networkd systemd-userdbd; do
    ln -sf /dev/null ${ROOT_MNT}/etc/systemd/system/${svc}.service 2>/dev/null
done
for socket in systemd-networkd.socket; do
    ln -sf /dev/null ${ROOT_MNT}/etc/systemd/system/${socket} 2>/dev/null
done
rm -f ${ROOT_MNT}/etc/systemd/system/dbus-org.freedesktop.resolve1.service 2>/dev/null

# The SD image uses the dedicated epass resize initramfs only.
# Arch's generic shutdown-ramfs path can pivot PID 1 into /run/initramfs and
# trigger "Attempted to kill init!" on these systems, so mask it explicitly.
rm -f ${ROOT_MNT}/etc/systemd/system/shutdown.target.wants/mkinitcpio-generate-shutdown-ramfs.service 2>/dev/null
rm -f ${ROOT_MNT}/usr/lib/systemd/system/shutdown.target.wants/mkinitcpio-generate-shutdown-ramfs.service 2>/dev/null
ln -sf /dev/null ${ROOT_MNT}/etc/systemd/system/mkinitcpio-generate-shutdown-ramfs.service 2>/dev/null
echo "  mkinitcpio-generate-shutdown-ramfs.service: masked"

# Linux 5.4 lacks the newer procfs hardening mode expected by systemd 248.
# Only keep the compat drop-ins for the services we intentionally start.
rm -f ${ROOT_MNT}/etc/systemd/system/systemd-logind.service
rm -f ${ROOT_MNT}/etc/systemd/system/systemd-timesyncd.service
rm -f ${ROOT_MNT}/etc/systemd/system/dbus-org.freedesktop.login1.service
rm -f ${ROOT_MNT}/etc/systemd/system/dbus-org.freedesktop.timesync1.service

if [ "${ENABLE_SYSTEMD_LOGIND}" = "yes" ]; then
    mkdir -p "${ROOT_MNT}/etc/systemd/system/systemd-logind.service.d"
    cp "${DEPLOY}/etc/systemd/system/systemd-logind.service.d/epass-kernel54-compat.conf" \
       "${ROOT_MNT}/etc/systemd/system/systemd-logind.service.d/"
    echo "  systemd-logind.service: enabled with kernel 5.4 compat drop-in"
else
    ln -sf /dev/null ${ROOT_MNT}/etc/systemd/system/systemd-logind.service
    ln -sf /dev/null ${ROOT_MNT}/etc/systemd/system/dbus-org.freedesktop.login1.service
    echo "  systemd-logind.service: masked"
fi

if [ "${ENABLE_SYSTEMD_TIMESYNCD}" = "yes" ]; then
    mkdir -p "${ROOT_MNT}/etc/systemd/system/systemd-timesyncd.service.d"
    cp "${DEPLOY}/etc/systemd/system/systemd-timesyncd.service.d/epass-kernel54-compat.conf" \
       "${ROOT_MNT}/etc/systemd/system/systemd-timesyncd.service.d/"
    echo "  systemd-timesyncd.service: enabled with kernel 5.4 compat drop-in"
else
    ln -sf /dev/null ${ROOT_MNT}/etc/systemd/system/systemd-timesyncd.service
    ln -sf /dev/null ${ROOT_MNT}/etc/systemd/system/dbus-org.freedesktop.timesync1.service
    echo "  systemd-timesyncd.service: masked"
fi

# Fix resolv.conf: Arch default is symlink → /run/systemd/resolve/stub-resolv.conf
# With resolved masked, the symlink is dangling → writes fail silently
rm -f ${ROOT_MNT}/etc/resolv.conf
cat > ${ROOT_MNT}/etc/resolv.conf << 'DNSEOF'
nameserver 8.8.8.8
nameserver 1.1.1.1
DNSEOF

CROSS=${BUILDROOT}/output/host/bin/arm-buildroot-linux-gnueabi
SYSROOT=${BUILDROOT}/output/host/arm-buildroot-linux-gnueabi/sysroot

if [ "${ENABLE_FIRSTBOOT}" = "yes" ]; then
    # First-boot hardware/screen selector. It writes hardware selection and
    # switches boot.env into resize mode for the next boot when resize is enabled.
    ${CROSS}-gcc -Os -static --sysroot=${SYSROOT} -DEPASS_ENABLE_RESIZE=$([ "$(resize_supported && printf yes || printf no)" = "yes" ] && printf 1 || printf 0) -o "${BUILD_TMP}/epass-firstboot-select" \
        ${DEPLOY}/usr/local/bin/epass-firstboot-select.c
    ${CROSS}-strip "${BUILD_TMP}/epass-firstboot-select"
    cp "${BUILD_TMP}/epass-firstboot-select" ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/epass-firstboot-select
    cp ${DEPLOY}/etc/systemd/system/epass-firstboot-select.service ${ROOT_MNT}/etc/systemd/system/
    ln -sf /etc/systemd/system/epass-firstboot-select.service ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/
    echo "  epass-firstboot-select.service: enabled"
else
    echo "  epass-firstboot-select.service: skipped"
fi

if [ "${ENABLE_SCREEN_DETECT}" = "yes" ]; then
    # Screen type detection (hsd/laowu/boe R/B swap toggle)
    cp ${DEPLOY}/usr/local/bin/screen-detect.sh ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/screen-detect.sh
    cp ${DEPLOY}/etc/systemd/system/screen-detect.service ${ROOT_MNT}/etc/systemd/system/
    ln -sf /etc/systemd/system/screen-detect.service ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/
    echo "  screen-detect.service: enabled"
else
    echo "  screen-detect.service: skipped"
fi

if resize_supported; then
    ${CROSS}-gcc -Os -static --sysroot=${SYSROOT} -o "${BUILD_TMP}/epass-bootenv" \
        ${DEPLOY}/usr/local/bin/epass-bootenv.c
    ${CROSS}-strip "${BUILD_TMP}/epass-bootenv"
    cp "${BUILD_TMP}/epass-bootenv" ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/epass-bootenv
    build_resize_initramfs
    mkdir -p ${BOOT_MNT}/epass
    fat_install_file "${RESIZE_INITRAMFS_IMAGE}" "${BOOT_MNT}/epass/initramfs-resize.cpio.gz"
    echo "  resize initramfs: installed"
else
    echo "  resize initramfs: skipped"
fi

${CROSS}-gcc -Os -static --sysroot=${SYSROOT} -o "${BUILD_TMP}/epass-usb-mode" \
    ${DEPLOY}/usr/local/bin/epass-usb-mode.c
${CROSS}-strip "${BUILD_TMP}/epass-usb-mode"
cp "${BUILD_TMP}/epass-usb-mode" ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-usb-mode

# fake-hwclock (save/restore time without hardware RTC)
cp ${DEPLOY}/usr/local/bin/fake-hwclock ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/fake-hwclock
cp ${DEPLOY}/etc/systemd/system/fake-hwclock-load.service ${ROOT_MNT}/etc/systemd/system/
cp ${DEPLOY}/etc/systemd/system/fake-hwclock-save.service ${ROOT_MNT}/etc/systemd/system/
mkdir -p ${ROOT_MNT}/etc/systemd/system/sysinit.target.wants
ln -sf /etc/systemd/system/fake-hwclock-load.service ${ROOT_MNT}/etc/systemd/system/sysinit.target.wants/
mkdir -p ${ROOT_MNT}/etc/systemd/system/shutdown.target.wants
ln -sf /etc/systemd/system/fake-hwclock-save.service ${ROOT_MNT}/etc/systemd/system/shutdown.target.wants/
if [ "${ENABLE_SYSTEMD_TIMESYNCD}" = "yes" ]; then
    mkdir -p ${ROOT_MNT}/etc/systemd
    cat > ${ROOT_MNT}/etc/systemd/timesyncd.conf << 'NTPEOF'
[Time]
NTP=pool.ntp.org
FallbackNTP=time.google.com
NTPEOF
else
    rm -f ${ROOT_MNT}/etc/systemd/timesyncd.conf
fi

# Copy devmem tool for TCON register access
cp ${BUILDROOT}/output/target/sbin/devmem ${ROOT_MNT}/usr/local/bin/ 2>/dev/null || true
chmod +x ${ROOT_MNT}/usr/local/bin/devmem 2>/dev/null || true

# Hardware poweroff script (replicates Buildroot BusyBox shutdown)
cp ${DEPLOY}/usr/local/bin/hw-poweroff.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/hw-poweroff.sh
[ -f "${DEPLOY}/usr/local/bin/epass-boot-mark.sh" ] \
    || fail_build "Missing boot marker helper: ${DEPLOY}/usr/local/bin/epass-boot-mark.sh"
cp ${DEPLOY}/usr/local/bin/epass-boot-mark.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-boot-mark.sh
cp ${DEPLOY}/usr/local/bin/epass-data-mount.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-data-mount.sh
cp ${DEPLOY}/usr/local/bin/epass-app-import.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-app-import.sh
cp ${DEPLOY}/usr/local/bin/epass-run-srgn-config.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-run-srgn-config.sh
cp ${DEPLOY}/usr/local/bin/epass-wait-for-gui-ready.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-wait-for-gui-ready.sh
cp ${DEPLOY}/usr/local/bin/epass-gui-should-start.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-gui-should-start.sh
cp ${DEPLOY}/usr/local/bin/epass-gui-preflight.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-gui-preflight.sh
cp ${DEPLOY}/usr/local/bin/epass-gui-fallback.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/epass-gui-fallback.sh
mkdir -p ${ROOT_MNT}/etc/systemd/system/sshd.service.d
cp ${DEPLOY}/etc/systemd/system/sshd.service.d/epass-start-after-gui.conf \
   ${ROOT_MNT}/etc/systemd/system/sshd.service.d/
mkdir -p ${ROOT_MNT}/etc/systemd/system/sshdgenkeys.service.d
cp ${DEPLOY}/etc/systemd/system/sshdgenkeys.service.d/epass-start-after-gui.conf \
   ${ROOT_MNT}/etc/systemd/system/sshdgenkeys.service.d/

if [ "${PRESEED_FIRSTBOOT}" = "yes" ]; then
    preseed_firstboot_state
fi

if [ "${ENABLE_BOOT_ANIMATION}" = "yes" ]; then
    # Boot splash (centered image on framebuffer, runs before DRM app)
    CROSS=${BUILDROOT}/output/host/bin/arm-buildroot-linux-gnueabi
    SYSROOT=${BUILDROOT}/output/host/arm-buildroot-linux-gnueabi/sysroot
    ${CROSS}-gcc -O2 --sysroot=${SYSROOT} -o "${BUILD_TMP}/fb-splash" ${DEPLOY}/usr/local/bin/fb-splash.c
    ${CROSS}-strip "${BUILD_TMP}/fb-splash"
    cp "${BUILD_TMP}/fb-splash" ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/fb-splash
    cp ${DEPLOY}/usr/local/bin/fb-splash.sh ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/fb-splash.sh
    [ -f "${DEPLOY}/etc/systemd/system/epass-boot-animation.service" ] \
        || fail_build "Missing boot animation service: ${DEPLOY}/etc/systemd/system/epass-boot-animation.service"
    cp ${DEPLOY}/etc/systemd/system/epass-boot-animation.service ${ROOT_MNT}/etc/systemd/system/
    ln -sf /etc/systemd/system/epass-boot-animation.service ${ROOT_MNT}/etc/systemd/system/basic.target.wants/
    [ -L "${ROOT_MNT}/etc/systemd/system/basic.target.wants/epass-boot-animation.service" ] \
        || fail_build "Failed to enable epass-boot-animation.service in image"
    [ -f "${DEPLOY}/etc/systemd/system/epass-stop-boot-animation.service" ] \
        || fail_build "Missing boot animation stop service: ${DEPLOY}/etc/systemd/system/epass-stop-boot-animation.service"
    cp ${DEPLOY}/etc/systemd/system/epass-stop-boot-animation.service ${ROOT_MNT}/etc/systemd/system/
    [ -f "${DEPLOY}/etc/systemd/system/epass-gui-ready.path" ] \
        || fail_build "Missing GUI ready path unit: ${DEPLOY}/etc/systemd/system/epass-gui-ready.path"
    cp ${DEPLOY}/etc/systemd/system/epass-gui-ready.path ${ROOT_MNT}/etc/systemd/system/
    ln -sf /etc/systemd/system/epass-gui-ready.path ${ROOT_MNT}/etc/systemd/system/basic.target.wants/
    [ -L "${ROOT_MNT}/etc/systemd/system/basic.target.wants/epass-gui-ready.path" ] \
        || fail_build "Failed to enable epass-gui-ready.path in image"
    # Splash directory (exposed via MTP as "Boot Splash" disk)
    mkdir -p ${ROOT_MNT}/root/splash
    cp ${DEPLOY}/usr/local/bin/png2splash.py ${ROOT_MNT}/root/splash/
    chmod +x ${ROOT_MNT}/root/splash/png2splash.py
    # Default boot animation
    if [ -f "${DEPLOY}/usr/local/bin/default_splash.splash" ]; then
        cp ${DEPLOY}/usr/local/bin/default_splash.splash ${ROOT_MNT}/root/splash/splash.splash
        echo "  boot splash: deployed ($(stat -c%s ${DEPLOY}/usr/local/bin/default_splash.splash) bytes)"
    fi
    # Boot splash overlay text (displayed at bottom of screen by fb-splash)
    if [ -f "${DEPLOY}/usr/local/bin/splash.txt" ]; then
        cp ${DEPLOY}/usr/local/bin/splash.txt ${ROOT_MNT}/root/splash/splash.txt
        echo "  splash text: $(cat ${DEPLOY}/usr/local/bin/splash.txt)"
    fi
    cat > ${ROOT_MNT}/root/splash/README.txt << 'SPLASHEOF'
=== ArkEPass Boot Splash ===

Put splash.splash in this folder to set boot screen.

Static .splash format:
  Byte 0-1: width  (uint16, little-endian)
  Byte 2-3: height (uint16, little-endian)
  Byte 4+:  width * height * 2 bytes RGB565

Animated .splash format:
  Byte 0-3: "ESPL"
  Byte 4-5: width
  Byte 6-7: height
  Byte 8-9: frame count
  Byte 10-11: frame delay in ms
  Byte 12+: frames * width * height * 2 bytes RGB565

Any resolution OK — auto-centered, black background.

Convert from PNG (needs Python + Pillow):
  python3 png2splash.py image.png -o splash.splash
  python3 png2splash.py image.png --resize 360x640 -o splash.splash
  python3 png2splash.py anim.gif -o splash.splash
SPLASHEOF
else
    echo "  epass-boot-animation.service: skipped"
fi

if [ "${ENABLE_CPU_OVERCLOCK}" = "yes" ]; then
    # CPU overclock scripts
    cp ${DEPLOY}/usr/local/bin/overclock.sh ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/overclock.sh
    # Default to stock frequency marker to avoid noisy "usage" on first boot
    echo "408" > ${ROOT_MNT}/root/.oc_freq

    # Overclock service (runs before DRM app, uses saved sweet-spot freq)
    cat > ${ROOT_MNT}/etc/systemd/system/cpu-overclock.service << 'OCEOF'
[Unit]
Description=CPU Overclock
After=sysinit.target
Before=drm-arch-app.service

[Service]
Type=oneshot
ExecStart=/usr/local/bin/overclock.sh
RemainAfterExit=yes

[Install]
WantedBy=basic.target
OCEOF
    ln -sf /etc/systemd/system/cpu-overclock.service ${ROOT_MNT}/etc/systemd/system/basic.target.wants/
    echo "  cpu-overclock.service: enabled"
else
    echo "  cpu-overclock.service: skipped"
fi

# --- Theme system ---
mkdir -p ${ROOT_MNT}/root/themes/arch-blue
cat > ${ROOT_MNT}/root/themes/arch-blue/theme.conf << 'EOF'
name=Arch Blue
dark=1
primary=1793d1
secondary=E05D44
bg_color=15171A
text_color=ffffff
EOF

mkdir -p ${ROOT_MNT}/root/themes/monokai
cat > ${ROOT_MNT}/root/themes/monokai/theme.conf << 'EOF'
name=Monokai
dark=1
primary=A6E22E
secondary=F92672
bg_color=272822
text_color=F8F8F2
EOF

mkdir -p ${ROOT_MNT}/root/themes/nord
cat > ${ROOT_MNT}/root/themes/nord/theme.conf << 'EOF'
name=Nord
dark=1
primary=88C0D0
secondary=BF616A
bg_color=2E3440
text_color=ECEFF4
EOF

echo "arch-blue" > ${ROOT_MNT}/root/.epass_active_theme

cat > ${ROOT_MNT}/root/themes/readme.md << 'READMEEOF'
# ArkEPass Theme Guide

## Create a theme
1. Create a folder here (e.g. `my-theme`)
2. Add `theme.conf` or `theme.json` inside it
3. Optional: add `bg.png` (360x640 recommended) as background image

## theme.conf format
```
name=My Theme
dark=1
primary=1793d1
secondary=E05D44
bg_color=15171A
text_color=ffffff
bg_image=bg.png
```

## theme.json format
```json
{
  "name": "My Theme",
  "dark": true,
  "primary": "1793d1",
  "secondary": "E05D44",
  "bg_color": "15171A",
  "text_color": "ffffff",
  "bg_image": "bg.png"
}
```

## Fields
- name: display name in Settings
- dark: 1=dark mode, 0=light mode
- primary: main color (button focus, arc indicator) 6-digit hex
- secondary: accent color (selected state) 6-digit hex
- bg_color: screen background 6-digit hex
- text_color: default text color 6-digit hex
- bg_image: background image filename (optional, placed in same folder)

## Apply
Select in Settings > Theme. The UI restarts once to reload themed assets.
READMEEOF

# Ensure sysctl loads even if daemon-reload fails
cat > ${ROOT_MNT}/etc/rc.local << 'RCEOF'
#!/bin/sh
sysctl -w vm.vfs_cache_pressure=200 2>/dev/null
sysctl -w vm.min_free_kbytes=4096 2>/dev/null
sysctl -w vm.swappiness=60 2>/dev/null
RCEOF
chmod +x ${ROOT_MNT}/etc/rc.local

# Enable sysrq power-off (176 = sync + remount-ro + power-off)
echo "kernel.sysrq = 176" > ${ROOT_MNT}/etc/sysctl.d/99-epass.conf

# USB mode scripts (unified usbctl.sh + wrapper scripts for compatibility)
cp ${DEPLOY}/usr/local/bin/usbctl.sh ${ROOT_MNT}/usr/local/bin/
chmod +x ${ROOT_MNT}/usr/local/bin/usbctl.sh
# Some callers invoke `usbctl` directly; provide command aliases in common PATHs.
ln -sf /usr/local/bin/usbctl.sh ${ROOT_MNT}/usr/local/bin/usbctl
mkdir -p ${ROOT_MNT}/usr/bin
ln -sf /usr/local/bin/usbctl.sh ${ROOT_MNT}/usr/bin/usbctl
# umtprd binary + config (MTP responder daemon)
cp ${BUILDROOT}/output/target/usr/sbin/umtprd ${ROOT_MNT}/usr/local/bin/ 2>/dev/null || true
chmod +x ${ROOT_MNT}/usr/local/bin/umtprd 2>/dev/null || true
mkdir -p ${ROOT_MNT}/etc/umtprd
cp ${DEPLOY}/etc/umtprd/umtprd.conf ${ROOT_MNT}/etc/umtprd/ 2>/dev/null || true

# fstab: keep root on ext4 and limit writeback churn on SD.
if grep -qE '^/dev/root[[:space:]]+/' ${ROOT_MNT}/etc/fstab 2>/dev/null; then
    sed -i -E 's|^/dev/root[[:space:]]+/[[:space:]]+[^[:space:]]+[[:space:]]+[^[:space:]]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+|/dev/root\t/\t\text4\trw,noatime,nodiratime,commit=60\t0\t1|' ${ROOT_MNT}/etc/fstab
else
    printf '/dev/root\t/\t\text4\trw,noatime,nodiratime,commit=60\t0\t1\n' >> ${ROOT_MNT}/etc/fstab
fi
if ! grep -qE '^[^#]+[[:space:]]+/run[[:space:]]+tmpfs[[:space:]]+' ${ROOT_MNT}/etc/fstab 2>/dev/null; then
    printf 'tmpfs\t/run\ttmpfs\tmode=755,nosuid,nodev,size=32M\t0\t0\n' >> ${ROOT_MNT}/etc/fstab
fi
# fstab: mount boot partition only when requested; firstboot uses /run/epass-boot.
echo "LABEL=${BOOT_LABEL} /boot vfat noauto,nofail,rw,umask=022 0 0" >> ${ROOT_MNT}/etc/fstab
if shared_data_partition_enabled; then
    echo "LABEL=${DATA_LABEL} /mnt/epass-data vfat noauto,nofail,rw,uid=0,gid=0,umask=022,utf8=1 0 0" >> ${ROOT_MNT}/etc/fstab
fi
# Hostname
echo "${HOSTNAME_VALUE}" > ${ROOT_MNT}/etc/hostname

# SSH: allow root password login
sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' ${ROOT_MNT}/etc/ssh/sshd_config 2>/dev/null || true

# Shared libraries from Buildroot
echo "Copying shared libraries..."
for lib in libhavege; do
    for f in ${BUILDROOT}/output/target/usr/lib/${lib}.so*; do
        [ -f "$f" ] && cp -a "$f" ${ROOT_MNT}/usr/lib/ && echo "  $(basename $f)"
    done
done
if [ "${INCLUDE_DEV_TOOLS}" = "yes" ]; then
    for f in ${BUILDROOT}/output/target/usr/lib/libncursesw.so*; do
        [ -f "$f" ] && cp -a "$f" ${ROOT_MNT}/usr/lib/ && echo "  $(basename $f)"
    done
    # terminfo database (needed for SSH terminal sessions)
    echo "Copying terminfo database..."
    if [ -d "${BUILDROOT}/output/target/usr/share/terminfo" ]; then
        mkdir -p ${ROOT_MNT}/usr/share/terminfo
        cp -r ${BUILDROOT}/output/target/usr/share/terminfo/* ${ROOT_MNT}/usr/share/terminfo/
        echo "  terminfo: copied"
    fi
fi

# Kernel modules (for CardKB, sound, etc.)
echo "Copying kernel modules..."
if [ -d "${BUILDROOT}/output/target/lib/modules/5.4.99" ]; then
    mkdir -p ${ROOT_MNT}/lib/modules/
    cp -r ${BUILDROOT}/output/target/lib/modules/5.4.99 ${ROOT_MNT}/lib/modules/
    echo "  modules/5.4.99: copied"
fi

# Auto-load CardKB module
mkdir -p ${ROOT_MNT}/etc/modules-load.d
echo "cardkb" > ${ROOT_MNT}/etc/modules-load.d/cardkb.conf

# --- Step 8: Deploy haveged (entropy daemon) ---
echo ""
echo "=== Step 8: Deploying haveged ==="
if [ -f "${BUILDROOT}/output/target/usr/sbin/haveged" ]; then
    cp ${BUILDROOT}/output/target/usr/sbin/haveged ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/haveged
    echo "  haveged: deployed"
else
    echo "  haveged: NOT FOUND (skipped)"
fi

if [ "${INCLUDE_DEV_TOOLS}" = "yes" ]; then
    # Deploy btop (system monitor for SSH sessions)
    if [ -f "${BUILDROOT}/output/target/usr/bin/btop" ]; then
        cp ${BUILDROOT}/output/target/usr/bin/btop ${ROOT_MNT}/usr/local/bin/
        chmod +x ${ROOT_MNT}/usr/local/bin/btop
        if [ -d "${BUILDROOT}/output/target/usr/share/btop/themes" ]; then
            mkdir -p ${ROOT_MNT}/usr/local/share/btop
            cp -r ${BUILDROOT}/output/target/usr/share/btop/themes ${ROOT_MNT}/usr/local/share/btop/
        fi
        if [ -f "${BUILDROOT}/output/target/usr/share/btop/README.md" ]; then
            mkdir -p ${ROOT_MNT}/usr/local/share/btop
            cp ${BUILDROOT}/output/target/usr/share/btop/README.md ${ROOT_MNT}/usr/local/share/btop/
        fi
        echo "  btop: deployed"
    else
        echo "  btop: NOT FOUND (run 'make btop' or reload the epass defconfig first)"
    fi

    # Deploy vim (text editor for SSH sessions)
    if [ -f "${BUILDROOT}/output/target/usr/bin/vim" ]; then
        cp ${BUILDROOT}/output/target/usr/bin/vim ${ROOT_MNT}/usr/local/bin/
        chmod +x ${ROOT_MNT}/usr/local/bin/vim
        echo "  vim: deployed"
    fi
else
    echo "  dev tools: skipped (INCLUDE_DEV_TOOLS=${INCLUDE_DEV_TOOLS})"
fi

# Deploy MicroPython (keep lightweight REPL/tooling available on device)
if [ -f "${BUILDROOT}/output/target/usr/bin/micropython" ]; then
    cp ${BUILDROOT}/output/target/usr/bin/micropython ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/micropython
    echo "  micropython: deployed"
else
    echo "  micropython: NOT FOUND (run 'make micropython-rebuild' in buildroot first)"
fi

# Deploy Python 3.11 (cross-compiled independently)
PYTHON311_DIR="${BUILDROOT}/epass-arch/python-install"
if [ "${INCLUDE_PYTHON}" = "yes" ] && [ -d "${PYTHON311_DIR}/usr/bin" ] && [ -f "${PYTHON311_DIR}/usr/bin/python3.11" ]; then
    echo "Deploying Python 3.11..."
    # Binary
    cp ${PYTHON311_DIR}/usr/bin/python3.11 ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/python3.11
    ln -sf python3.11 ${ROOT_MNT}/usr/local/bin/python3
    ln -sf python3.11 ${ROOT_MNT}/usr/local/bin/python
    # Standard library
    if [ -d "${PYTHON311_DIR}/usr/lib/python3.11" ]; then
        mkdir -p ${ROOT_MNT}/usr/lib/
        cp -a ${PYTHON311_DIR}/usr/lib/python3.11 ${ROOT_MNT}/usr/lib/
        PYCOUNT=$(find ${ROOT_MNT}/usr/lib/python3.11 -type f | wc -l)
        echo "  python3.11 stdlib: deployed (${PYCOUNT} files)"
    fi
    # Shared library
    for f in ${PYTHON311_DIR}/usr/lib/libpython3.11*.so*; do
        [ -f "$f" ] && cp -a "$f" ${ROOT_MNT}/usr/lib/
    done
    echo "  python3.11: deployed"
elif [ "${INCLUDE_PYTHON}" = "yes" ] && [ -f "${BUILDROOT}/output/target/usr/bin/python3.8" ]; then
    # Fallback: use Buildroot's Python 3.8 if 3.11 not built
    echo "WARNING: Python 3.11 not found, falling back to 3.8"
    cp ${BUILDROOT}/output/target/usr/bin/python3.8 ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/python3.8
    ln -sf python3.8 ${ROOT_MNT}/usr/local/bin/python3
    ln -sf python3.8 ${ROOT_MNT}/usr/local/bin/python
    PYTHON_TARGET_LIB="${BUILDROOT}/output/target/usr/lib/python3.8"
    if [ -d "${PYTHON_TARGET_LIB}" ]; then
        mkdir -p ${ROOT_MNT}/usr/lib/
        cp -a ${PYTHON_TARGET_LIB} ${ROOT_MNT}/usr/lib/
    fi
    for f in ${BUILDROOT}/output/target/usr/lib/libpython3*; do
        [ -f "$f" ] && cp -a "$f" ${ROOT_MNT}/usr/lib/
    done
    echo "  python3.8: deployed (fallback)"
else
    if [ "${INCLUDE_PYTHON}" = "yes" ]; then
        echo "  WARNING: No Python found! Run ./build_python311.sh or 'make python3-rebuild'"
    else
        echo "  python: skipped (INCLUDE_PYTHON=${INCLUDE_PYTHON})"
    fi
fi

# --- Step 8b: Deploy drm_arch_app (replaces LVGL UI) ---
DRM_APP_BIN="${BUILDROOT}/output/target/usr/bin/drm_arch_app"
TARGET_LIB=${BUILDROOT}/output/target/usr/lib

if [ "${ENABLE_DRM_GUI}" = "yes" ] && [ -f "${DRM_APP_BIN}" ]; then
    verify_drm_app_bin_freshness "${DRM_APP_BIN}"

    # 1. Main binary
    cp ${DRM_APP_BIN} ${ROOT_MNT}/usr/local/bin/drm_arch_app
    chmod +x ${ROOT_MNT}/usr/local/bin/drm_arch_app
    ln -sf /usr/local/bin/drm_arch_app ${ROOT_MNT}/root/drm_arch_app
    echo "  drm_arch_app: deployed ($(stat -c%s ${DRM_APP_BIN}) bytes)"

    # 2. CedarX libraries
    for lib in libMemAdapter libVE libvideoengine libvdecoder \
               libcdc_base libcdx_base libcdx_common libcdx_parser libcdx_stream libcdx_playback \
               libawh264 libawmjpeg libawmjpegplus libawmpeg2; do
        if [ -f "${TARGET_LIB}/${lib}.so" ]; then
            cp -a "${TARGET_LIB}/${lib}.so" ${ROOT_MNT}/usr/lib/
            echo "  ${lib}.so"
        fi
    done

    # 3. Dependency libraries: copy only missing non-glibc NEEDED entries.
    copy_needed_libs "${DRM_APP_BIN}"
    for cedar_elf in ${ROOT_MNT}/usr/lib/libMemAdapter.so ${ROOT_MNT}/usr/lib/libVE.so \
                     ${ROOT_MNT}/usr/lib/libvideoengine.so ${ROOT_MNT}/usr/lib/libvdecoder.so \
                     ${ROOT_MNT}/usr/lib/libcdc_base.so ${ROOT_MNT}/usr/lib/libcdx_base.so \
                     ${ROOT_MNT}/usr/lib/libcdx_common.so ${ROOT_MNT}/usr/lib/libcdx_parser.so \
                     ${ROOT_MNT}/usr/lib/libcdx_stream.so ${ROOT_MNT}/usr/lib/libcdx_playback.so; do
        [ -f "${cedar_elf}" ] && copy_needed_libs "${cedar_elf}"
    done

    # 4. Common third-party foreground app runtime libraries not referenced by drm_arch_app itself.
    copy_optional_runtime_lib "libgpiod.so.2"

    # 5. Resource files
    mkdir -p \
        ${ROOT_MNT}/root/res/fallback \
        ${ROOT_MNT}/root/themes \
        ${ROOT_MNT}/assets \
        ${ROOT_MNT}/app \
        ${ROOT_MNT}/dispimg \
        ${ROOT_MNT}/mnt/epass-data \
        ${DATA_MNT}/assets \
        ${DATA_MNT}/display-images \
        ${DATA_MNT}/themes \
        ${DATA_MNT}/apps-inbox \
        ${DATA_MNT}/import-log

    RES_SRC=${BUILDROOT}/board/rhodesisland/epass/rootfs/root/res
    FALLBACK_SRC=${RES_SRC}/fallback
    FALLBACK_DST=${ROOT_MNT}/root/res/fallback
    FALLBACK_VIDEO=${FALLBACK_SRC}/loop_1.mp4
    FALLBACK_CFG=${FALLBACK_SRC}/epconfig.json

    if ! compgen -G "${RES_SRC}/*.png" > /dev/null; then
        fail_build "Required UI PNG resources missing: ${RES_SRC}/*.png"
    fi
    [ -f "${FALLBACK_VIDEO}" ] || fail_build "Fallback video missing: ${FALLBACK_VIDEO}"
    [ -f "${FALLBACK_CFG}" ] || fail_build "Fallback config missing: ${FALLBACK_CFG}"
    grep -Eq '"screen"[[:space:]]*:[[:space:]]*"360x640"' "${FALLBACK_CFG}" \
        || fail_build "Fallback config must set screen=360x640: ${FALLBACK_CFG}"
    cp ${RES_SRC}/*.png ${ROOT_MNT}/root/res/
    if [ ! -d "${FALLBACK_SRC}" ]; then
        fail_build "Fallback source directory missing: ${FALLBACK_SRC}"
    fi
    cp -r ${FALLBACK_SRC}/. ${FALLBACK_DST}/

    for req in epconfig.json loop_1.mp4; do
        if [ ! -f "${FALLBACK_DST}/${req}" ]; then
            fail_build "Fallback resource missing after copy: ${FALLBACK_DST}/${req}"
        fi
    done
    echo "  resources: deployed"

    # 4b. EPass paths config (env + config file)
    cat > ${ROOT_MNT}/etc/epass.conf << 'EPASSEOF'
ASSETS_DIR=/assets
APPS_DIR=/app
EPASSEOF
    if [ -d "${BUILDROOT}/epass-arch/assets-src" ]; then
        cp -r ${BUILDROOT}/epass-arch/assets-src/* ${ROOT_MNT}/assets/ 2>/dev/null || true
        cp -r ${BUILDROOT}/epass-arch/assets-src/* ${DATA_MNT}/assets/ 2>/dev/null || true
        if mountpoint -q "${DATA_MNT}" 2>/dev/null; then
            echo "  assets: prepopulated"
        else
            echo "  assets: staged in rootfs fallback (shared data will seed on first boot)"
        fi
    else
        echo "  assets: using built-in PRTS fallback (${FALLBACK_DST})"
    fi
    populate_shared_data_partition

    # 5. CedarX config
    echo -e "[paramter]\nlog_level = 6" > ${ROOT_MNT}/etc/cedarx.conf

    # 6. systemd runner and service
    cp ${DEPLOY}/usr/local/bin/drm-arch-app-runner.sh ${ROOT_MNT}/usr/local/bin/
    chmod +x ${ROOT_MNT}/usr/local/bin/drm-arch-app-runner.sh
    if [ -x "${BUILDROOT}/output/target/usr/bin/srgn_config" ]; then
        cp ${BUILDROOT}/output/target/usr/bin/srgn_config ${ROOT_MNT}/usr/local/bin/srgn_config
        chmod +x ${ROOT_MNT}/usr/local/bin/srgn_config
        echo "  srgn_config: deployed"
    else
        echo "  WARNING: srgn_config not found; exit code 5 will return to DRM app"
    fi
    [ -f "${DEPLOY}/etc/systemd/system/drm-arch-app.service" ] \
        || fail_build "Missing service template: ${DEPLOY}/etc/systemd/system/drm-arch-app.service"
    cp ${DEPLOY}/etc/systemd/system/drm-arch-app.service ${ROOT_MNT}/etc/systemd/system/
    rm -f ${ROOT_MNT}/etc/systemd/system/screen-detect.service.wants/drm-arch-app.service
    ln -sf /etc/systemd/system/drm-arch-app.service ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/
    [ -L "${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/drm-arch-app.service" ] \
        || fail_build "Failed to enable drm-arch-app.service in multi-user.target"
    if shared_data_partition_enabled; then
        [ -f "${DEPLOY}/etc/systemd/system/epass-data-mount.service" ] \
            || fail_build "Missing data mount service template: ${DEPLOY}/etc/systemd/system/epass-data-mount.service"
        cp ${DEPLOY}/etc/systemd/system/epass-data-mount.service ${ROOT_MNT}/etc/systemd/system/
        ln -sf /etc/systemd/system/epass-data-mount.service ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/
        [ -L "${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/epass-data-mount.service" ] \
            || fail_build "Failed to enable epass-data-mount.service in multi-user.target"
    fi
    [ -f "${DEPLOY}/etc/systemd/system/epass-gui-fallback.service" ] \
        || fail_build "Missing fallback service template: ${DEPLOY}/etc/systemd/system/epass-gui-fallback.service"
    cp ${DEPLOY}/etc/systemd/system/epass-gui-fallback.service ${ROOT_MNT}/etc/systemd/system/
    # NOTE: exit code 4 (FORMAT_SD) removed — SD card IS the root filesystem on Arch

    # 7. Enable service
    echo "  drm-arch-app.service: enabled in multi-user.target"
elif [ "${ENABLE_DRM_GUI}" = "yes" ]; then
    fail_build "drm_arch_app missing at ${DRM_APP_BIN}; run ./epass-arch/build_drm_arch_app.sh or make drm_arch_app-reconfigure"
else
    echo "  drm-arch-app.service: skipped"
fi

# --- Step 9: Setup haveged for entropy ---
if [ -f "${ROOT_MNT}/usr/local/bin/haveged" ]; then
    cat > ${ROOT_MNT}/etc/systemd/system/haveged.service << 'EOF'
[Unit]
Description=Entropy Daemon (haveged)
DefaultDependencies=no
After=sysinit.target
Before=systemd-random-seed.service

[Service]
Type=simple
ExecStart=/usr/local/bin/haveged -F
Restart=always

[Install]
WantedBy=multi-user.target
EOF
    ln -sf /etc/systemd/system/haveged.service ${ROOT_MNT}/etc/systemd/system/multi-user.target.wants/
    echo "  haveged service: enabled"
fi

# Normalize unit file permissions: systemd warns on executable .service/.timer files.
if [ -d "${ROOT_MNT}/etc/systemd/system" ]; then
    find "${ROOT_MNT}/etc/systemd/system" -maxdepth 1 -type f \( -name "*.service" -o -name "*.timer" \) -exec chmod 644 {} +
fi
[ -f "${ROOT_MNT}/etc/systemd/journald.conf" ] && chmod 644 "${ROOT_MNT}/etc/systemd/journald.conf"
[ -f "${ROOT_MNT}/etc/systemd/timesyncd.conf" ] && chmod 644 "${ROOT_MNT}/etc/systemd/timesyncd.conf"

# --- Step 10: Set root password and profile ---
echo ""
echo "=== Step 9: Configuring root user ==="
# Create .bash_profile (SSH login shells read this, NOT .bashrc)
cat > ${ROOT_MNT}/root/.bash_profile << 'BPEOF'
[[ -f ~/.bashrc ]] && . ~/.bashrc
BPEOF

# Shell environment
cat >> ${ROOT_MNT}/root/.bashrc << 'BASHEOF'

# ArkEPass environment
export TERM=${TERM:-xterm-256color}
export LANG=C.UTF-8
export HOME=${HOME:-/root}

# Correct PS1 (readline needs \[...\] around ANSI codes for proper line wrapping)
PS1='\[\e[32m\]\u@\h\[\e[0m\]:\[\e[34m\]\w\[\e[0m\]\$ '

# TAB completion
[ -r /usr/share/bash-completion/bash_completion ] && source /usr/share/bash-completion/bash_completion
bind 'set show-all-if-ambiguous on' 2>/dev/null
bind 'set completion-ignore-case on' 2>/dev/null

# Aliases
alias ll='ls -la'
alias vi='vim 2>/dev/null || command vi'
BASHEOF

# --- Cleanup & unmount ---
echo ""
echo "=== Step 10: Finalizing image ==="
check_staged_rootfs_capacity
if is_rootless_backend; then
    build_rootless_sd_image
    rm -rf "${ROOTLESS_WORKDIR}"
    ROOTLESS_WORKDIR=""
    ROOTLESS_GENROOT=""
else
    sync
    umount "${BOOT_MNT}"
    umount "${ROOT_MNT}"
    if shared_data_partition_enabled && mountpoint -q "${DATA_MNT}"; then
        umount "${DATA_MNT}"
    fi
    if [ -n "${LOOP}" ]; then
        losetup -d "${LOOP}"
    fi
    if shared_data_partition_enabled; then
        cleanup_temp_dir "${BOOT_MNT}"
        cleanup_temp_dir "${ROOT_MNT}"
        cleanup_temp_dir "${DATA_MNT}"
    else
        cleanup_temp_dir "${BOOT_MNT}"
        cleanup_temp_dir "${ROOT_MNT}"
    fi
fi

echo ""
echo "============================================"
echo "  Image built successfully!"
echo "  File: ${IMG}"
echo "  Size: $(du -h ${IMG} | cut -f1)"
echo "============================================"
echo ""
echo "Burn to SD card:"
echo "  sudo dd if=${IMG} of=/dev/sdX bs=4M status=progress"
echo ""
echo "Or use balenaEtcher on Windows."
echo ""
echo "Default login: root / root (Arch default)"
echo "USB default: MTP file transfer (use usbctl rndis for network maintenance)"
