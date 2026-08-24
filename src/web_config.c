/*
 * Luckfox Pico Pro Max (RV1106) - Production-safe read-only status monitor
 *
 * Design goals:
 *   - Never modify /userdata/rkipc.ini.
 *   - Never restart/kill rkipc from the web tier.
 *   - Avoid per-second directory scans and shell-outs.
 *   - Monitor recordings via inotify and cache all status.
 *   - Expose only GET endpoints: /, /api/status, /api/logs, /healthz.
 *   - Log only state changes and errors to reduce SD-card writes.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define WEB_PORT 8080
#define RTSP_PORT 554
#define CONFIG_FILE "/userdata/rkipc.ini"
#define SD_MOUNT_PATH "/mnt/sdcard"
#define RECORDING_PATH "/mnt/sdcard/recordings"
#define LOG_FILE "/mnt/sdcard/web_status.log"
#define FALLBACK_LOG_FILE "/tmp/web_status.log"

#define AUTH_BASE64 "YWRtaW46bHVja2ZveA==" /* admin:luckfox */
#define MAX_LOG_SIZE (2 * 1024 * 1024)
#define HTTP_REQUEST_MAX 16384
#define HTTP_BODY_MAX (96 * 1024)
#define LOG_READ_MAX (48 * 1024)
#define STATUS_INTERVAL_SEC 5
#define RECORDING_TIMEOUT_SEC 300
#define INOTIFY_RETRY_SEC 5

typedef struct {
    int rtsp_running;
    int recording_active;
    int sd_status;          /* 0 = missing/unmounted, 1 = mounted RO, 2 = mounted RW */
    int snapshot_enabled;
    uint64_t video_count;
    uint64_t sd_total_bytes;
    uint64_t sd_used_bytes;
    uint64_t sd_free_bytes;
    uint64_t last_segment_age_sec;
    char uptime[64];
    char memory[64];
    char storage[96];
    char current_time[64];
} camera_status_t;

static volatile sig_atomic_t server_running = 1;
static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static camera_status_t g_status;
static double g_last_segment_mono = 0.0;
static uint64_t g_video_count = 0;
static FILE *log_fp = NULL;
static int log_using_fallback = 0;

/* GPIO1 base for the three board LEDs used by the original firmware. */
#define GPIO1_BASE_PHY 0xFF530000UL
#define GPIO_MAP_SIZE 4096UL
#define GPIO_MAP_MASK (GPIO_MAP_SIZE - 1)
#define GPIO_SWPORT_DR_H 0x0004
#define GPIO_SWPORT_DDR_H 0x000C
#define LED1_BIT 5 /* recording */
#define LED2_BIT 6 /* SD */
#define LED3_BIT 7 /* RTSP */
static volatile uint32_t *gpio_base = NULL;
static void *gpio_mapping = NULL;

static double monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void trim_string(char *s) {
    char *start;
    char *end;

    if (!s || !*s) return;

    start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    if (!*s) return;
    end = s + strlen(s) - 1;
    while (end >= s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
}

static int is_video_name(const char *name) {
    const char *dot;
    if (!name || name[0] == '.') return 0;
    dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".ts") == 0 ||
           strcasecmp(dot, ".mp4") == 0 ||
           strcasecmp(dot, ".mkv") == 0 ||
           strcasecmp(dot, ".h264") == 0 ||
           strcasecmp(dot, ".h265") == 0;
}

static int get_sd_mount_status(void);

static void log_open_locked(void) {
    if (log_fp && log_using_fallback && get_sd_mount_status() != 0) {
        FILE *persistent;
        fflush(log_fp);
        persistent = fopen(LOG_FILE, "a");
        if (persistent) {
            fclose(log_fp);
            log_fp = persistent;
            log_using_fallback = 0;
        }
    }

    if (log_fp) return;

    if (get_sd_mount_status() != 0) {
        log_fp = fopen(LOG_FILE, "a");
        if (log_fp) log_using_fallback = 0;
    }
    if (!log_fp) {
        log_fp = fopen(FALLBACK_LOG_FILE, "a");
        if (log_fp) log_using_fallback = 1;
    }
}

