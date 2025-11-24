/*
 * ==================================================================================
 * Luckfox Web Config - Advanced Status Monitor & Configuration Tool
 * ==================================================================================
 * 
 * Description:
 *   Comprehensive HTTP server for real-time monitoring and configuration
 *   management of Luckfox Pico camera system with web interface.
 * 
 * Features:
 *   - Real-time status dashboard (uptime, memory, storage, recording counts)
 *   - LED indicators for RTSP stream, recording activity, and SD card health
 *   - Live configuration editing via web interface (safe file locking)
 *   - Timezone-corrected clock display (UTC-8 / PST)
 *   - HTTP Basic Authentication for security
 *   - Dark mode UI theme
 * 
 * Endpoints:
 *   GET  /                - Main dashboard HTML page
 *   GET  /api/status      - JSON status data
 *   GET  /api/config      - Read configuration values
 *   POST /api/config      - Update configuration values
 * 
 * Configuration:
 *   Port:        8080
 *   Config File: /userdata/rkipc.ini
 *   Log File:    /mnt/sdcard/web_status.log
 *   Auth:        admin:luckfox (Base64: YWRtaW46bHVja2ZveA==)
 * 
 * Platform: Luckfox Pico Pro Max (RV1106, ARM32, Buildroot)
 * Author:   Auto-generated for embedded camera system
 * Date:     2025-11-24
 * Version:  2.0
 * ==================================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <sys/file.h>
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>

// ==================================================================================
// CONFIGURATION CONSTANTS
// ==================================================================================

#define WEB_PORT 8080                          // HTTP server listening port
#define LOG_FILE "/mnt/sdcard/web_status.log"  // Log file path on SD card
#define MAX_LOG_SIZE (2 * 1024 * 1024)         // 2MB log rotation threshold
#define BUFFER_SIZE 32768                      // General purpose buffer size
#define AUTH_BASE64 "YWRtaW46bHVja2ZveA=="     // Base64 for "admin:luckfox"

#define CONFIG_FILE "/userdata/rkipc.ini"      // Main system configuration file
#define RECORDING_PATH "/mnt/sdcard/recordings"// Video storage directory
#define SD_MOUNT_PATH "/mnt/sdcard"            // SD card mount point
#define RECORDING_TIMEOUT 300                  // 5 minutes - max gap for "recording active"

// ==================================================================================
// GLOBAL STATE
// ==================================================================================

static volatile int server_running = 1;  // Server running flag for graceful shutdown
static FILE *log_fp = NULL;              // Log file handle

// ==================================================================================
// LOGGING SYSTEM
// ==================================================================================

/**
 * log_msg() - Write timestamped log message to file
 * @level: Log level string (INFO, WARN, ERROR)
 * @fmt: Printf-style format string
 * @...: Variable arguments for format string
 * 
 * Handles log rotation when MAX_LOG_SIZE is exceeded.
 * Thread-safe for single-process use.
 */
void log_msg(const char *level, const char *fmt, ...) {
    // Open log file if not already open
    if (!log_fp) {
        log_fp = fopen(LOG_FILE, "a");
        if (!log_fp) return;
    }
    
    // Check file size and rotate if needed
    struct stat st;
    if (fstat(fileno(log_fp), &st) == 0 && st.st_size > MAX_LOG_SIZE) {
        fclose(log_fp);
        rename(LOG_FILE, "/mnt/sdcard/web_status.log.old");
        log_fp = fopen(LOG_FILE, "a");
        if (!log_fp) return;
    }
    
    // Generate timestamp
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);
    
    // Write log entry
    fprintf(log_fp, "[%s] [%s] ", timestamp, level);
    va_list args;
    va_start(args, fmt);
    vfprintf(log_fp, fmt, args);
    va_end(args);
    fprintf(log_fp, "\n");
    fflush(log_fp);
}

// ==================================================================================
// SIGNAL HANDLING
// ==================================================================================

/**
 * signal_handler() - Handle termination signals gracefully
 * @sig: Signal number (SIGINT, SIGTERM, etc.)
 * 
 * Sets server_running = 0 to trigger graceful shutdown.
 */
void signal_handler(int sig) {
    log_msg("INFO", "Received signal %d, shutting down", sig);
    server_running = 0;
}

// ==================================================================================
// AUTHENTICATION
// ==================================================================================

/**
 * check_auth() - Validate HTTP Basic Authentication
 * @auth_header: Full "Authorization:" header line from HTTP request
 * 
 * Returns: 1 if valid, 0 if invalid or missing
 * 
 * Expects: "Authorization: Basic YWRtaW46bHVja2ZveA=="
 */
