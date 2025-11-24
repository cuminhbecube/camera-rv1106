/*
 * Luckfox Web Config - SIMPLE & STABLE VERSION
 * - Read-only monitoring (NO restart buttons)
 * - File logging to SD card
 * - Basic auth (admin:luckfox)
 * - Recording always enabled by default
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
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>

#define WEB_PORT 8080
#define LOG_FILE "/mnt/sdcard/web_status.log"
#define MAX_LOG_SIZE (2 * 1024 * 1024)  // 2MB
#define BUFFER_SIZE 32768
#define AUTH_BASE64 "YWRtaW46bHVja2ZveA=="  // admin:luckfox

static volatile int server_running = 1;
static FILE *log_fp = NULL;

void log_msg(const char *level, const char *fmt, ...) {
    if (!log_fp) {
        log_fp = fopen(LOG_FILE, "a");
        if (!log_fp) return;
    }
    
    struct stat st;
    if (fstat(fileno(log_fp), &st) == 0 && st.st_size > MAX_LOG_SIZE) {
        fclose(log_fp);
        rename(LOG_FILE, "/mnt/sdcard/web_status.log.old");
        log_fp = fopen(LOG_FILE, "a");
        if (!log_fp) return;
    }
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);
    
    fprintf(log_fp, "[%s] [%s] ", timestamp, level);
    va_list args;
    va_start(args, fmt);
    vfprintf(log_fp, fmt, args);
    va_end(args);
    fprintf(log_fp, "\n");
    fflush(log_fp);
}

void signal_handler(int sig) {
    server_running = 0;
    if (log_fp) fclose(log_fp);
}

int get_rtsp_status() {
    FILE *fp = popen("netstat -tuln 2>/dev/null | grep ':554 ' | wc -l", "r");
    if (!fp) return 0;
    int count = 0;
    fscanf(fp, "%d", &count);
    pclose(fp);
    return (count > 0) ? 1 : 0;
}

int get_recording_status() {
    // Check if recording folder exists and has recent files (modified in last 5 minutes)
    DIR *dir = opendir("/mnt/sdcard/recordings");
    if (!dir) return 0;
    
    time_t now = time(NULL);
    int has_recent = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".mp4")) {
            char path[512];
            snprintf(path, sizeof(path), "/mnt/sdcard/recordings/%s", entry->d_name);
            struct stat st;
            if (stat(path, &st) == 0) {
                // File modified in last 5 minutes = actively recording
                if ((now - st.st_mtime) < 300) {
                    has_recent = 1;
                    break;
                }
            }
        }
    }
    closedir(dir);
    return has_recent;
}

int get_recording_count() {
    DIR *dir = opendir("/mnt/sdcard/recordings");
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".mp4")) count++;
    }
    closedir(dir);
    return count;
}

void get_uptime(char *buf, size_t size) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        int hours = si.uptime / 3600;
        int mins = (si.uptime % 3600) / 60;
        snprintf(buf, size, "%dh %dm", hours, mins);
    } else {
        strcpy(buf, "N/A");
    }
}

void get_memory(char *buf, size_t size) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        unsigned long free_mb = si.freeram / 1024 / 1024;
        unsigned long total_mb = si.totalram / 1024 / 1024;
        snprintf(buf, size, "%luM / %luM", free_mb, total_mb);
    } else {
        strcpy(buf, "N/A");
    }
}

void get_storage(char *buf, size_t size) {
    FILE *fp = popen("df -h /mnt/sdcard 2>/dev/null | tail -1", "r");
    if (!fp) {
        strcpy(buf, "N/A");
        return;
    }
    
    char line[256], avail[32], total[32];
    if (fgets(line, sizeof(line), fp)) {
        // Parse: /dev/mmcblk1p1  119.1G  623.5M  118.5G   1% /mnt/sdcard
        if (sscanf(line, "%*s %s %*s %s", total, avail) == 2) {
            snprintf(buf, size, "%s free / %s total", avail, total);
        } else {
            strcpy(buf, "N/A");
        }
    } else {
        strcpy(buf, "N/A");
    }
    pclose(fp);
}

int check_auth(const char *auth_header) {
    if (!auth_header) return 0;
    return strstr(auth_header, AUTH_BASE64) != NULL;
}

void send_unauthorized(int sock) {
    const char *response = 
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"Luckfox Admin\"\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><h1>401 Unauthorized</h1></body></html>";
    send(sock, response, strlen(response), 0);
}

void send_json(int sock, const char *json) {
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n%s", json);
    send(sock, response, strlen(response), 0);
}

void send_status(int sock) {
    char uptime[64], memory[64], storage[128];
    get_uptime(uptime, sizeof(uptime));
    get_memory(memory, sizeof(memory));
    get_storage(storage, sizeof(storage));
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char current_time[64];
    strftime(current_time, sizeof(current_time), "%Y-%m-%d %H:%M:%S", t);
    
    char json[BUFFER_SIZE];
    snprintf(json, sizeof(json),
        "{\"rtsp_running\":%d,"
        "\"recording_enabled\":%d,"
        "\"uptime\":\"%s\","
        "\"memory\":\"%s\","
        "\"storage\":\"%s\","
        "\"time\":\"%s\","
        "\"video_count\":%d}",
        get_rtsp_status(),
        get_recording_status(),
        uptime,
        memory,
        storage,
        current_time,
        get_recording_count()
    );
    
    send_json(sock, json);
}

void send_html(int sock) {
    const char *html = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html>\n"
        "<html><head>\n"
        "<meta charset='utf-8'>\n"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
        "<title>Luckfox Status Monitor</title>\n"
        "<style>\n"
        ":root{--bg:#0d0d0d;--card:#1a1a1a;--primary:#888;--success:#10b981;--danger:#ef4444;--text:#e0e0e0;--text-dim:#999;--border:#2a2a2a}\n"
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
        ".badge{display:inline-block;padding:6px 16px;border-radius:20px;font-size:13px;font-weight:600}\n"
        ".badge.success{background:var(--success);color:white}\n"
        ".badge.danger{background:var(--danger);color:white}\n"
        ".led{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:8px;animation:pulse 2s infinite}\n"
        ".led.green{background:#10b981;box-shadow:0 0 10px #10b981}\n"
        ".led.red{background:#ef4444;box-shadow:0 0 10px #ef4444}\n"
        "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}\n"
        ".info{background:#1a1a1a;border-left:3px solid var(--primary);padding:15px;border-radius:8px;margin-top:20px}\n"
        ".info h3{color:var(--primary);margin-bottom:10px;font-size:16px}\n"
        ".info p{color:var(--text-dim);font-size:14px;line-height:1.6}\n"
        "</style>\n"
        "</head><body>\n"
        "<div class='container'>\n"
        "<h1>🎥 Luckfox Status Monitor</h1>\n"
        "<div class='grid'>\n"
        "<div class='card'>\n"
        "<h2>📊 System Status</h2>\n"
        "<div id='status'><div class='status-item'><span class='label'>Loading...</span></div></div>\n"
        "</div>\n"
        "<div class='card'>\n"
        "<h2>💡 LED Indicators</h2>\n"
        "<div class='status-item'>\n"
        "<span class='label'>RTSP Stream</span>\n"
        "<span class='value' id='led-rtsp'><span class='led red'></span>OFF</span>\n"
        "</div>\n"
        "<div class='status-item'>\n"
        "<span class='label'>Recording</span>\n"
        "<span class='value' id='led-rec'><span class='led red'></span>OFF</span>\n"
        "</div>\n"
        "</div>\n"
        "</div>\n"
        "<div class='info'>\n"
        "<h3>ℹ️ Read-Only Monitor</h3>\n"
        "<p>This is a stable monitoring interface. Recording is always enabled and managed by the system. To change settings, edit <code>/userdata/rkipc.ini</code> manually and restart the device.</p>\n"
        "</div>\n"
        "</div>\n"
        "<script>\n"
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
        "}\n"
        "loadStatus();\n"
        "setInterval(loadStatus,5000);\n"
        "</script>\n"
        "</body></html>";
    
    send(sock, html, strlen(html), 0);
}

void handle_request(int client_sock) {
    char buffer[4096];
    int n = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        close(client_sock);
        return;
    }
    buffer[n] = '\0';
    
    char method[16], path[256];
    sscanf(buffer, "%s %s", method, path);
    
    char *auth_line = strstr(buffer, "Authorization:");
    if (!check_auth(auth_line)) {
        send_unauthorized(client_sock);
        close(client_sock);
        return;
    }
    
    log_msg("INFO", "%s %s", method, path);
    
    if (strcmp(path, "/") == 0) {
        send_html(client_sock);
    }
    else if (strcmp(path, "/api/status") == 0) {
        send_status(client_sock);
    }
    else {
        const char *notfound = "HTTP/1.1 404 Not Found\r\n\r\n404";
        send(client_sock, notfound, strlen(notfound), 0);
    }
    
    close(client_sock);
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    log_msg("INFO", "=== Luckfox Status Monitor Starting ===");
    
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        log_msg("ERROR", "Socket creation failed");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(WEB_PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_msg("ERROR", "Bind failed on port %d", WEB_PORT);
        close(server_sock);
        return 1;
    }
    
    if (listen(server_sock, 5) < 0) {
        log_msg("ERROR", "Listen failed");
        close(server_sock);
        return 1;
    }
    
    log_msg("INFO", "Server listening on port %d", WEB_PORT);
    
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            if (server_running) log_msg("ERROR", "Accept failed");
            continue;
        }
        
        handle_request(client_sock);
    }
    
    close(server_sock);
    log_msg("INFO", "=== Server stopped ===");
    if (log_fp) fclose(log_fp);
    
    return 0;
}
