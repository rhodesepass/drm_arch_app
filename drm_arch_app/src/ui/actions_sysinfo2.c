// 系统详细页 / 网络页 / shell页 适配层
#include "ui.h"
#include "screens.h"
#include "ui/actions_sysinfo.h"
#include "utils/log.h"
#include "utils/misc.h"
#include "config.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#define SYSINFO_DEVICE_TREE_MODEL_PATH "/proc/device-tree/model"
#define SYSINFO_DEVICE_TREE_COMPATIBLE_PATH "/proc/device-tree/compatible"
#define SYSINFO_CPUINFO_PATH "/proc/cpuinfo"

static void trim_line_end(char *buf) {
    size_t len;

    if(buf == NULL){
        return;
    }

    len = strlen(buf);
    while(len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                      buf[len - 1] == ' ' || buf[len - 1] == '\t')){
        buf[len - 1] = '\0';
        len--;
    }
}

static int read_text_file_trim(const char *path, char *buf, size_t buf_sz) {
    FILE *fp;
    size_t nread;

    if(path == NULL || buf == NULL || buf_sz == 0){
        return -1;
    }

    fp = fopen(path, "rb");
    if(fp == NULL){
        buf[0] = '\0';
        return -1;
    }

    nread = fread(buf, 1, buf_sz - 1, fp);
    fclose(fp);
    buf[nread] = '\0';
    trim_line_end(buf);
    return 0;
}

static int read_os_release_value(const char *key, char *buf, size_t buf_sz) {
    FILE *fp;
    char line[256];
    size_t key_len;

    if(key == NULL || buf == NULL || buf_sz == 0){
        return -1;
    }

    fp = fopen(SYSINFO_OSRELEASE_PATH, "r");
    if(fp == NULL){
        buf[0] = '\0';
        return -1;
    }

    key_len = strlen(key);
    while(fgets(line, sizeof(line), fp) != NULL){
        size_t value_len;

        if(strncmp(line, key, key_len) != 0 || line[key_len] != '='){
            continue;
        }

        safe_strcpy(buf, buf_sz, line + key_len + 1);
        trim_line_end(buf);

        value_len = strlen(buf);
        if(value_len >= 2 && buf[0] == '"' && buf[value_len - 1] == '"'){
            memmove(buf, buf + 1, value_len - 2);
            buf[value_len - 2] = '\0';
        }

        fclose(fp);
        return 0;
    }

    fclose(fp);
    buf[0] = '\0';
    return -1;
}

static int read_meminfo_kb(const char *field, uint64_t *value_kb) {
    FILE *fp;
    char line[256];
    char name[64];
    unsigned long long value;

    if(field == NULL || value_kb == NULL){
        return -1;
    }

    fp = fopen(SYSINFO_MEMINFO_PATH, "r");
    if(fp == NULL){
        return -1;
    }

    while(fgets(line, sizeof(line), fp) != NULL){
        if(sscanf(line, "%63[^:]: %llu kB", name, &value) == 2 && strcmp(name, field) == 0){
            fclose(fp);
            *value_kb = (uint64_t)value;
            return 0;
        }
    }

    fclose(fp);
    return -1;
}

static void format_bytes_short(uint64_t bytes, char *buf, size_t buf_sz) {
    double size;
    const char *unit;

    if(buf == NULL || buf_sz == 0){
        return;
    }

    size = (double)bytes;
    unit = "B";

    if(size >= 1024.0 * 1024.0 * 1024.0){
        size /= 1024.0 * 1024.0 * 1024.0;
        unit = "GB";
    } else if(size >= 1024.0 * 1024.0){
        size /= 1024.0 * 1024.0;
        unit = "MB";
    } else if(size >= 1024.0){
        size /= 1024.0;
        unit = "KB";
    }

    if(size >= 100.0){
        snprintf(buf, buf_sz, "%.0f%s", size, unit);
    } else if(size >= 10.0){
        snprintf(buf, buf_sz, "%.1f%s", size, unit);
    } else {
        snprintf(buf, buf_sz, "%.2f%s", size, unit);
    }
}

static void format_uptime_short(long uptime_sec, char *buf, size_t buf_sz) {
    long days;
    long hours;
    long minutes;

    if(buf == NULL || buf_sz == 0){
        return;
    }

    if(uptime_sec < 0){
        safe_strcpy(buf, buf_sz, "N/A");
        return;
    }

    days = uptime_sec / 86400;
    hours = (uptime_sec % 86400) / 3600;
    minutes = (uptime_sec % 3600) / 60;

    if(days > 0){
        snprintf(buf, buf_sz, "%ldd %ldh", days, hours);
    } else if(hours > 0){
        snprintf(buf, buf_sz, "%ldh %ldm", hours, minutes);
    } else {
        snprintf(buf, buf_sz, "%ldm", minutes);
    }
}