int check_auth(const char *auth_header) {
    if (!auth_header) return 0;
    
    char *encoded = strstr(auth_header, "Basic ");
    if (!encoded) return 0;
    
    encoded += 6;  // Skip "Basic "
    
    // Strip whitespace and compare with expected Base64
    char clean[256] = {0};
    int i = 0;
    while (encoded[i] && encoded[i] != '\r' && encoded[i] != '\n' && encoded[i] != ' ') {
        clean[i] = encoded[i];
        i++;
    }
    
    return strcmp(clean, AUTH_BASE64) == 0;
}

/**
 * send_unauthorized() - Send 401 Unauthorized response
 * @sock: Client socket descriptor
 * 
 * Triggers browser to show login dialog.
 */
void send_unauthorized(int sock) {
    const char *response = 
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"Luckfox Camera\"\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><h1>401 Unauthorized</h1></body></html>";
    send(sock, response, strlen(response), 0);
}

// ==================================================================================
// STATUS MONITORING FUNCTIONS
// ==================================================================================

/**
 * get_rtsp_status() - Check if RTSP server is running
 * 
 * Returns: 1 if running, 0 if not
 * 
 * Checks if RTSP port 554 is listening instead of checking process name
 * because process name detection (pgrep) can be unreliable.
 */
int get_rtsp_status() {
    // Check if port 554 is listening (RTSP server running)
    int ret = system("netstat -ln 2>/dev/null | grep -q ':554 ' || ss -ln 2>/dev/null | grep -q ':554 '");
    if (ret == 0) return 1;
    
    // Fallback: check for rkipc process (less strict match)
    ret = system("pgrep rkipc > /dev/null 2>&1");
    return (ret == 0) ? 1 : 0;
}

/**
 * get_recording_status() - Check if camera is actively recording
 * 
 * Returns: 1 if recording (file modified in last 5 min), 0 if idle
 * 
 * Checks most recent file modification time in RECORDING_PATH.
 * This is more reliable than checking config values which don't
 * reflect actual recording state.
 */
int get_recording_status() {
    DIR *dir = opendir(RECORDING_PATH);
    if (!dir) return 0;
    
    time_t now = time(NULL);
    time_t newest_mtime = 0;
    
    struct dirent *entry;
    char filepath[512];
    struct stat st;
    
    // Find most recently modified file
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        snprintf(filepath, sizeof(filepath), "%s/%s", RECORDING_PATH, entry->d_name);
        
        if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st.st_mtime > newest_mtime) {
                newest_mtime = st.st_mtime;
            }
        }
    }
    closedir(dir);
    
    // Recording active if file was modified in last 5 minutes
    return (now - newest_mtime < RECORDING_TIMEOUT) ? 1 : 0;
}

/**
 * get_recording_count() - Count total video files
 * 
 * Returns: Number of video files in RECORDING_PATH
 */
int get_recording_count() {
    DIR *dir = opendir(RECORDING_PATH);
    if (!dir) return 0;
    
    int count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    closedir(dir);
    
    return count;
}

/**
 * get_sd_status() - Check SD card health and mount status
 * 
 * Returns: 0 = unmounted/error, 1 = read-only, 2 = read-write
 * 
 * Performs actual write test to verify writability.
 */
int get_sd_status() {
    struct stat st;
    
    // Check if mount point exists and is accessible
    if (stat(SD_MOUNT_PATH, &st) != 0) {
        return 0;  // Unmounted or inaccessible
    }
    
    // Try to create a test file to verify write access
    char test_file[256];
    snprintf(test_file, sizeof(test_file), "%s/.write_test_%d", SD_MOUNT_PATH, getpid());
    
    FILE *fp = fopen(test_file, "w");
    if (!fp) {
        return 1;  // Mounted but read-only
    }
    
    fclose(fp);
    unlink(test_file);  // Clean up test file
    
    return 2;  // Mounted read-write (healthy)
}

/**
 * get_uptime() - Get system uptime as formatted string
 * @buffer: Output buffer (must be at least 64 bytes)
 * 
 * Format: "2d 3h 45m" or "5h 12m" or "23m"
 */
void get_uptime(char *buffer) {
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        strcpy(buffer, "unknown");
        return;
    }
    
    long uptime = info.uptime;
    int days = uptime / 86400;
    int hours = (uptime % 86400) / 3600;
    int minutes = (uptime % 3600) / 60;
    
    if (days > 0) {
        sprintf(buffer, "%dd %dh %dm", days, hours, minutes);
    } else if (hours > 0) {
        sprintf(buffer, "%dh %dm", hours, minutes);
    } else {
        sprintf(buffer, "%dm", minutes);
    }
}

/**
 * get_memory() - Get memory usage as formatted string
 * @buffer: Output buffer (must be at least 64 bytes)
 * 
 * Format: "45.2M / 256M (17.7%)"
 * Reads MemAvailable from /proc/meminfo for accurate available memory
 */
