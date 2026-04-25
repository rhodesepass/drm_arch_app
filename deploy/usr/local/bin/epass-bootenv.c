#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_ENV_PATH "/boot/epass/boot.env"

struct boot_env {
    char device_rev[32];
    char screen[32];
    char epass_firstboot[8];
    char epass_resize_pending[8];
    char epass_resize_stage[32];
    char epass_boot_mode[32];
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-f path] show <key>\n"
            "       %s [-f path] set <key=value> [key=value...]\n",
            prog, prog);
}

static bool copy_value(char *dst, size_t size, const char *src)
{
    size_t len = strlen(src);
    if (len == 0 || len >= size)
        return false;
    memcpy(dst, src, len + 1);
    return true;
}

static void init_defaults(struct boot_env *env)
{
    copy_value(env->device_rev, sizeof(env->device_rev), "0.6");
    copy_value(env->screen, sizeof(env->screen), "laowu");
    copy_value(env->epass_firstboot, sizeof(env->epass_firstboot), "1");
    copy_value(env->epass_resize_pending, sizeof(env->epass_resize_pending), "0");
    copy_value(env->epass_resize_stage, sizeof(env->epass_resize_stage), "done");
    copy_value(env->epass_boot_mode, sizeof(env->epass_boot_mode), "normal");
}

static bool valid_token(const char *value)
{
    if (!value || !*value)
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '.' || *p == '_' || *p == '-') {
            continue;
        }
        return false;
    }
    return true;
}

static bool set_field(struct boot_env *env, const char *key, const char *value)
{
    if (strcmp(key, "device_rev") == 0)
        return valid_token(value) && copy_value(env->device_rev, sizeof(env->device_rev), value);
    if (strcmp(key, "screen") == 0)
        return valid_token(value) && copy_value(env->screen, sizeof(env->screen), value);
    if (strcmp(key, "epass_firstboot") == 0)
        return (strcmp(value, "0") == 0 || strcmp(value, "1") == 0) &&
               copy_value(env->epass_firstboot, sizeof(env->epass_firstboot), value);
    if (strcmp(key, "epass_resize_pending") == 0)
        return (strcmp(value, "0") == 0 || strcmp(value, "1") == 0) &&
               copy_value(env->epass_resize_pending, sizeof(env->epass_resize_pending), value);
    if (strcmp(key, "epass_resize_stage") == 0)
        return (strcmp(value, "partition") == 0 ||
                strcmp(value, "fs") == 0 ||
                strcmp(value, "done") == 0 ||
                strcmp(value, "error") == 0) &&
               copy_value(env->epass_resize_stage, sizeof(env->epass_resize_stage), value);
    if (strcmp(key, "epass_boot_mode") == 0)
        return (strcmp(value, "normal") == 0 || strcmp(value, "resize") == 0) &&
               copy_value(env->epass_boot_mode, sizeof(env->epass_boot_mode), value);
    return false;
}

static const char *get_field(const struct boot_env *env, const char *key)
{
    if (strcmp(key, "device_rev") == 0)
        return env->device_rev;
    if (strcmp(key, "screen") == 0)
        return env->screen;
    if (strcmp(key, "epass_firstboot") == 0)
        return env->epass_firstboot;
    if (strcmp(key, "epass_resize_pending") == 0)
        return env->epass_resize_pending;
    if (strcmp(key, "epass_resize_stage") == 0)
        return env->epass_resize_stage;
    if (strcmp(key, "epass_boot_mode") == 0)
        return env->epass_boot_mode;
    return NULL;
}

static int sync_parent_dir(const char *path)
{
    char dirbuf[256];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(dirbuf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dirbuf, path, len + 1);

    char *slash = strrchr(dirbuf, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    if (slash == dirbuf)
        slash[1] = '\0';
    else
        *slash = '\0';

    int dirfd = open(dirbuf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0)
        return -1;
    int rc = fsync(dirfd);
    int saved = errno;
    close(dirfd);
    errno = saved;
    return rc;
}

static int load_env(const char *path, struct boot_env *env)
{
    init_defaults(env);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');
        if (!eq) {
            fclose(fp);
            errno = EINVAL;
            return -1;
        }
        *eq = '\0';
        if (!set_field(env, line, eq + 1)) {
            fclose(fp);
            errno = EINVAL;
            return -1;
        }
    }

    if (ferror(fp)) {
        int saved = errno;
        fclose(fp);
        errno = saved;
        return -1;
    }
    fclose(fp);
    return 0;
}

static int write_env(const char *path, const struct boot_env *env)
{
    char dirbuf[256];
    char tmp[320];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(dirbuf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dirbuf, path, len + 1);

    char *slash = strrchr(dirbuf, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    if (slash == dirbuf)
        slash[1] = '\0';
    else
        *slash = '\0';

    if (snprintf(tmp, sizeof(tmp), "%s/.tmpXXXXXX", dirbuf) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    if (fchmod(fd, 0644) < 0) {
        int saved = errno;
        close(fd);
        unlink(tmp);
        errno = saved;
        return -1;
    }

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        int saved = errno;
        close(fd);
        unlink(tmp);
        errno = saved;
        return -1;
    }

    fprintf(fp,
            "device_rev=%s\n"
            "screen=%s\n"
            "epass_firstboot=%s\n"
            "epass_resize_pending=%s\n"
            "epass_resize_stage=%s\n"
            "epass_boot_mode=%s\n",
            env->device_rev,
            env->screen,
            env->epass_firstboot,
            env->epass_resize_pending,
            env->epass_resize_stage,
            env->epass_boot_mode);

    if (fflush(fp) != 0 || fsync(fd) < 0 || fclose(fp) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }

    if (rename(tmp, path) < 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    return sync_parent_dir(path);
}

int main(int argc, char **argv)
{
    const char *path = DEFAULT_ENV_PATH;
    int argi = 1;

    if (argc > 3 && strcmp(argv[1], "-f") == 0) {
        path = argv[2];
        argi = 3;
    }
    if (argc <= argi) {
        usage(argv[0]);
        return 64;
    }

    const char *cmd = argv[argi++];
    struct boot_env env;

    if (strcmp(cmd, "show") == 0) {
        if (argc != argi + 1) {
            usage(argv[0]);
            return 64;
        }
        if (load_env(path, &env) < 0) {
            perror("load_env");
            return 1;
        }
        const char *value = get_field(&env, argv[argi]);
        if (!value) {
            fprintf(stderr, "unknown key: %s\n", argv[argi]);
            return 64;
        }
        printf("%s\n", value);
        return 0;
    }

    if (strcmp(cmd, "set") == 0) {
        if (argc <= argi) {
            usage(argv[0]);
            return 64;
        }
        if (load_env(path, &env) < 0) {
            perror("load_env");
            return 1;
        }
        for (; argi < argc; argi++) {
            char *eq = strchr(argv[argi], '=');
            if (!eq) {
                fprintf(stderr, "invalid assignment: %s\n", argv[argi]);
                return 64;
            }
            *eq = '\0';
            if (!set_field(&env, argv[argi], eq + 1)) {
                fprintf(stderr, "invalid key/value: %s=%s\n", argv[argi], eq + 1);
                return 64;
            }
            *eq = '=';
        }
        if (write_env(path, &env) < 0) {
            perror("write_env");
            return 1;
        }
        return 0;
    }

    usage(argv[0]);
    return 64;
}
