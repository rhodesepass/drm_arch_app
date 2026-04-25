#include "apps/apps_importer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "utils/log.h"
#include "utils/misc.h"

#define APPS_INBOX_ARCHIVE "/root/apps-inbox.tar.gz"
#define APPS_IMPORT_TMP_TEMPLATE "/tmp/apps-inbox.XXXXXX"
#define APPS_IMPORT_PATH_MAX 256

static int run_process(char *const argv[])
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) {
        log_error("apps inbox: fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        log_error("apps inbox: waitpid failed: %s", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }

    if (WIFEXITED(status)) {
        log_error("apps inbox: command failed with exit status %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_error("apps inbox: command killed by signal %d", WTERMSIG(status));
    } else {
        log_error("apps inbox: command failed: 0x%x", status);
    }

    return -1;
}

static int remove_tree(const char *path)
{
    DIR *dir;
    struct dirent *entry;
    struct stat st;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path);
    }

    dir = opendir(path);
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char child[APPS_IMPORT_PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        join_path(child, sizeof(child), path, entry->d_name);
        if (remove_tree(child) != 0) {
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);
    return rmdir(path);
}

static int ensure_directory(const char *path, mode_t mode)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        return chmod(path, mode & 0777);
    }

    if (errno != ENOENT) {
        return -1;
    }

    return mkdir(path, mode & 0777);
}

static int copy_regular_file(const char *src, const char *dst, mode_t mode)
{
    int src_fd = -1;
    int dst_fd = -1;
    char buf[8192];
    ssize_t nread;

    src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        return -1;
    }

    dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (dst_fd < 0) {
        close(src_fd);
        return -1;
    }

    while ((nread = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;

        while (written < nread) {
            ssize_t n = write(dst_fd, buf + written, (size_t)(nread - written));
            if (n < 0) {
                close(dst_fd);
                close(src_fd);
                return -1;
            }
            written += n;
        }
    }

    close(dst_fd);
    close(src_fd);

    if (nread < 0) {
        return -1;
    }

    return chmod(dst, mode & 0777);
}

static int copy_symlink(const char *src, const char *dst)
{
    char target[APPS_IMPORT_PATH_MAX];
    ssize_t len;

    len = readlink(src, target, sizeof(target) - 1);
    if (len < 0) {
        return -1;
    }

    target[len] = '\0';
    unlink(dst);
    return symlink(target, dst);
}

static int copy_node(const char *src, const char *dst)
{
    DIR *dir;
    struct dirent *entry;
    struct stat st;

    if (lstat(src, &st) != 0) {
        return -1;
    }

    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        if (ensure_directory(dst, st.st_mode) != 0) {
            return -1;
        }

        dir = opendir(src);
        if (dir == NULL) {
            return -1;
        }

        while ((entry = readdir(dir)) != NULL) {
            char src_child[APPS_IMPORT_PATH_MAX];
            char dst_child[APPS_IMPORT_PATH_MAX];

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            join_path(src_child, sizeof(src_child), src, entry->d_name);
            join_path(dst_child, sizeof(dst_child), dst, entry->d_name);
            if (copy_node(src_child, dst_child) != 0) {
                closedir(dir);
                return -1;
            }
        }

        closedir(dir);
        return chmod(dst, st.st_mode & 0777);
    }

    if (S_ISREG(st.st_mode)) {
        return copy_regular_file(src, dst, st.st_mode);
    }

    if (S_ISLNK(st.st_mode)) {
        return copy_symlink(src, dst);
    }

    log_warn("apps inbox: skipping unsupported file type: %s", src);
    return 0;
}

static bool is_directory(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

static int select_import_root(const char *extract_dir, char *dst, size_t dst_sz)
{
    DIR *dir;
    struct dirent *entry;
    char app_root[APPS_IMPORT_PATH_MAX];
    char single_entry[APPS_IMPORT_PATH_MAX];
    int entry_count = 0;

    join_path(app_root, sizeof(app_root), extract_dir, "app");
    if (is_directory(app_root)) {
        safe_strcpy(dst, dst_sz, app_root);
        return 0;
    }

    dir = opendir(extract_dir);
    if (dir == NULL) {
        return -1;
    }

    single_entry[0] = '\0';
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        entry_count++;
        join_path(single_entry, sizeof(single_entry), extract_dir, entry->d_name);
        if (entry_count > 1) {
            break;
        }
    }
    closedir(dir);

    if (entry_count == 1 && is_directory(single_entry)) {
        safe_strcpy(dst, dst_sz, single_entry);
        return 0;
    }

    safe_strcpy(dst, dst_sz, extract_dir);
    return 0;
}

static int install_import_root(const char *src_root)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(src_root);
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char src_path[APPS_IMPORT_PATH_MAX];
        char dst_path[APPS_IMPORT_PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        join_path(src_path, sizeof(src_path), src_root, entry->d_name);
        join_path(dst_path, sizeof(dst_path), APPS_DIR, entry->d_name);

        if (remove_tree(dst_path) != 0 && errno != ENOENT) {
            log_error("apps inbox: failed to remove %s: %s", dst_path, strerror(errno));
            closedir(dir);
            return -1;
        }

        if (copy_node(src_path, dst_path) != 0) {
            log_error("apps inbox: failed to install %s: %s", entry->d_name, strerror(errno));
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);
    return 0;
}

int apps_import_inbox(void)
{
    char tmp_dir[] = APPS_IMPORT_TMP_TEMPLATE;
    char import_root[APPS_IMPORT_PATH_MAX];
    char *extract_argv[] = { "tar", "-xzf", (char *)APPS_INBOX_ARCHIVE, "-C", tmp_dir, NULL };
    int ret = 0;

    if (!file_exists_readable(APPS_INBOX_ARCHIVE)) {
        return 0;
    }

    if (mkdtemp(tmp_dir) == NULL) {
        log_error("apps inbox: mkdtemp failed: %s", strerror(errno));
        return -1;
    }

    log_info("apps inbox: importing %s", APPS_INBOX_ARCHIVE);

    if (run_process(extract_argv) != 0) {
        log_error("apps inbox: failed to extract archive");
        ret = -1;
        goto cleanup;
    }

    if (select_import_root(tmp_dir, import_root, sizeof(import_root)) != 0) {
        log_error("apps inbox: failed to resolve import root");
        ret = -1;
        goto cleanup;
    }

    if (install_import_root(import_root) != 0) {
        ret = -1;
        goto cleanup;
    }

    if (unlink(APPS_INBOX_ARCHIVE) != 0) {
        log_warn("apps inbox: imported, but failed to remove archive: %s", strerror(errno));
    }

    log_info("apps inbox: import completed");
    ret = 1;

cleanup:
    if (remove_tree(tmp_dir) != 0) {
        log_warn("apps inbox: failed to cleanup %s: %s", tmp_dir, strerror(errno));
    }

    return ret;
}