static int read_cpuinfo_field(const char *field, char *buf, size_t buf_sz) {
    FILE *fp;
    char line[256];
    size_t key_len;

    if(field == NULL || buf == NULL || buf_sz == 0){
        return -1;
    }

    fp = fopen(SYSINFO_CPUINFO_PATH, "r");
    if(fp == NULL){
        buf[0] = '\0';
        return -1;
    }

    key_len = strlen(field);
    while(fgets(line, sizeof(line), fp) != NULL){
        char *value;

        if(strncmp(line, field, key_len) != 0){
            continue;
        }

        value = strchr(line, ':');
        if(value == NULL){
            continue;
        }

        value++;
        while(*value == ' ' || *value == '\t'){
            value++;
        }

        safe_strcpy(buf, buf_sz, value);
        trim_line_end(buf);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    buf[0] = '\0';
    return -1;
}

static int read_compatible_token(char *buf, size_t buf_sz) {
    FILE *fp;
    char raw[256];
    size_t nread;
    size_t i;

    if(buf == NULL || buf_sz == 0){
        return -1;
    }

    fp = fopen(SYSINFO_DEVICE_TREE_COMPATIBLE_PATH, "rb");
    if(fp == NULL){
        buf[0] = '\0';
        return -1;
    }

    nread = fread(raw, 1, sizeof(raw) - 1, fp);
    fclose(fp);
    if(nread == 0){
        buf[0] = '\0';
        return -1;
    }

    raw[nread] = '\0';
    for(i = 0; i < nread; i++){
        if(raw[i] == '\0'){
            raw[i] = '\n';
        }
    }

    {
        char *token = strtok(raw, "\n");
        if(token == NULL){
            buf[0] = '\0';
            return -1;
        }

        if(strncmp(token, "allwinner,", 10) == 0){
            token += 10;
        }

        safe_strcpy(buf, buf_sz, token);
        return 0;
    }
}

static void clear_pressed_state(lv_event_t *e) {
    lv_obj_t *obj;

    if(e == NULL || lv_event_get_code(e) != LV_EVENT_PRESSED){
        return;
    }

    obj = lv_event_get_target(e);
    if(obj != NULL){
        lv_obj_remove_state(obj, LV_STATE_PRESSED);
    }
}

const char *get_var_ram_label(){
    static char buf[64];
    char used_str[24];
    char total_str[24];
    uint64_t mem_total_kb;
    uint64_t mem_available_kb;
    uint64_t mem_used_bytes;
    uint64_t mem_total_bytes;

    if(read_meminfo_kb("MemTotal", &mem_total_kb) != 0 ||
       read_meminfo_kb("MemAvailable", &mem_available_kb) != 0 ||
       mem_total_kb < mem_available_kb){
        return "N/A";
    }

    mem_total_bytes = mem_total_kb * 1024ULL;
    mem_used_bytes = (mem_total_kb - mem_available_kb) * 1024ULL;

    format_bytes_short(mem_used_bytes, used_str, sizeof(used_str));
    format_bytes_short(mem_total_bytes, total_str, sizeof(total_str));
    snprintf(buf, sizeof(buf), "%s/%s", used_str, total_str);
    return buf;
}

void set_var_ram_label(const char *value){
    (void)value;
}

int32_t get_var_ram_percent(){
    uint64_t mem_total_kb;
    uint64_t mem_available_kb;

    if(read_meminfo_kb("MemTotal", &mem_total_kb) != 0 ||
       read_meminfo_kb("MemAvailable", &mem_available_kb) != 0 ||
       mem_total_kb == 0 ||
       mem_total_kb < mem_available_kb){
        return 0;
    }

    return (int32_t)(((mem_total_kb - mem_available_kb) * 100ULL) / mem_total_kb);
}

void set_var_ram_percent(int32_t value){
    (void)value;
}

const char *get_var_os_detail(){
    static char buf[128];

    if(read_os_release_value("PRETTY_NAME", buf, sizeof(buf)) == 0){
        return buf;
    }
    if(read_os_release_value("NAME", buf, sizeof(buf)) == 0){
        return buf;
    }
    return "Unknown";
}

void set_var_os_detail(const char *value){
    (void)value;
}

const char *get_var_ker_detail(){
    static char buf[128];
    struct utsname uts;

    if(uname(&uts) == 0){
        snprintf(buf, sizeof(buf), "%s", uts.release);
        return buf;
    }

    return "Unknown";
}

void set_var_ker_detail(const char *value){
    (void)value;
}

const char *get_var_board_detail(){
    static char buf[128];

    if(read_text_file_trim(SYSINFO_DEVICE_TREE_MODEL_PATH, buf, sizeof(buf)) == 0 && buf[0] != '\0'){
        return buf;
    }

    return "ArkEPass";
}

void set_var_board_detail(const char *value){
    (void)value;
}

const char *get_var_soc_detail(){
    static char buf[128];

    if(read_compatible_token(buf, sizeof(buf)) == 0 && buf[0] != '\0'){
        return buf;
    }
    if(read_cpuinfo_field("Hardware", buf, sizeof(buf)) == 0 && buf[0] != '\0'){
        return buf;
    }
    if(read_cpuinfo_field("system type", buf, sizeof(buf)) == 0 && buf[0] != '\0'){
        return buf;
    }

    return "Allwinner F1C200S";
}

void set_var_soc_detail(const char *value){
    (void)value;
}

const char *get_var_ram_detail(){
    static char buf[64];
    uint64_t mem_total_kb;

    if(read_meminfo_kb("MemTotal", &mem_total_kb) != 0){
        return "Unknown";
    }

    format_bytes_short(mem_total_kb * 1024ULL, buf, sizeof(buf));
    return buf;
}

void set_var_ram_detail(const char *value){
    (void)value;
}

const char *get_var_ip_detail(){
    static char buf[INET_ADDRSTRLEN];
    struct ifaddrs *ifaddr;
    struct ifaddrs *ifa;
    int best_score;

    if(getifaddrs(&ifaddr) != 0){
        return "offline";
    }

    best_score = -1;
    buf[0] = '\0';

    for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next){
        struct sockaddr_in *sa;
        int score;
        char addr[INET_ADDRSTRLEN];

        if(ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET){
            continue;
        }
        if((ifa->ifa_flags & IFF_LOOPBACK) != 0){
            continue;
        }

        sa = (struct sockaddr_in *)ifa->ifa_addr;
        if(inet_ntop(AF_INET, &sa->sin_addr, addr, sizeof(addr)) == NULL){
            continue;
        }

        score = 10;
        if(strcmp(ifa->ifa_name, "usb0") == 0){
            score = 100;
        } else if(strcmp(ifa->ifa_name, "rndis0") == 0){
            score = 90;
        } else if(strcmp(ifa->ifa_name, "eth0") == 0){
            score = 80;
        } else if(strcmp(ifa->ifa_name, "wlan0") == 0){
            score = 70;
        }

        if(score > best_score){
            best_score = score;
            safe_strcpy(buf, sizeof(buf), addr);
        }
    }

    freeifaddrs(ifaddr);

    if(best_score < 0){
        return "offline";
    }

    return buf;
}