static void log_msg(const char *level, const char *fmt, ...) {
    struct stat st;
    time_t now;
    struct tm tm_now;
    char timestamp[64];
    va_list ap;

    pthread_mutex_lock(&log_mutex);
    log_open_locked();
    if (!log_fp) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    if (fstat(fileno(log_fp), &st) == 0 && st.st_size > MAX_LOG_SIZE) {
        int was_fallback = log_using_fallback;
        fclose(log_fp);
        log_fp = NULL;

        if (was_fallback) {
            rename(FALLBACK_LOG_FILE, FALLBACK_LOG_FILE ".old");
        } else {
            rename(LOG_FILE, LOG_FILE ".old");
        }
        log_open_locked();
        if (!log_fp) {
            pthread_mutex_unlock(&log_mutex);
            return;
        }
    }

    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    fprintf(log_fp, "[%s] [%s] ", timestamp, level ? level : "INFO");
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', log_fp);
    fflush(log_fp);
    pthread_mutex_unlock(&log_mutex);
}

static void signal_handler(int sig) {
    (void)sig;
    server_running = 0;
}

static int read_ini_int(const char *section, const char *key, int default_value) {
    FILE *fp;
    char line[512];
    char current_section[128] = "";
    int in_section = 0;

    fp = fopen(CONFIG_FILE, "r");
    if (!fp) return default_value;

    while (fgets(line, sizeof(line), fp)) {
        char *eq;
        char file_key[128];
        char file_value[256];

        trim_string(line);
        if (!*line || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[') {
            if (sscanf(line, "[%127[^]]]", current_section) == 1) {
                trim_string(current_section);
                in_section = strcmp(current_section, section) == 0;
            } else {
                in_section = 0;
            }
            continue;
        }

        if (!in_section) continue;
        eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        strncpy(file_key, line, sizeof(file_key) - 1);
        file_key[sizeof(file_key) - 1] = '\0';
        strncpy(file_value, eq + 1, sizeof(file_value) - 1);
        file_value[sizeof(file_value) - 1] = '\0';
        trim_string(file_key);
        trim_string(file_value);

        if (strcmp(file_key, key) == 0) {
            int value = atoi(file_value);
            fclose(fp);
            return value;
        }
    }

    fclose(fp);
    return default_value;
}

static int mount_option_present(const char *options, const char *wanted) {
    const char *p = options;
    size_t wanted_len = strlen(wanted);

    while (p && *p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == wanted_len && strncmp(p, wanted, wanted_len) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

static int get_sd_mount_status(void) {
    FILE *fp;
    char device[256], mountpoint[256], fstype[64], options[512];
    int dump_freq, pass_no;
    int status = 0;

    fp = fopen("/proc/mounts", "r");
    if (!fp) return 0;

    while (fscanf(fp, "%255s %255s %63s %511s %d %d\n",
                  device, mountpoint, fstype, options, &dump_freq, &pass_no) == 6) {
        if (strcmp(mountpoint, SD_MOUNT_PATH) == 0) {
            if (mount_option_present(options, "rw")) status = 2;
            else status = 1;
            break;
        }
    }

    fclose(fp);
    return status;
}

static void human_bytes(uint64_t bytes, char *out, size_t out_size) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    int unit = 0;

    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    snprintf(out, out_size, "%.1f %s", value, units[unit]);
}

static void get_storage_stats(int sd_status,
                              uint64_t *total,
                              uint64_t *used,
                              uint64_t *free_bytes,
                              char *display,
                              size_t display_size) {
    struct statvfs vfs;
    char used_text[32], total_text[32];

    *total = *used = *free_bytes = 0;
    snprintf(display, display_size, "unmounted");

    if (sd_status == 0) return;
    if (statvfs(SD_MOUNT_PATH, &vfs) != 0) {
        snprintf(display, display_size, "unknown");
        return;
    }

    *total = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
    *free_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    *used = *total >= *free_bytes ? *total - *free_bytes : 0;

    human_bytes(*used, used_text, sizeof(used_text));
    human_bytes(*total, total_text, sizeof(total_text));
    snprintf(display, display_size, "%s / %s", used_text, total_text);
}

static void get_uptime(char *buffer, size_t size) {
    struct sysinfo info;
    long uptime;
    int days, hours, minutes;

    if (sysinfo(&info) != 0) {
        snprintf(buffer, size, "unknown");
        return;
    }

    uptime = info.uptime;
    days = (int)(uptime / 86400);
    hours = (int)((uptime % 86400) / 3600);
    minutes = (int)((uptime % 3600) / 60);

    if (days > 0) snprintf(buffer, size, "%dd %dh %dm", days, hours, minutes);
    else if (hours > 0) snprintf(buffer, size, "%dh %dm", hours, minutes);
    else snprintf(buffer, size, "%dm", minutes);
}

static void get_memory(char *buffer, size_t size) {
    FILE *fp;
    char line[256];
    unsigned long total_kb = 0;
    unsigned long available_kb = 0;

    fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        snprintf(buffer, size, "unknown");
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu kB", &total_kb) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu kB", &available_kb) == 1) break;
    }
    fclose(fp);

    if (total_kb == 0) {
        snprintf(buffer, size, "unknown");
        return;
    }

    {
        unsigned long used_kb = total_kb > available_kb ? total_kb - available_kb : 0;
        unsigned long total_mb = total_kb / 1024;
        unsigned long used_mb = used_kb / 1024;
        unsigned int percent = total_kb ? (unsigned int)((used_kb * 100UL) / total_kb) : 0;
        snprintf(buffer, size, "%luM / %luM (%u%%)", used_mb, total_mb, percent);
    }
}