void get_memory(char *buffer) {
    unsigned long total_kb = 0, available_kb = 0;
    
    // Read MemTotal and MemAvailable from /proc/meminfo
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        strcpy(buffer, "unknown");
        return;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu", &total_kb) == 1) {
            continue;
        }
        if (sscanf(line, "MemAvailable: %lu", &available_kb) == 1) {
            break;  // Got both values
        }
    }
    fclose(fp);
    
    if (total_kb == 0) {
        strcpy(buffer, "unknown");
        return;
    }
    
    unsigned long total_mb = total_kb / 1024;
    unsigned long available_mb = available_kb / 1024;
    unsigned long used_mb = total_mb - available_mb;
    int percent = (int)((100.0 * used_mb) / total_mb);
    
    sprintf(buffer, "%luM / %luM (%d%%)", used_mb, total_mb, percent);
}

/**
 * get_storage() - Get SD card storage usage
 * @buffer: Output buffer (must be at least 128 bytes)
 * 
 * Format: "12.5G / 119.1G"
 * 
 * Uses df command instead of statvfs() because statvfs returns
 * incorrect values on some embedded filesystems.
 */
void get_storage(char *buffer) {
    FILE *fp = popen("df -h /mnt/sdcard | tail -1 | awk '{print $3 \" / \" $2}'", "r");
    if (!fp) {
        strcpy(buffer, "unknown");
        return;
    }
    
    if (fgets(buffer, 128, fp) == NULL) {
        strcpy(buffer, "unknown");
    } else {
        // Remove trailing newline
        buffer[strcspn(buffer, "\n")] = 0;
    }
    
    pclose(fp);
}

/**
 * get_current_time() - Get current time from system
 * @buffer: Output buffer (must be at least 64 bytes)
 * 
 * Format: "2025-11-24 18:30:45"
 * 
 * Uses system localtime (respects /etc/TZ or system timezone setting)
 */
void get_current_time(char *buffer) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);  // Use system's local time
    strftime(buffer, 64, "%Y-%m-%d %H:%M:%S", t);
}

// ==================================================================================
// CONFIGURATION FILE HANDLING
// ==================================================================================

/**
 * read_config_value() - Read a value from rkipc.ini
 * @section: INI section name (e.g., "storage.0")
 * @key: Configuration key (e.g., "enable")
 * @value: Output buffer for value
 * @value_size: Size of output buffer
 * 
 * Returns: 1 on success, 0 on failure
 * 
 * Searches for [section] then reads key=value pair.
 */
int read_config_value(const char *section, const char *key, char *value, size_t value_size) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        log_msg("ERROR", "Cannot open config file for reading");
        return 0;
    }
    
    char line[512];
    char current_section[128] = {0};
    int in_section = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        // Remove trailing whitespace
        line[strcspn(line, "\r\n")] = 0;
        
        // Check for section header
        if (line[0] == '[') {
            sscanf(line, "[%127[^]]]", current_section);
            in_section = (strcmp(current_section, section) == 0);
            continue;
        }
        
        // If in target section, look for key
        if (in_section && strstr(line, "=")) {
            char file_key[128], file_value[256];
            if (sscanf(line, " %127[^ =] = %255[^\r\n]", file_key, file_value) == 2) {
                if (strcmp(file_key, key) == 0) {
                    // Trim leading/trailing spaces from value
                    char *start = file_value;
                    while (*start == ' ') start++;
                    char *end = start + strlen(start) - 1;
                    while (end > start && *end == ' ') end--;
                    *(end + 1) = '\0';
                    
                    strncpy(value, start, value_size - 1);
                    value[value_size - 1] = '\0';
                    fclose(fp);
                    return 1;
                }
            }
        }
    }
    
    fclose(fp);
    return 0;
}

/**
 * write_config_value() - Safely update a value in rkipc.ini
 * @section: INI section name
 * @key: Configuration key
 * @new_value: New value to write
 * 
 * Returns: 1 on success, 0 on failure
 * 
 * Uses atomic file replacement with file locking to prevent
 * race conditions and corruption during concurrent access.
 */