void set_var_ip_detail(const char *value){
    (void)value;
}

const char *get_var_uptime_detail(){
    static char buf[32];
    struct sysinfo info;

    if(sysinfo(&info) != 0){
        return "Unknown";
    }

    format_uptime_short(info.uptime, buf, sizeof(buf));
    return buf;
}

void set_var_uptime_detail(const char *value){
    (void)value;
}

const char *get_var_codename_detail(){
    return APP_SUBCODENAME;
}

void set_var_codename_detail(const char *value){
    (void)value;
}

void action_show_sys(lv_event_t * e){
    log_debug("action_show_sys");
    clear_pressed_state(e);
    loadScreen(SCREEN_ID_SYSINFO2);
}

void action_show_net(lv_event_t * e){
    lv_event_code_t event;

    event = lv_event_get_code(e);
    if(event != LV_EVENT_PRESSED){
        return;
    }

    log_debug("action_show_net");
    clear_pressed_state(e);
    loadScreen(SCREEN_ID_NET);
}

void action_show_shell(lv_event_t * e){
    lv_event_code_t event;

    event = lv_event_get_code(e);
    if(event != LV_EVENT_PRESSED){
        return;
    }

    log_debug("action_show_shell");
    clear_pressed_state(e);
    loadScreen(SCREEN_ID_SHELL);
}