static void get_current_time(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static int probe_tcp_port_local(int port) {
    int fd;
    int flags;
    int result = 0;
    struct sockaddr_in addr;
    fd_set wfds;
    struct timeval tv;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        result = 1;
    } else if (errno == EINPROGRESS) {
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = 0;
        tv.tv_usec = 300000;
        if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0 &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0 &&
            so_error == 0) {
            result = 1;
        }
    }

    close(fd);
    return result;
}

static void initial_recording_scan(void) {
    DIR *dir;
    struct dirent *entry;
    uint64_t count = 0;
    time_t newest_mtime = 0;
    time_t now_wall = time(NULL);
    double now_mono = monotonic_seconds();

    dir = opendir(RECORDING_PATH);
    if (!dir) {
        pthread_mutex_lock(&status_mutex);
        g_video_count = 0;
        g_last_segment_mono = 0.0;
        pthread_mutex_unlock(&status_mutex);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[768];
        struct stat st;

        if (!is_video_name(entry->d_name)) continue;
        snprintf(path, sizeof(path), "%s/%s", RECORDING_PATH, entry->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            count++;
            if (st.st_mtime > newest_mtime) newest_mtime = st.st_mtime;
        }
    }
    closedir(dir);

    pthread_mutex_lock(&status_mutex);
    g_video_count = count;
    if (newest_mtime > 0) {
        double age = difftime(now_wall, newest_mtime);
        if (age < 0.0) age = 0.0;
        g_last_segment_mono = now_mono - age;
        if (g_last_segment_mono < 0.0) g_last_segment_mono = 0.0;
    } else {
        g_last_segment_mono = 0.0;
    }
    pthread_mutex_unlock(&status_mutex);
}

static void *recording_watch_thread(void *arg) {
    int fd = -1;
    int wd = -1;
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    (void)arg;

    while (server_running) {
        if (fd < 0) {
            fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (fd < 0) {
                log_msg("ERROR", "inotify_init1 failed: %s", strerror(errno));
                sleep(INOTIFY_RETRY_SEC);
                continue;
            }
        }

        if (wd < 0) {
            initial_recording_scan();
            wd = inotify_add_watch(fd, RECORDING_PATH,
                                   IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                   IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF |
                                   IN_UNMOUNT | IN_IGNORED);
            if (wd < 0) {
                close(fd);
                fd = -1;
                sleep(INOTIFY_RETRY_SEC);
                continue;
            }
            log_msg("INFO", "Recording watcher attached to %s", RECORDING_PATH);
        }

        {
            ssize_t len = read(fd, buffer, sizeof(buffer));
            if (len < 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    log_msg("WARN", "inotify read failed: %s", strerror(errno));
                    inotify_rm_watch(fd, wd);
                    wd = -1;
                    close(fd);
                    fd = -1;
                } else {
                    usleep(250000);
                }
                continue;
            }

            if (len == 0) {
                usleep(250000);
                continue;
            }

            {
                char *ptr = buffer;
                while (ptr < buffer + len) {
                    struct inotify_event *ev = (struct inotify_event *)ptr;
                    int video = ev->len > 0 && is_video_name(ev->name);

                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED)) {
                        wd = -1;
                    }

                    if (video) {
                        pthread_mutex_lock(&status_mutex);
                        if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
                            g_video_count++;
                        }
                        if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                            if (g_video_count > 0) g_video_count--;
                        }
                        if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) {
                            g_last_segment_mono = monotonic_seconds();
                        }
                        pthread_mutex_unlock(&status_mutex);
                    }

                    ptr += sizeof(struct inotify_event) + ev->len;
                }
            }
        }

        if (wd < 0) {
            close(fd);
            fd = -1;
            sleep(INOTIFY_RETRY_SEC);
        }
    }

    if (wd >= 0 && fd >= 0) inotify_rm_watch(fd, wd);
    if (fd >= 0) close(fd);
    return NULL;
}