int write_config_value(const char *section, const char *key, const char *new_value) {
    // Lock config file to prevent concurrent writes
    int lock_fd = open(CONFIG_FILE, O_RDONLY);
    if (lock_fd < 0) {
        log_msg("ERROR", "Cannot open config file for locking");
        return 0;
    }
    
    if (flock(lock_fd, LOCK_EX) < 0) {
        log_msg("ERROR", "Cannot acquire lock on config file");
        close(lock_fd);
        return 0;
    }
    
    // Read original file
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        log_msg("ERROR", "Cannot open config file for reading");
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }
    
    // Create temporary file for output
    char temp_file[256];
    snprintf(temp_file, sizeof(temp_file), "%s.tmp.%d", CONFIG_FILE, getpid());
    
    FILE *out = fopen(temp_file, "w");
    if (!out) {
        log_msg("ERROR", "Cannot create temporary config file");
        fclose(fp);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }
    
    // Process file line by line
    char line[512];
    char current_section[128] = {0};
    int in_section = 0;
    int updated = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        // Check for section header
        if (line[0] == '[') {
            sscanf(line, "[%127[^]]]", current_section);
            in_section = (strcmp(current_section, section) == 0);
            fputs(line, out);
            continue;
        }
        
        // If in target section, check if this is the key to update
        if (in_section && strstr(line, "=")) {
            char file_key[128];
            if (sscanf(line, " %127[^ =]", file_key) == 1) {
                if (strcmp(file_key, key) == 0) {
                    // Replace this line with new value
                    fprintf(out, "%s = %s\n", key, new_value);
                    updated = 1;
                    continue;
                }
            }
        }
        
        // Copy line as-is
        fputs(line, out);
    }
    
    fclose(fp);
    fclose(out);
    
    if (!updated) {
        log_msg("WARN", "Config key [%s]:%s not found in file", section, key);
        unlink(temp_file);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }
    
    // Atomically replace original file
    if (rename(temp_file, CONFIG_FILE) != 0) {
        log_msg("ERROR", "Cannot replace config file: %s", strerror(errno));
        unlink(temp_file);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }
    
    // Release lock
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    
    log_msg("INFO", "Updated config [%s]:%s = %s", section, key, new_value);
    return 1;
}

// ==================================================================================
// HTTP RESPONSE FUNCTIONS
// ==================================================================================

/**
 * send_json() - Send JSON response with proper headers
 * @sock: Client socket descriptor
 * @json: JSON string to send
 */
void send_json(int sock, const char *json) {
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n%s", json);
    send(sock, response, strlen(response), 0);
}

/**
 * send_status() - Send JSON status data for dashboard
 * @sock: Client socket descriptor
 * 
 * JSON fields: rtsp_running, recording_enabled, sd_status, uptime,
 *              memory, storage, time, video_count
 */
void send_status(int sock) {
    char uptime[64], memory[64], storage[128], current_time[64];
    
    get_uptime(uptime);
    get_memory(memory);
    get_storage(storage);
    get_current_time(current_time);
    
    char json[BUFFER_SIZE];
    snprintf(json, sizeof(json),
        "{\"rtsp_running\":%d,"
        "\"recording_enabled\":%d,"
        "\"sd_status\":%d,"
        "\"uptime\":\"%s\","
        "\"memory\":\"%s\","
        "\"storage\":\"%s\","
        "\"time\":\"%s\","
        "\"video_count\":%d}",
        get_rtsp_status(),
        get_recording_status(),
        get_sd_status(),
        uptime,
        memory,
        storage,
        current_time,
        get_recording_count()
    );
    
    send_json(sock, json);
}

/**
 * send_config_data() - Send current configuration values as JSON
 * @sock: Client socket descriptor
 * 
 * Returns editable config values that users can modify via web UI.
 * Includes: storage, recording duration, video resolution, bitrate
 */
void send_config_data(int sock) {
    char storage_enable[32], folder_name[256], rtsp_enable[32];
    char file_duration[32], width[32], height[32], max_rate[32];
    char output_data_type[32];
    
    // Read storage config
    read_config_value("storage.0", "enable", storage_enable, sizeof(storage_enable));
    read_config_value("storage.0", "folder_name", folder_name, sizeof(folder_name));
    read_config_value("storage.0", "file_duration", file_duration, sizeof(file_duration));
    
    // Read video config
    read_config_value("video.source", "enable_rtsp", rtsp_enable, sizeof(rtsp_enable));
    read_config_value("video.0", "width", width, sizeof(width));
    read_config_value("video.0", "height", height, sizeof(height));
    read_config_value("video.0", "max_rate", max_rate, sizeof(max_rate));
    read_config_value("video.0", "output_data_type", output_data_type, sizeof(output_data_type));
    
    char json[BUFFER_SIZE];
    snprintf(json, sizeof(json),
        "{\"storage_enable\":\"%s\","
        "\"folder_name\":\"%s\","
        "\"file_duration\":\"%s\","
        "\"rtsp_enable\":\"%s\","
        "\"width\":\"%s\","
        "\"height\":\"%s\","
        "\"max_rate\":\"%s\","
        "\"output_data_type\":\"%s\"}",
        storage_enable,
        folder_name,
        file_duration,
        rtsp_enable,
        width,
        height,
        max_rate,
        output_data_type
    );
    
    send_json(sock, json);
}

/**
 * handle_restart_rkipc() - Restart rkipc service
 * @sock: Client socket descriptor
 * 
 * Kills and restarts rkipc process to apply config changes.
 */
