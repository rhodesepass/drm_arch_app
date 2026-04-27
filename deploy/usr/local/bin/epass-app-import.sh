#!/bin/bash
set -u

PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

DATA_MNT=/mnt/epass-data
INBOX_DIR=${DATA_MNT}/apps-inbox
IMPORT_LOG_DIR=${DATA_MNT}/import-log
APP_DIR=/app
STATE_DIR=/var/lib/epass
STATE_FILE=${STATE_DIR}/app-import-state.tsv
TMP_ROOT=/tmp/epass-app-import
CHANGED=0

log_line() {
    local level="$1"
    local msg="$2"

    mkdir -p "${IMPORT_LOG_DIR}" "${STATE_DIR}" "${TMP_ROOT}" 2>/dev/null || true
    printf '%s [%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "${level}" "${msg}" >> "${IMPORT_LOG_DIR}/app-import.log" 2>/dev/null || true
}

archive_fingerprint() {
    stat -c '%s:%Y' "$1" 2>/dev/null
}

directory_fingerprint() {
    local dir="$1"

    (
        find "${dir}" -mindepth 1 \( -type f -o -type d \) -print 2>/dev/null | sort |
        while IFS= read -r path; do
            if [ -d "${path}" ]; then
                stat -c 'd:%n:%Y' "${path}" 2>/dev/null
            else
                stat -c 'f:%n:%s:%Y' "${path}" 2>/dev/null
            fi
        done
    ) | cksum | awk '{print $1 ":" $2}'
}

state_get() {
    local entry_name="$1"

    [ -f "${STATE_FILE}" ] || return 1
    awk -F '\t' -v name="${entry_name}" '$1 == name { print $2; exit 0 }' "${STATE_FILE}"
}

state_put() {
    local entry_name="$1"
    local fingerprint="$2"
    local tmp_state

    mkdir -p "${STATE_DIR}" 2>/dev/null || true
    tmp_state=$(mktemp "${TMP_ROOT}/state.XXXXXX")
    if [ -f "${STATE_FILE}" ]; then
        awk -F '\t' -v name="${entry_name}" '$1 != name { print $0 }' "${STATE_FILE}" > "${tmp_state}"
    fi
    printf '%s\t%s\n' "${entry_name}" "${fingerprint}" >> "${tmp_state}"
    mv "${tmp_state}" "${STATE_FILE}"
}

top_level_entry_count() {
    local archive="$1"

    tar -tzf "${archive}" 2>/dev/null | awk -F/ 'NF > 0 && $1 != "" {print $1}' | sort -u | wc -l
}

top_level_entry_name() {
    local archive="$1"

    tar -tzf "${archive}" 2>/dev/null | awk -F/ 'NF > 0 && $1 != "" {print $1}' | sort -u | head -n 1
}

install_package_root() {
    local package_root="$1"
    local install_name="$2"
    local source_name="$3"
    local state_key="$4"
    local fingerprint="$5"
    local install_tmp

    if [ ! -f "${package_root}/appconfig.json" ]; then
        log_line "ERROR" "missing appconfig.json: ${source_name}"
        return 1
    fi

    mkdir -p "${APP_DIR}" 2>/dev/null || true
    install_tmp="${APP_DIR}/.${install_name}.import.$$"
    rm -rf "${install_tmp}"
    cp -a "${package_root}" "${install_tmp}"
    chmod -R u+rwX,go+rX "${install_tmp}" 2>/dev/null || true

    rm -rf "${APP_DIR}/${install_name}"
    mv "${install_tmp}" "${APP_DIR}/${install_name}"
    state_put "${state_key}" "${fingerprint}"
    CHANGED=1
    log_line "INFO" "installed ${source_name} -> ${APP_DIR}/${install_name}"
    return 0
}

install_archive() {
    local archive="$1"
    local archive_name archive_base state_key fingerprint prev_fp extract_dir top_count top_name
    local package_root install_name install_tmp

    archive_name=$(basename "${archive}")
    archive_base=${archive_name%.tar.gz}
    archive_base=${archive_base%.tgz}
    state_key="archive:${archive_name}"
    fingerprint=$(archive_fingerprint "${archive}")
    prev_fp=$(state_get "${state_key}" || true)

    if [ -n "${prev_fp}" ] && [ "${prev_fp}" = "${fingerprint}" ]; then
        return 0
    fi

    extract_dir=$(mktemp -d "${TMP_ROOT}/pkg.XXXXXX")
    if ! tar -xzf "${archive}" -C "${extract_dir}" >/dev/null 2>&1; then
        log_line "ERROR" "extract failed: ${archive_name}"
        rm -rf "${extract_dir}"
        return 1
    fi

    top_count=$(top_level_entry_count "${archive}")
    top_name=$(top_level_entry_name "${archive}")

    if [ "${top_count}" = "1" ] && [ -d "${extract_dir}/${top_name}" ]; then
        package_root="${extract_dir}/${top_name}"
        install_name="${top_name}"
    else
        package_root="${extract_dir}"
        install_name="${archive_base}"
    fi

    install_package_root "${package_root}" "${install_name}" "${archive_name}" "${state_key}" "${fingerprint}" || {
        rm -rf "${extract_dir}"
        return 1
    }
    rm -rf "${extract_dir}"
    return 0
}

install_directory() {
    local app_dir="$1"
    local dir_name state_key fingerprint prev_fp

    dir_name=$(basename "${app_dir}")
    state_key="dir:${dir_name}"
    fingerprint=$(directory_fingerprint "${app_dir}")
    prev_fp=$(state_get "${state_key}" || true)

    if [ -n "${prev_fp}" ] && [ "${prev_fp}" = "${fingerprint}" ]; then
        return 0
    fi

    install_package_root "${app_dir}" "${dir_name}" "${dir_name}/" "${state_key}" "${fingerprint}"
}

main() {
    local archive
    local app_dir

    [ -d "${INBOX_DIR}" ] || exit 10
    mkdir -p "${TMP_ROOT}" "${APP_DIR}" "${STATE_DIR}" 2>/dev/null || true

    while IFS= read -r -d '' archive; do
        install_archive "${archive}" || true
    done < <(find "${INBOX_DIR}" -maxdepth 1 -type f \( -name '*.tar.gz' -o -name '*.tgz' \) -print0 | sort -z)

    while IFS= read -r -d '' app_dir; do
        install_directory "${app_dir}" || true
    done < <(find "${INBOX_DIR}" -maxdepth 1 -mindepth 1 -type d ! -name '.*' -print0 | sort -z)

    if [ "${CHANGED}" -eq 0 ]; then
        exit 10
    fi

    exit 0
}

main "$@"