static void log_transition(const char *name, int old_value, int new_value) {
    if (old_value == new_value) return;
    log_msg(new_value ? "INFO" : "WARN", "%s changed: %d -> %d", name, old_value, new_value);
}

static void *status_thread(void *arg) {
    camera_status_t previous;
    int have_previous = 0;
    (void)arg;
    memset(&previous, 0, sizeof(previous));

    while (server_running) {
        camera_status_t next;
        double now_mono;
        double last_segment;
        uint64_t count;

        memset(&next, 0, sizeof(next));
        next.rtsp_running = probe_tcp_port_local(RTSP_PORT);
        next.sd_status = get_sd_mount_status();
        next.snapshot_enabled = read_ini_int("video.jpeg", "enable_cycle_snapshot", 0);

        get_uptime(next.uptime, sizeof(next.uptime));
        get_memory(next.memory, sizeof(next.memory));
        get_current_time(next.current_time, sizeof(next.current_time));
        get_storage_stats(next.sd_status,
                          &next.sd_total_bytes,
                          &next.sd_used_bytes,
                          &next.sd_free_bytes,
                          next.storage,
                          sizeof(next.storage));

        pthread_mutex_lock(&status_mutex);
        last_segment = g_last_segment_mono;
        count = g_video_count;
        pthread_mutex_unlock(&status_mutex);

        next.video_count = count;
        now_mono = monotonic_seconds();
        if (last_segment > 0.0 && now_mono >= last_segment) {
            double age = now_mono - last_segment;
            next.last_segment_age_sec = (uint64_t)age;
            next.recording_active =
                next.rtsp_running &&
                next.sd_status == 2 &&
                age <= (double)RECORDING_TIMEOUT_SEC;
        } else {
            next.last_segment_age_sec = 0;
            next.recording_active = 0;
        }

        if (have_previous) {
            log_transition("RTSP", previous.rtsp_running, next.rtsp_running);
            log_transition("Recording", previous.recording_active, next.recording_active);
            log_transition("SD status", previous.sd_status, next.sd_status);
        }

        pthread_mutex_lock(&status_mutex);
        g_status = next;
        pthread_mutex_unlock(&status_mutex);

        previous = next;
        have_previous = 1;

        for (int i = 0; i < STATUS_INTERVAL_SEC * 10 && server_running; ++i) {
            usleep(100000);
        }
    }

    return NULL;
}

static camera_status_t status_snapshot(void) {
    camera_status_t copy;
    pthread_mutex_lock(&status_mutex);
    copy = g_status;
    pthread_mutex_unlock(&status_mutex);
    return copy;
}

static int gpio_setup(void) {
    int mem_fd;

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        log_msg("WARN", "LED GPIO disabled: cannot open /dev/mem");
        return 0;
    }

    gpio_mapping = mmap(NULL, GPIO_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                        mem_fd, GPIO1_BASE_PHY & ~GPIO_MAP_MASK);
    close(mem_fd);

    if (gpio_mapping == MAP_FAILED) {
        gpio_mapping = NULL;
        log_msg("WARN", "LED GPIO disabled: mmap failed");
        return 0;
    }

    gpio_base = (volatile uint32_t *)((char *)gpio_mapping +
                                     (GPIO1_BASE_PHY & GPIO_MAP_MASK));
    {
        uint32_t val = 0;
        val |= (1U << (LED1_BIT + 16)) | (1U << LED1_BIT);
        val |= (1U << (LED2_BIT + 16)) | (1U << LED2_BIT);
        val |= (1U << (LED3_BIT + 16)) | (1U << LED3_BIT);
        *(gpio_base + GPIO_SWPORT_DDR_H / 4) = val;
    }

    return 1;
}