void handle_restart_rkipc(int sock) {
    log_msg("INFO", "Restarting rkipc service...");
    
    // Kill rkipc
    system("killall rkipc 2>/dev/null");
    sleep(2);
    
    // Start rkipc with TZ and LD_LIBRARY_PATH
    system("export TZ='ICT-7' && export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib:$LD_LIBRARY_PATH && cd /oem && /oem/usr/bin/rkipc -a /oem/usr/share/iqfiles &");
    sleep(3);
    
    // Check if started
    int running = get_rtsp_status();
    
    char response[256];
    if (running) {
        snprintf(response, sizeof(response), "{\"success\":true,\"message\":\"rkipc restarted successfully\"}");
        log_msg("INFO", "rkipc restart successful");
    } else {
        snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"rkipc failed to start\"}");
        log_msg("ERROR", "rkipc restart failed");
    }
    
    send_json(sock, response);
}

/**
 * handle_config_update() - Process POST /api/config request
 * @sock: Client socket descriptor
 * @body: HTTP POST body containing config updates
 * 
 * Expected body format: "key1=value1&key2=value2"
 * 
 * Supported keys: storage_enable, folder_name, file_duration, 
 *                 rtsp_enable, width, height, max_rate, output_data_type
 */
void handle_config_update(int sock, const char *body) {
    if (!body || strlen(body) == 0) {
        const char *error = "{\"error\":\"Empty request body\"}";
        send_json(sock, error);
        return;
    }
    
    log_msg("INFO", "Config update request: %s", body);
    
    // Parse simple key=value& format
    char body_copy[4096];
    strncpy(body_copy, body, sizeof(body_copy) - 1);
    body_copy[sizeof(body_copy) - 1] = '\0';
    
    int success_count = 0;
    char *pair = strtok(body_copy, "&");
    
    while (pair != NULL) {
        char key[128], value[256];
        if (sscanf(pair, "%127[^=]=%255s", key, value) == 2) {
            // Map web keys to INI section/key pairs
            if (strcmp(key, "storage_enable") == 0) {
                if (write_config_value("storage.0", "enable", value)) success_count++;
            }
            else if (strcmp(key, "folder_name") == 0) {
                if (write_config_value("storage.0", "folder_name", value)) success_count++;
            }
            else if (strcmp(key, "file_duration") == 0) {
                if (write_config_value("storage.0", "file_duration", value)) success_count++;
            }
            else if (strcmp(key, "rtsp_enable") == 0) {
                if (write_config_value("video.source", "enable_rtsp", value)) success_count++;
            }
            else if (strcmp(key, "width") == 0) {
                if (write_config_value("video.0", "width", value)) success_count++;
            }
            else if (strcmp(key, "height") == 0) {
                if (write_config_value("video.0", "height", value)) success_count++;
            }
            else if (strcmp(key, "max_rate") == 0) {
                if (write_config_value("video.0", "max_rate", value)) success_count++;
            }
            else if (strcmp(key, "output_data_type") == 0) {
                if (write_config_value("video.0", "output_data_type", value)) success_count++;
            }
        }
        pair = strtok(NULL, "&");
    }
    
    char response[512];
    if (success_count > 0) {
        // Send response first
        snprintf(response, sizeof(response), "{\"success\":true,\"updated\":%d,\"message\":\"Configuration saved. Restarting services...\"}", success_count);
        send_json(sock, response);
        
        // Auto-restart rkipc after saving config (async)
        log_msg("INFO", "Auto-restarting rkipc after config update...");
        sleep(1);
        system("killall rkipc 2>/dev/null");
        sleep(2);
        system("export TZ='ICT-7' && export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib:$LD_LIBRARY_PATH && cd /oem && /oem/usr/bin/rkipc -a /oem/usr/share/iqfiles &");
        return;
    } else {
        snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"No valid updates\"}");
        send_json(sock, response);
    }
}

/**
 * send_html() - Send main dashboard HTML page
 * @sock: Client socket descriptor
 * 
 * Generates complete HTML with embedded CSS and JavaScript.
 * Includes status dashboard, LED indicators, and config editor.
 */