static void set_led(int bit, int state) {
    uint32_t val;
    if (!gpio_base) return;
    val = (1U << (bit + 16)) | ((state ? 1U : 0U) << bit);
    *(gpio_base + GPIO_SWPORT_DR_H / 4) = val;
}

static void *led_thread(void *arg) {
    int blink = 0;
    (void)arg;

    if (!gpio_setup()) return NULL;

    while (server_running) {
        camera_status_t s = status_snapshot();

        set_led(LED1_BIT, s.recording_active);
        set_led(LED3_BIT, s.rtsp_running);

        if (s.sd_status == 2) set_led(LED2_BIT, 1);
        else if (s.sd_status == 1) {
            set_led(LED2_BIT, blink);
            blink = !blink;
        } else set_led(LED2_BIT, 0);

        sleep(1);
    }

    set_led(LED1_BIT, 0);
    set_led(LED2_BIT, 0);
    set_led(LED3_BIT, 0);

    if (gpio_mapping) {
        munmap(gpio_mapping, GPIO_MAP_SIZE);
        gpio_mapping = NULL;
        gpio_base = NULL;
    }
    return NULL;
}

static int send_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return 0;
    }
    return 1;
}

static int send_response(int fd,
                         int code,
                         const char *reason,
                         const char *content_type,
                         const char *body,
                         const char *extra_headers) {
    char header[1024];
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "X-Frame-Options: DENY\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     code,
                     reason,
                     content_type ? content_type : "text/plain; charset=utf-8",
                     body_len,
                     extra_headers ? extra_headers : "");
    if (n <= 0 || (size_t)n >= sizeof(header)) return 0;
    if (!send_all(fd, header, (size_t)n)) return 0;
    return body_len == 0 || send_all(fd, body, body_len);
}

static const char *find_header_value(const char *request, const char *name) {
    const char *line = strstr(request, "\r\n");
    size_t name_len = strlen(name);

    if (!line) return NULL;
    line += 2;

    while (*line) {
        const char *end = strstr(line, "\r\n");
        if (!end || end == line) break;

        if ((size_t)(end - line) > name_len + 1 &&
            strncasecmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *value = line + name_len + 1;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            return value;
        }
        line = end + 2;
    }
    return NULL;
}

static int check_auth(const char *request) {
    const char *value = find_header_value(request, "Authorization");
    static const char prefix[] = "Basic ";
    size_t token_len = strlen(AUTH_BASE64);

    if (!value) return 0;
    if (strncasecmp(value, prefix, sizeof(prefix) - 1) != 0) return 0;
    value += sizeof(prefix) - 1;

    /* Exact bounded comparison: no copy, no attacker-controlled stack write. */
    if (strncmp(value, AUTH_BASE64, token_len) != 0) return 0;
    value += token_len;
    return *value == '\r' || *value == '\n' || *value == ' ' || *value == '\t';
}

static ssize_t read_http_request(int fd, char *buffer, size_t capacity) {
    size_t used = 0;

    while (used + 1 < capacity) {
        ssize_t n = recv(fd, buffer + used, capacity - used - 1, 0);
        if (n > 0) {
            used += (size_t)n;
            buffer[used] = '\0';
            if (strstr(buffer, "\r\n\r\n")) return (ssize_t)used;
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        return -1;
    }

    if (used < capacity) buffer[used] = '\0';
    return (ssize_t)used;
}

static size_t json_escape_append(char *out, size_t pos, size_t cap, const char *input) {
    const unsigned char *p = (const unsigned char *)input;

    while (*p && pos + 2 < cap) {
        unsigned char c = *p++;
        const char *esc = NULL;

        switch (c) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b"; break;
            case '\f': esc = "\\f"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default: break;
        }

        if (esc) {
            size_t n = strlen(esc);
            if (pos + n >= cap) break;
            memcpy(out + pos, esc, n);
            pos += n;
        } else if (c < 0x20) {
            if (pos + 6 >= cap) break;
            pos += (size_t)snprintf(out + pos, cap - pos, "\\u%04x", c);
        } else {
            out[pos++] = (char)c;
        }
    }
    if (pos < cap) out[pos] = '\0';
    return pos;
}