void send_html(int sock) {
    const char *html = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html>\n"
        "<html><head>\n"
        "<meta charset='utf-8'>\n"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
        "<title>Luckfox Camera Control</title>\n"
        "<style>\n"
        ":root{--bg:#0d0d0d;--card:#1a1a1a;--primary:#888;--success:#10b981;--warning:#f59e0b;--danger:#ef4444;--text:#e0e0e0;--text-dim:#999;--border:#2a2a2a}\n"
        "*{margin:0;padding:0;box-sizing:border-box}\n"
        "body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);padding:20px}\n"
        ".container{max-width:1200px;margin:0 auto}\n"
        "h1{font-size:28px;margin-bottom:30px;background:linear-gradient(135deg,#888,#aaa);-webkit-background-clip:text;-webkit-text-fill-color:transparent}\n"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:20px;margin-bottom:20px}\n"
        ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;box-shadow:0 4px 6px rgba(0,0,0,0.3)}\n"
        ".card h2{font-size:18px;margin-bottom:15px;color:var(--primary)}\n"
        ".status-item{display:flex;justify-content:space-between;padding:12px 0;border-bottom:1px solid var(--border)}\n"
        ".status-item:last-child{border:0}\n"
        ".label{color:var(--text-dim);font-size:14px}\n"
        ".value{font-weight:600;color:var(--text);font-size:16px}\n"
        ".led{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:8px;animation:pulse 2s infinite}\n"
        ".led.green{background:var(--success);box-shadow:0 0 10px var(--success)}\n"
        ".led.yellow{background:var(--warning);box-shadow:0 0 10px var(--warning)}\n"
        ".led.red{background:var(--danger);box-shadow:0 0 10px var(--danger)}\n"
        "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}\n"
        ".config-item{margin-bottom:15px}\n"
        ".config-item label{display:block;color:var(--text-dim);font-size:13px;margin-bottom:5px}\n"
        ".config-item input,.config-item select{width:100%;padding:10px;background:var(--bg);border:1px solid var(--border);border-radius:6px;color:var(--text);font-size:14px}\n"
        ".config-item input:focus,.config-item select:focus{outline:none;border-color:var(--primary)}\n"
        ".btn{width:100%;padding:12px;background:var(--primary);color:white;border:none;border-radius:6px;font-size:14px;font-weight:600;cursor:pointer;margin-top:10px}\n"
        ".btn:hover{background:#999}\n"
        ".btn:active{background:#777}\n"
        ".info{background:#1a1a1a;border-left:3px solid var(--primary);padding:15px;border-radius:8px;margin-top:20px}\n"
        ".info h3{color:var(--primary);margin-bottom:10px;font-size:16px}\n"
        ".info p{color:var(--text-dim);font-size:14px;line-height:1.6}\n"
        ".msg{padding:10px;border-radius:6px;margin-top:10px;font-size:13px;display:none}\n"
        ".msg.success{background:rgba(16,185,129,0.2);color:var(--success);border:1px solid var(--success)}\n"
        ".msg.error{background:rgba(239,68,68,0.2);color:var(--danger);border:1px solid var(--danger)}\n"
        "</style>\n"
        "</head><body>\n"
        "<div class='container'>\n"
        "<h1>🎥 Luckfox Camera Control</h1>\n"
        "<div class='grid'>\n"
        "<div class='card'>\n"
        "<h2>📊 System Status</h2>\n"
        "<div id='status'><div class='status-item'><span class='label'>Loading...</span></div></div>\n"
        "</div>\n"
        "<div class='card'>\n"
        "<h2>💡 LED Indicators</h2>\n"
        "<div class='status-item'><span class='label'>RTSP Stream</span><span class='value' id='led-rtsp'><span class='led red'></span>OFF</span></div>\n"
        "<div class='status-item'><span class='label'>Recording</span><span class='value' id='led-rec'><span class='led red'></span>OFF</span></div>\n"
        "<div class='status-item'><span class='label'>SD Card</span><span class='value' id='led-sd'><span class='led red'></span>ERROR</span></div>\n"
        "<button type='button' class='btn' onclick='restartRkipc()' style='margin-top:15px;background:var(--warning)'>🔄 Restart RTSP/Recording</button>\n"
        "</div>\n"
        "</div>\n"
        "<div class='card'>\n"
        "<h2>⚙️ Configuration</h2>\n"
        "<form id='configForm'>\n"
        "<div class='config-item'>\n"
        "<label>Recording Folder</label>\n"
        "<input type='text' id='folder_name' name='folder_name' placeholder='recordings'>\n"
        "</div>\n"
        "<div class='config-item'>\n"
        "<label>Recording Duration (seconds/file)</label>\n"
        "<input type='number' id='file_duration' name='file_duration' min='10' max='600' step='10' placeholder='180'>\n"
        "</div>\n"
        "<div class='config-item'>\n"
        "<label>Video Resolution</label>\n"
        "<select id='resolution' name='resolution'>\n"
        "<option value='2304x1296'>2304x1296 (3MP)</option>\n"
        "<option value='1920x1080'>1920x1080 (1080p)</option>\n"
        "<option value='1280x720'>1280x720 (720p)</option>\n"
        "<option value='704x576'>704x576 (D1)</option>\n"
        "</select>\n"
        "</div>\n"
        "<div class='config-item'>\n"
        "<label>Bitrate (kbps)</label>\n"
        "<input type='number' id='max_rate' name='max_rate' min='512' max='8192' step='128' placeholder='2048'>\n"
        "</div>\n"
        "<div class='config-item' style='display:none'>\n"
        "<select id='storage_enable' name='storage_enable'><option value='1' selected>Enabled</option></select>\n"
        "</div>\n"
        "<div class='config-item' style='display:none'>\n"
        "<select id='output_data_type' name='output_data_type'><option value='H.265' selected>H.265</option></select>\n"
        "</div>\n"
        "<div class='config-item' style='display:none'>\n"
        "<select id='rtsp_enable' name='rtsp_enable'><option value='1' selected>Enabled</option></select>\n"
        "</div>\n"
        "<button type='submit' class='btn'>💾 Save Configuration</button>\n"
        "</form>\n"
        "<div id='msg' class='msg'></div>\n"
        "</div>\n"
        "<div class='info'>\n"
        "<h3>ℹ️ About This Interface</h3>\n"
        "<p>Real-time status monitor with live configuration editing. Status updates every 5 seconds. Changes to configuration require a manual system restart to take full effect. Config file: <code>/userdata/rkipc.ini</code></p>\n"
        "</div>\n"
        "</div>\n"
        "<script>\n"
        "let cfg={};\n"
        "async function loadStatus(){\n"
        "const r=await fetch('/api/status');\n"
        "const d=await r.json();\n"
        "let h='';\n"
        "h+='<div class=\"status-item\"><span class=\"label\">Uptime</span><span class=\"value\">'+d.uptime+'</span></div>';\n"
        "h+='<div class=\"status-item\"><span class=\"label\">Memory</span><span class=\"value\">'+d.memory+'</span></div>';\n"
        "h+='<div class=\"status-item\"><span class=\"label\">Storage</span><span class=\"value\">'+d.storage+'</span></div>';\n"
        "h+='<div class=\"status-item\"><span class=\"label\">Videos</span><span class=\"value\">'+d.video_count+' files</span></div>';\n"
        "h+='<div class=\"status-item\"><span class=\"label\">Time</span><span class=\"value\">'+d.time+'</span></div>';\n"
        "document.getElementById('status').innerHTML=h;\n"
        "document.getElementById('led-rtsp').innerHTML=(d.rtsp_running?'<span class=\"led green\"></span>ON':'<span class=\"led red\"></span>OFF');\n"
        "document.getElementById('led-rec').innerHTML=(d.recording_enabled?'<span class=\"led green\"></span>ON':'<span class=\"led red\"></span>OFF');\n"
        "let sd='<span class=\"led red\"></span>ERROR';\n"
        "if(d.sd_status===2)sd='<span class=\"led green\"></span>OK';\n"
        "else if(d.sd_status===1)sd='<span class=\"led yellow\"></span>READ-ONLY';\n"
        "document.getElementById('led-sd').innerHTML=sd;\n"
        "}\n"
        "async function loadConfig(){\n"
        "const r=await fetch('/api/config');\n"
        "cfg=await r.json();\n"
        "document.getElementById('storage_enable').value=cfg.storage_enable;\n"
        "document.getElementById('folder_name').value=cfg.folder_name;\n"
        "document.getElementById('file_duration').value=cfg.file_duration;\n"
        "document.getElementById('rtsp_enable').value=cfg.rtsp_enable;\n"
        "document.getElementById('max_rate').value=cfg.max_rate;\n"
        "document.getElementById('output_data_type').value=cfg.output_data_type;\n"
        "const res=cfg.width+'x'+cfg.height;\n"
        "document.getElementById('resolution').value=res;\n"
        "}\n"
        "async function restartRkipc(){\n"
        "if(!confirm('Restart rkipc service? This will restart RTSP stream and recording.'))return;\n"
        "const btn=event.target;\n"
        "btn.disabled=true;\n"
        "btn.textContent='⏳ Restarting...';\n"
        "try{\n"
        "const r=await fetch('/api/restart',{method:'POST'});\n"
        "const res=await r.json();\n"
        "const msg=document.getElementById('msg');\n"
        "if(res.success){\n"
        "msg.className='msg success';\n"
        "msg.textContent='✓ '+res.message;\n"
        "setTimeout(()=>loadStatus(),3000);\n"
        "}else{\n"
        "msg.className='msg error';\n"
        "msg.textContent='✗ '+(res.error||'Restart failed');\n"
        "}\n"
        "msg.style.display='block';\n"
        "setTimeout(()=>{msg.style.display='none'},5000);\n"
        "}catch(e){\n"
        "alert('Error: '+e.message);\n"
        "}\n"
        "btn.disabled=false;\n"
        "btn.textContent='🔄 Restart RTSP/Recording';\n"
        "}\n"
        "document.getElementById('configForm').addEventListener('submit',async(e)=>{\n"
        "e.preventDefault();\n"
        "const form=new FormData(e.target);\n"
        "const res_val=document.getElementById('resolution').value.split('x');\n"
        "form.set('width',res_val[0]);\n"
        "form.set('height',res_val[1]);\n"
        "form.delete('resolution');\n"
        "const body=new URLSearchParams(form).toString();\n"
        "const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});\n"
        "const res=await r.json();\n"
        "const msg=document.getElementById('msg');\n"
        "if(res.success){\n"
        "msg.className='msg success';\n"
        "msg.textContent='✓ Configuration saved ('+res.updated+' values). Click Restart button to apply!';\n"
        "}else{\n"
        "msg.className='msg error';\n"
        "msg.textContent='✗ Failed to save configuration: '+(res.error||'Unknown error');\n"
        "}\n"
        "msg.style.display='block';\n"
        "setTimeout(()=>{msg.style.display='none'},8000);\n"
        "loadConfig();\n"
        "});\n"
        "loadStatus();\n"
        "loadConfig();\n"
        "setInterval(loadStatus,5000);\n"
        "</script>\n"
        "</body></html>";
    
    send(sock, html, strlen(html), 0);
}

// ==================================================================================
// HTTP REQUEST HANDLING
// ==================================================================================

/**
 * handle_request() - Main HTTP request router
 * @client_sock: Client socket descriptor
 * 
 * Routes requests to appropriate handlers:
 *   GET  /               -> send_html()
 *   GET  /api/status     -> send_status()
 *   GET  /api/config     -> send_config_data()
 *   POST /api/config     -> handle_config_update()
 *   POST /api/restart    -> handle_restart_rkipc()
 */
void handle_request(int client_sock) {
    char buffer[8192];
    int n = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        close(client_sock);
        return;
    }
    buffer[n] = '\0';
    
    // Parse HTTP method and path
    char method[16], path[256];
    sscanf(buffer, "%15s %255s", method, path);
    
    // Check authentication
    char *auth_line = strstr(buffer, "Authorization:");
    if (!check_auth(auth_line)) {
        send_unauthorized(client_sock);
        close(client_sock);
        return;
    }
    
    log_msg("INFO", "%s %s", method, path);
    
    // Route to handlers
    if (strcmp(path, "/") == 0) {
        send_html(client_sock);
    }
    else if (strcmp(path, "/api/status") == 0) {
        send_status(client_sock);
    }
    else if (strcmp(path, "/api/config") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_config_data(client_sock);
        }
        else if (strcmp(method, "POST") == 0) {
            // Extract POST body
            char *body = strstr(buffer, "\r\n\r\n");
            if (body) {
                body += 4;  // Skip past "\r\n\r\n"
                handle_config_update(client_sock, body);
            } else {
                const char *error = "{\"error\":\"Missing request body\"}";
                send_json(client_sock, error);
            }
        }
    }
    else if (strcmp(path, "/api/restart") == 0 && strcmp(method, "POST") == 0) {
        handle_restart_rkipc(client_sock);
    }
    else {
        // 404 Not Found
        const char *notfound = "HTTP/1.1 404 Not Found\r\n\r\n404 Not Found";
        send(client_sock, notfound, strlen(notfound), 0);
    }
    
    close(client_sock);
}