static void send_status_json(int fd) {
    camera_status_t s = status_snapshot();
    char json[4096];

    snprintf(json, sizeof(json),
             "{"
             "\"rtsp_running\":%d,"
             "\"recording_active\":%d,"
             "\"recording_enabled\":%d,"
             "\"sd_status\":%d,"
             "\"snapshot_enabled\":%d,"
             "\"video_count\":%llu,"
             "\"last_segment_age_sec\":%llu,"
             "\"sd_total_bytes\":%llu,"
             "\"sd_used_bytes\":%llu,"
             "\"sd_free_bytes\":%llu,"
             "\"uptime\":\"%s\","
             "\"memory\":\"%s\","
             "\"storage\":\"%s\","
             "\"time\":\"%s\""
             "}",
             s.rtsp_running,
             s.recording_active,
             s.recording_active,
             s.sd_status,
             s.snapshot_enabled,
             (unsigned long long)s.video_count,
             (unsigned long long)s.last_segment_age_sec,
             (unsigned long long)s.sd_total_bytes,
             (unsigned long long)s.sd_used_bytes,
             (unsigned long long)s.sd_free_bytes,
             s.uptime,
             s.memory,
             s.storage,
             s.current_time);

    send_response(fd, 200, "OK", "application/json; charset=utf-8", json, NULL);
}