// ==================================================================================
// MAIN SERVER LOOP
// ==================================================================================

/**
 * main() - Entry point and server event loop
 * 
 * Initializes HTTP server on WEB_PORT (8080), accepts connections,
 * and dispatches requests to handle_request().
 * 
 * Runs until SIGINT/SIGTERM received.
 */
int main() {
    // Setup signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    log_msg("INFO", "=== Luckfox Camera Web Config v2.0 Starting ===");
    
    // Create TCP socket
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        log_msg("ERROR", "Socket creation failed");
        return 1;
    }
    
    // Enable address reuse for quick restarts
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind to port
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(WEB_PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_msg("ERROR", "Bind failed on port %d", WEB_PORT);
        close(server_sock);
        return 1;
    }
    
    // Listen for connections
    if (listen(server_sock, 5) < 0) {
        log_msg("ERROR", "Listen failed");
        close(server_sock);
        return 1;
    }
    
    log_msg("INFO", "Server listening on port %d", WEB_PORT);
    log_msg("INFO", "Timezone: Using system TZ (should be ICT-7 for Vietnam)");
    
    // Main event loop - accept and handle connections
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            if (server_running) {
                log_msg("ERROR", "Accept failed");
            }
            continue;
        }
        
        // Handle request (blocking, single-threaded)
        handle_request(client_sock);
    }
    
    // Cleanup on shutdown
    close(server_sock);
    log_msg("INFO", "=== Server stopped gracefully ===");
    if (log_fp) fclose(log_fp);
    
    return 0;
}