static void send_logs_json(int fd) {
    FILE *fp;
    long size;
    long start = 0;
    char input[LOG_READ_MAX + 1];
    size_t read_len;
    char *body;
    size_t cap = HTTP_BODY_MAX;
    size_t pos = 0;
    char *line;
    char *saveptr = NULL;

    pthread_mutex_lock(&log_mutex);
    if (log_fp) fflush(log_fp);

    fp = fopen(LOG_FILE, "r");
    if (!fp) fp = fopen(FALLBACK_LOG_FILE, "r");
    if (!fp) {
        pthread_mutex_unlock(&log_mutex);
        send_response(fd, 200, "OK", "application/json; charset=utf-8", "[]", NULL);
        return;
    }

    if (fseek(fp, 0, SEEK_END) == 0) {
        size = ftell(fp);
        if (size > LOG_READ_MAX) start = size - LOG_READ_MAX;
        if (fseek(fp, start, SEEK_SET) != 0) start = 0;
    }

    read_len = fread(input, 1, LOG_READ_MAX, fp);
    fclose(fp);
    pthread_mutex_unlock(&log_mutex);
    input[read_len] = '\0';

    if (start > 0) {
        char *nl = strchr(input, '\n');
        if (nl) memmove(input, nl + 1, strlen(nl + 1) + 1);
    }

    body = (char *)malloc(cap);
    if (!body) {
        send_response(fd, 500, "Internal Server Error",
                      "application/json; charset=utf-8",
                      "{\"error\":\"out of memory\"}", NULL);
        return;
    }

    body[pos++] = '[';
    body[pos] = '\0';

    line = strtok_r(input, "\n", &saveptr);
    while (line && pos + 4 < cap) {
        if (pos > 1) body[pos++] = ',';
        body[pos++] = '"';
        body[pos] = '\0';
        pos = json_escape_append(body, pos, cap, line);
        if (pos + 2 >= cap) break;
        body[pos++] = '"';
        body[pos] = '\0';
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (pos + 2 < cap) {
        body[pos++] = ']';
        body[pos] = '\0';
    } else {
        body[cap - 2] = ']';
        body[cap - 1] = '\0';
    }

    send_response(fd, 200, "OK", "application/json; charset=utf-8", body, NULL);
    free(body);
}

static const char DASHBOARD_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Luckfox Camera Status</title>"
"<style>"
":root{color-scheme:dark;--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#e6edf3;"
"--dim:#8b949e;--ok:#3fb950;--warn:#d29922;--bad:#f85149}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);"
"font:14px system-ui,sans-serif}.wrap{max-width:1000px;margin:auto;padding:24px}"
"h1{font-size:24px;margin:0 0 18px}.grid{display:grid;grid-template-columns:"
"repeat(auto-fit,minmax(280px,1fr));gap:14px}.card{background:var(--card);"
"border:1px solid var(--border);border-radius:10px;padding:16px}.row{display:flex;"
"justify-content:space-between;gap:16px;padding:9px 0;border-bottom:1px solid var(--border)}"
".row:last-child{border:0}.label{color:var(--dim)}.dot{display:inline-block;width:10px;"
"height:10px;border-radius:50%;margin-right:7px}.ok{background:var(--ok)}"
".warn{background:var(--warn)}.bad{background:var(--bad)}pre{white-space:pre-wrap;"
"word-break:break-word;max-height:360px;overflow:auto;background:#010409;padding:12px;"
"border-radius:8px;border:1px solid var(--border)}button{background:#21262d;color:var(--text);"
"border:1px solid var(--border);border-radius:7px;padding:8px 12px;cursor:pointer}"
".note{color:var(--dim);margin:10px 0 20px}</style></head><body><div class='wrap'>"
"<h1>Luckfox RV1106 Camera Status</h1>"
"<div class='note'>Read-only monitor. Camera configuration and rkipc lifecycle are not exposed over HTTP.</div>"
"<div class='grid'><section class='card'><h2>Services</h2>"
"<div class='row'><span class='label'>RTSP</span><span id='rtsp'>...</span></div>"
"<div class='row'><span class='label'>Recording</span><span id='rec'>...</span></div>"
"<div class='row'><span class='label'>SD card</span><span id='sd'>...</span></div>"
"<div class='row'><span class='label'>Snapshot</span><span id='snap'>...</span></div>"
"<div class='row'><span class='label'>Segments</span><span id='count'>...</span></div>"
"</section><section class='card'><h2>System</h2>"
"<div class='row'><span class='label'>Uptime</span><span id='uptime'>...</span></div>"
"<div class='row'><span class='label'>Memory</span><span id='memory'>...</span></div>"
"<div class='row'><span class='label'>Storage</span><span id='storage'>...</span></div>"
"<div class='row'><span class='label'>Board time</span><span id='time'>...</span></div>"
"</section></div><section class='card' style='margin-top:14px'><h2>Persistent event log</h2>"
"<button onclick='loadLogs()'>Refresh logs</button><pre id='logs'>Loading...</pre></section>"
"</div><script>"
"function badge(ok,text,warn){return `<span class='dot ${ok?'ok':warn?'warn':'bad'}'></span>${text}`}"
"async function loadStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});"
"if(!r.ok)throw new Error(r.status);const s=await r.json();"
"rtsp.innerHTML=badge(!!s.rtsp_running,s.rtsp_running?'RUNNING':'DOWN');"
"rec.innerHTML=badge(!!s.recording_active,s.recording_active?'ACTIVE':'IDLE/ERROR');"
"sd.innerHTML=badge(s.sd_status===2,s.sd_status===2?'READ-WRITE':s.sd_status===1?'READ-ONLY':'UNMOUNTED',s.sd_status===1);"
"snap.innerHTML=badge(!!s.snapshot_enabled,s.snapshot_enabled?'ENABLED':'DISABLED');"
"count.textContent=s.video_count;uptime.textContent=s.uptime;memory.textContent=s.memory;"
"storage.textContent=s.storage;time.textContent=s.time;}catch(e){console.error(e)}}"
"async function loadLogs(){try{const r=await fetch('/api/logs',{cache:'no-store'});"
"const a=await r.json();logs.textContent=a.join('\\n')||'(no events yet)';"
"logs.scrollTop=logs.scrollHeight;}catch(e){logs.textContent='Failed to load logs: '+e}}"
"loadStatus();loadLogs();setInterval(loadStatus,5000);</script></body></html>";

static void handle_request(int client_fd) {
    char request[HTTP_REQUEST_MAX];
    char method[16] = "";
    char path[512] = "";
    ssize_t n;

    n = read_http_request(client_fd, request, sizeof(request));
    if (n <= 0 || !strstr(request, "\r\n\r\n")) {
        send_response(client_fd, 400, "Bad Request", "text/plain; charset=utf-8",
                      "Bad Request\n", NULL);
        return;
    }

    if (sscanf(request, "%15s %511s", method, path) != 2) {
        send_response(client_fd, 400, "Bad Request", "text/plain; charset=utf-8",
                      "Bad Request\n", NULL);
        return;
    }

    if (!check_auth(request)) {
        send_response(client_fd, 401, "Unauthorized", "text/plain; charset=utf-8",
                      "Unauthorized\n",
                      "WWW-Authenticate: Basic realm=\"Luckfox Camera\"\r\n");
        return;
    }

    if (strcmp(method, "GET") != 0) {
        send_response(client_fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"error\":\"read-only monitor: GET only\"}",
                      "Allow: GET\r\n");
        return;
    }

    if (strcmp(path, "/") == 0) {
        send_response(client_fd, 200, "OK", "text/html; charset=utf-8",
                      DASHBOARD_HTML, NULL);
    } else if (strcmp(path, "/api/status") == 0) {
        send_status_json(client_fd);
    } else if (strcmp(path, "/api/logs") == 0) {
        send_logs_json(client_fd);
    } else if (strcmp(path, "/healthz") == 0) {
        camera_status_t s = status_snapshot();
        if (s.rtsp_running && s.sd_status == 2) {
            send_response(client_fd, 200, "OK", "application/json; charset=utf-8",
                          "{\"ok\":true}", NULL);
        } else {
            send_response(client_fd, 503, "Service Unavailable",
                          "application/json; charset=utf-8",
                          "{\"ok\":false}", NULL);
        }
    } else {
        send_response(client_fd, 404, "Not Found", "application/json; charset=utf-8",
                      "{\"error\":\"not found\"}", NULL);
    }
}

static void set_client_timeouts(int fd) {
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int main(void) {
    struct sigaction sa;
    pthread_t recording_tid;
    pthread_t status_tid;
    pthread_t led_tid;
    int have_recording_thread = 0;
    int have_status_thread = 0;
    int have_led_thread = 0;
    int server_fd;
    int one = 1;
    struct sockaddr_in addr;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    memset(&g_status, 0, sizeof(g_status));
    snprintf(g_status.storage, sizeof(g_status.storage), "initializing");

    log_msg("INFO", "=== Luckfox read-only status monitor starting ===");

    initial_recording_scan();

    if (pthread_create(&recording_tid, NULL, recording_watch_thread, NULL) == 0)
        have_recording_thread = 1;
    else
        log_msg("ERROR", "Failed to start recording watcher thread");

    if (pthread_create(&status_tid, NULL, status_thread, NULL) == 0)
        have_status_thread = 1;
    else
        log_msg("ERROR", "Failed to start status thread");

    if (pthread_create(&led_tid, NULL, led_thread, NULL) == 0)
        have_led_thread = 1;
    else
        log_msg("WARN", "Failed to start LED thread");

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_msg("ERROR", "socket failed: %s", strerror(errno));
        server_running = 0;
        goto shutdown;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(WEB_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_msg("ERROR", "bind port %d failed: %s", WEB_PORT, strerror(errno));
        close(server_fd);
        server_running = 0;
        goto shutdown;
    }

    if (listen(server_fd, 8) != 0) {
        log_msg("ERROR", "listen failed: %s", strerror(errno));
        close(server_fd);
        server_running = 0;
        goto shutdown;
    }

    log_msg("INFO", "HTTP monitor listening on port %d", WEB_PORT);

    while (server_running) {
        fd_set rfds;
        struct timeval tv;
        int ready;

        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ready = select(server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            log_msg("ERROR", "select failed: %s", strerror(errno));
            break;
        }
        if (ready == 0) continue;

        if (FD_ISSET(server_fd, &rfds)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno != EINTR) log_msg("WARN", "accept failed: %s", strerror(errno));
                continue;
            }

            set_client_timeouts(client_fd);
            handle_request(client_fd);
            close(client_fd);
        }
    }

    close(server_fd);

shutdown:
    server_running = 0;

    if (have_recording_thread) pthread_join(recording_tid, NULL);
    if (have_status_thread) pthread_join(status_tid, NULL);
    if (have_led_thread) pthread_join(led_tid, NULL);

    log_msg("INFO", "=== Luckfox status monitor stopped ===");

    pthread_mutex_lock(&log_mutex);
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    pthread_mutex_unlock(&log_mutex);

    return 0;
}
