/*
 * Video streaming and recording for Luckfox Pico Pro (RV1106)
 * Uses Rockchip MPP (Media Process Platform) for H.264 encoding
 * 
 * Features:
 * - RTSP streaming server
 * - Concurrent MP4 recording with 3-minute segments
 * - V4L2 camera capture
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

// Configuration - Will be overridden by config file
#define DEFAULT_VIDEO_WIDTH     1920
#define DEFAULT_VIDEO_HEIGHT    1080
#define DEFAULT_VIDEO_FPS       30
#define DEFAULT_VIDEO_BITRATE   2000000  // 2 Mbps
#define DEFAULT_SEGMENT_DURATION 180    // 3 minutes in seconds
#define DEFAULT_RTSP_PORT       8554
#define SD_MOUNT_PATH           "/mnt/sdcard"
#define RECORD_PATH             "/mnt/sdcard/recordings"
#define CONFIG_FILE_PATH        "/mnt/sdcard/luckfox_config.ini"
#define LOG_FILE_PATH           "/mnt/sdcard/system.log"
#define STATUS_FILE_PATH        "/tmp/video_status.json"
#define SNAPSHOT_FILE_PATH      "/tmp/snapshot.jpg"
#define LED_GPIO_PIN            71  // GPIO2_A7 (2*32 + 0*8 + 7 = 71)

// Global status variables
static int g_rtsp_clients = 0;
static int g_is_recording = 0;

// Logging Function
#include <stdarg.h>
static void log_message(const char *fmt, ...) {
    // Try to log to SD card first
    FILE *fp = fopen(LOG_FILE_PATH, "a");
    if (!fp) {
        // Fallback to /tmp/system.log if SD card is not writable
        fp = fopen("/tmp/system.log", "a");
    }
    if (!fp) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fprintf(fp, "\n");
    fclose(fp);
}

// GPIO Functions
static int gpio_export(int pin) {
    char buffer[64];
    int len;
    int fd;

    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
        // If already exported, this might fail, which is fine
        return -1;
    }

    len = snprintf(buffer, sizeof(buffer), "%d", pin);
    write(fd, buffer, len);
    close(fd);
    return 0;
}

static int gpio_set_direction(int pin, const char *dir) {
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;

    write(fd, dir, strlen(dir));
    close(fd);
    return 0;
}

static int gpio_write(int pin, int value) {
    char path[64];
    char buffer[2];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;

    snprintf(buffer, sizeof(buffer), "%d", value);
    write(fd, buffer, 1);
    close(fd);
    return 0;
}

// SD Card Management
static int check_and_mount_sd() {
    // Check if already mounted
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, SD_MOUNT_PATH)) {
                fclose(fp);
                printf("SD card already mounted at %s\n", SD_MOUNT_PATH);
                return 0;
            }
        }
        fclose(fp);
    }

    // Try to mount
    printf("Attempting to mount SD card...\n");
    
    // Ensure mount point exists
    mkdir(SD_MOUNT_PATH, 0777);

    // Try mounting partition 1 (mmcblk1p1)
    // Try exFAT first, then auto
    int ret = system("mount -t exfat /dev/mmcblk1p1 " SD_MOUNT_PATH);
    if (ret != 0) {
        ret = system("mount -t vfat /dev/mmcblk1p1 " SD_MOUNT_PATH);
    }
    if (ret != 0) {
        ret = system("mount /dev/mmcblk1p1 " SD_MOUNT_PATH);
    }
    
    if (ret != 0) {
        // Try mounting raw device (mmcblk1)
        ret = system("mount -t exfat /dev/mmcblk1 " SD_MOUNT_PATH);
        if (ret != 0) {
            ret = system("mount -t vfat /dev/mmcblk1 " SD_MOUNT_PATH);
        }
        if (ret != 0) {
            ret = system("mount /dev/mmcblk1 " SD_MOUNT_PATH);
        }
    }

    if (ret != 0) {
        printf("Mount failed. Checking if device exists...\n");
        
        // Check if device exists
        if (access("/dev/mmcblk1", F_OK) == 0) {
            printf("SD card detected at /dev/mmcblk1. Mount failed (exFAT/FAT32).\n");
            // Do NOT format automatically if user says it's always exFAT
            return -1;
        } else {
            printf("No SD card detected (/dev/mmcblk1 not found)\n");
            return -1;
        }
    }

    if (ret == 0) {
        printf("SD card mounted successfully\n");
        // Ensure recording directory exists
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "mkdir -p %s", RECORD_PATH);
        system(cmd);
        return 0;
    } else {
        printf("Failed to mount SD card\n");
        return -1;
    }
}

// Global config variables (loaded from file)
static int VIDEO_WIDTH = DEFAULT_VIDEO_WIDTH;
static int VIDEO_HEIGHT = DEFAULT_VIDEO_HEIGHT;
static int VIDEO_FPS = DEFAULT_VIDEO_FPS;
static int VIDEO_BITRATE = DEFAULT_VIDEO_BITRATE;
static int SEGMENT_DURATION = DEFAULT_SEGMENT_DURATION;
static int RTSP_PORT = DEFAULT_RTSP_PORT;
static int ENABLE_RTSP = 1;
static int ENABLE_RECORDING = 1;
static int ENABLE_TIMESTAMP_OSD = 1;

// Status Update Function
static void update_status_file() {
    FILE *fp = fopen(STATUS_FILE_PATH, "w");
    if (!fp) return;
    
    fprintf(fp, "{\"recording\":%d,\"rtsp_clients\":%d,\"rtsp_port\":%d}", 
            g_is_recording, g_rtsp_clients, RTSP_PORT);
    fclose(fp);
}

typedef struct {
    unsigned char *data;
    size_t size;
    int64_t pts;
    int keyframe;
} VideoFrame;

typedef struct {
    VideoFrame *frames;
    int capacity;
    int read_idx;
    int write_idx;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int active;
} FrameQueue;

static volatile int g_running = 1;
static FrameQueue g_frame_queue;

// Config file functions
static void create_default_config(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Warning: Cannot create config file at %s: %s\n", path, strerror(errno));
        return;
    }
    
    fprintf(f, "# Luckfox Pico Pro Video Configuration\n");
    fprintf(f, "# Auto-generated config file\n\n");
    fprintf(f, "[camera]\n");
    fprintf(f, "width = %d\n", DEFAULT_VIDEO_WIDTH);
    fprintf(f, "height = %d\n", DEFAULT_VIDEO_HEIGHT);
    fprintf(f, "fps = %d\n", DEFAULT_VIDEO_FPS);
    fprintf(f, "\n[encoder]\n");
    fprintf(f, "bitrate = %d\n", DEFAULT_VIDEO_BITRATE);
    fprintf(f, "\n[recording]\n");
    fprintf(f, "enabled = 1\n");
    fprintf(f, "segment_duration = %d  # seconds (60 = 1 minute)\n", DEFAULT_SEGMENT_DURATION);
    fprintf(f, "path = %s\n", RECORD_PATH);
    fprintf(f, "\n[rtsp]\n");
    fprintf(f, "enabled = 1\n");
    fprintf(f, "port = %d\n", DEFAULT_RTSP_PORT);
    fprintf(f, "\n[system]\n");
    fprintf(f, "timestamp_osd = 1  # Show timestamp on video\n");
    fprintf(f, "\n# Notes:\n");
    fprintf(f, "# - Edit this file to change settings\n");
    fprintf(f, "# - Reboot board for changes to take effect\n");
    fprintf(f, "# - segment_duration: video file length in seconds\n");
    
    fclose(f);
    printf("Created default config file: %s\n", path);
    log_message("Created default config file: %s", path);
}

static int parse_config_line(const char *line, const char *key, char *value, size_t value_size) {
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "%s = %%[^\n]", key);
    return sscanf(line, fmt, value) == 1;
}

static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Config file not found, creating default: %s\n", path);
        create_default_config(path);
        return;
    }
    
    printf("Loading config from: %s\n", path);
    char line[256];
    char value[128];
    char current_section[32] = "";
    
    while (fgets(line, sizeof(line), f)) {
        // Track current section
        if (line[0] == '[') {
            sscanf(line, "[%31[^]]", current_section);
            continue;
        }
        
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;
        
        if (parse_config_line(line, "width", value, sizeof(value))) {
            VIDEO_WIDTH = atoi(value);
        } else if (parse_config_line(line, "height", value, sizeof(value))) {
            VIDEO_HEIGHT = atoi(value);
        } else if (parse_config_line(line, "fps", value, sizeof(value))) {
            VIDEO_FPS = atoi(value);
        } else if (parse_config_line(line, "bitrate", value, sizeof(value))) {
            VIDEO_BITRATE = atoi(value);
        } else if (parse_config_line(line, "segment_duration", value, sizeof(value))) {
            SEGMENT_DURATION = atoi(value);
        } else if (parse_config_line(line, "port", value, sizeof(value))) {
            RTSP_PORT = atoi(value);
        } else if (parse_config_line(line, "enabled", value, sizeof(value))) {
            // Parse enabled based on current section
            int enabled_val = atoi(value);
            if (strcmp(current_section, "recording") == 0) {
                ENABLE_RECORDING = enabled_val;
            } else if (strcmp(current_section, "rtsp") == 0) {
                ENABLE_RTSP = enabled_val;
            }
        }
    }
    
    fclose(f);
    printf("Config loaded:\n");
    log_message("Config loaded from %s", path);
    printf("  Resolution: %dx%d @ %dfps\n", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
    printf("  Bitrate: %d bps\n", VIDEO_BITRATE);
    printf("  Segment: %d seconds\n", SEGMENT_DURATION);
    printf("  RTSP Port: %d\n", RTSP_PORT);
    printf("  Recording: %s\n", ENABLE_RECORDING ? "Enabled" : "Disabled");
    log_message("Config: Res=%dx%d FPS=%d Bitrate=%d Seg=%ds RTSP=%d Rec=%d", 
        VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS, VIDEO_BITRATE, SEGMENT_DURATION, RTSP_PORT, ENABLE_RECORDING);
}

// Signal handler
static void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    log_message("Received signal %d, shutting down...", sig);
    g_running = 0;
}

// Frame queue functions
static int frame_queue_init(FrameQueue *q, int capacity) {
    q->frames = calloc(capacity, sizeof(VideoFrame));
    if (!q->frames) return -1;
    q->capacity = capacity;
    q->read_idx = q->write_idx = 0;
    q->active = 1;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    return 0;
}

static void frame_queue_destroy(FrameQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->active = 0;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    
    for (int i = 0; i < q->capacity; i++) {
        if (q->frames[i].data) free(q->frames[i].data);
    }
    free(q->frames);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int frame_queue_push(FrameQueue *q, const unsigned char *data, size_t size, int64_t pts, int keyframe) {
    pthread_mutex_lock(&q->mutex);
    
    int next_idx = (q->write_idx + 1) % q->capacity;
    if (next_idx == q->read_idx) {
        // Queue full, drop oldest
        if (q->frames[q->read_idx].data) free(q->frames[q->read_idx].data);
        q->read_idx = (q->read_idx + 1) % q->capacity;
    }
    
    VideoFrame *frame = &q->frames[q->write_idx];
    if (frame->data) free(frame->data);
    frame->data = malloc(size);
    if (!frame->data) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    memcpy(frame->data, data, size);
    frame->size = size;
    frame->pts = pts;
    frame->keyframe = keyframe;
    
    q->write_idx = next_idx;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int frame_queue_pop(FrameQueue *q, VideoFrame *out) {
    pthread_mutex_lock(&q->mutex);
    
    while (q->read_idx == q->write_idx && q->active && g_running) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    
    if (!q->active || !g_running) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    
    VideoFrame *frame = &q->frames[q->read_idx];
    out->data = malloc(frame->size);
    if (!out->data) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    memcpy(out->data, frame->data, frame->size);
    out->size = frame->size;
    out->pts = frame->pts;
    out->keyframe = frame->keyframe;
    
    q->read_idx = (q->read_idx + 1) % q->capacity;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

// Placeholder: Camera capture thread (V4L2 + MPP encoding)
static void *camera_thread(void *arg) {
    (void)arg;
    printf("[CAMERA] Thread started (placeholder - requires MPP SDK)\n");
    
    // In real implementation:
    // 1. Open /dev/video0 with V4L2
    // 2. Configure format (NV12/YUV420)
    // 3. Initialize Rockchip MPP encoder (H.264)
    // 4. If ENABLE_TIMESTAMP_OSD: Add OSD overlay with current time
    // 5. Capture frames, encode, push to queue
    
    int frame_count = 0;
    while (g_running) {
        // Simulate frame rate
        usleep(1000000 / VIDEO_FPS);
        
        // Get current time
        time_t now = time(NULL);
        struct tm tm_gmt7;
        localtime_r(&now, &tm_gmt7);
        
        // Dummy H.264 NAL unit with timestamp (replace with real MPP output + OSD)
        unsigned char dummy_frame[4096];
        int size = snprintf((char*)dummy_frame, sizeof(dummy_frame),
                           "FRAME_%06d_TIME_%04d%02d%02d_%02d%02d%02d",
                           frame_count,
                           tm_gmt7.tm_year + 1900, tm_gmt7.tm_mon + 1, tm_gmt7.tm_mday,
                           tm_gmt7.tm_hour, tm_gmt7.tm_min, tm_gmt7.tm_sec);
        
        int64_t pts = frame_count * (1000000 / VIDEO_FPS);
        int keyframe = (frame_count % (VIDEO_FPS * 2)) == 0; // I-frame every 2 sec
        
        if (frame_queue_push(&g_frame_queue, dummy_frame, size, pts, keyframe) < 0) {
            fprintf(stderr, "[CAMERA] Failed to push frame %d\n", frame_count);
        }
        frame_count++;
        
        if (frame_count % (VIDEO_FPS * 10) == 0) {
            printf("[CAMERA] Captured %d frames (%.1f min)\n", 
                   frame_count, frame_count / (float)(VIDEO_FPS * 60));
            
            // Save snapshot (simulated)
            FILE *snap = fopen(SNAPSHOT_FILE_PATH, "w");
            if (snap) {
                // In real implementation, this would be a JPEG encoded frame
                fprintf(snap, "Simulated JPEG Snapshot\nTime: %ld\nFrame: %d", now, frame_count);
                fclose(snap);
            }
        }
    }
    
    printf("[CAMERA] Thread stopped\n");
    return NULL;
}

// RTSP streaming thread
static void *rtsp_thread(void *arg) {
    (void)arg;
    
    if (!ENABLE_RTSP) {
        printf("[RTSP] Disabled by config\n");
        return NULL;
    }
    
    printf("[RTSP] Server thread started on port %d (placeholder)\n", RTSP_PORT);
    update_status_file();
    
    // In real implementation:
    // 1. Create RTSP server socket
    // 2. Handle DESCRIBE/SETUP/PLAY requests
    // 3. Stream H.264 frames via RTP
    // Library: live555 or custom lightweight RTSP
    
    int stream_count = 0;
    while (g_running) {
        VideoFrame frame;
        if (frame_queue_pop(&g_frame_queue, &frame) < 0) break;
        
        // Simulate streaming delay
        usleep(1000);
        
        // Simulate client connection (toggle every 10 seconds for demo)
        static int sim_client_timer = 0;
        sim_client_timer++;
        if (sim_client_timer == 300) { // ~10s at 30fps
             g_rtsp_clients = 1;
             update_status_file();
        } else if (sim_client_timer == 600) {
             g_rtsp_clients = 0;
             update_status_file();
             sim_client_timer = 0;
        }

        stream_count++;
        if (stream_count % (VIDEO_FPS * 10) == 0) {
            printf("[RTSP] Streamed %d frames (%.1f min), last size: %zu bytes %s\n",
                   stream_count, stream_count / (float)(VIDEO_FPS * 60),
                   frame.size, frame.keyframe ? "[KEYFRAME]" : "");
        }
        
        free(frame.data);
    }
    
    printf("[RTSP] Server stopped, streamed %d frames\n", stream_count);
    return NULL;
}

// Recording thread with configurable segments
static void *record_thread(void *arg) {
    (void)arg;
    
    if (!ENABLE_RECORDING) {
        printf("[RECORD] Disabled by config\n");
        log_message("[RECORD] Disabled by config");
        return NULL;
    }
    
    printf("[RECORD] Thread started, saving to %s\n", RECORD_PATH);
    log_message("[RECORD] Thread started, saving to %s", RECORD_PATH);
    printf("[RECORD] Segment duration: %d seconds\n", SEGMENT_DURATION);
    
    g_is_recording = 1;
    update_status_file();

    // Ensure recording directory exists
    struct stat st = {0};
    if (stat(RECORD_PATH, &st) == -1) {
        printf("[RECORD] Creating directory: %s\n", RECORD_PATH);
        if (mkdir(RECORD_PATH, 0755) != 0) {
            fprintf(stderr, "[RECORD] Failed to create %s: %s\n", RECORD_PATH, strerror(errno));
            log_message("[RECORD] ERROR: Failed to create directory %s: %s", RECORD_PATH, strerror(errno));
            return NULL;
        }
    }
    
    FILE *out_file = NULL;
    time_t segment_start = 0;
    int segment_num = 0;
    int frame_count = 0;
    
    // LED Blink State
    int led_state = 0;
    int led_counter = 0;
    
    while (g_running) {
        VideoFrame frame;
        if (frame_queue_pop(&g_frame_queue, &frame) < 0) break;
        
        // Blink LED (Active Low: 0=ON, 1=OFF)
        // Blink every 15 frames (approx 0.5s at 30fps)
        led_counter++;
        if (led_counter >= VIDEO_FPS / 2) {
            led_state = !led_state;
            gpio_write(LED_GPIO_PIN, led_state ? 0 : 1); // 0=ON, 1=OFF
            led_counter = 0;
        }
        
        time_t now = time(NULL);
        
        // Create new segment file based on SEGMENT_DURATION
        if (!out_file || (now - segment_start) >= SEGMENT_DURATION) {
            if (out_file) {
                fclose(out_file);
                printf("[RECORD] Segment %d closed: %d frames (%d sec)\n", 
                       segment_num, frame_count, SEGMENT_DURATION);
                log_message("[RECORD] Segment %d closed: %d frames", segment_num, frame_count);
            }
            
            char filename[256];
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            
            // Filename format: video_YYYYMMDD_HHMMSS_segNNN.h264
            snprintf(filename, sizeof(filename), 
                    "%s/video_%04d%02d%02d_%02d%02d%02d_seg%03d.h264",
                    RECORD_PATH,
                    tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                    tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                    segment_num);
            
            out_file = fopen(filename, "wb");
            if (!out_file) {
                fprintf(stderr, "[RECORD] Failed to create %s: %s\n", filename, strerror(errno));
                log_message("[RECORD] ERROR: Failed to create file %s: %s", filename, strerror(errno));
                free(frame.data);
                break;
            }
            
            printf("[RECORD] New segment: %s (duration: %ds)\n", filename, SEGMENT_DURATION);
            log_message("[RECORD] New segment: %s", filename);
            segment_start = now;
            segment_num++;
            frame_count = 0;
        }
        
        // Write frame to file
        if (fwrite(frame.data, 1, frame.size, out_file) != frame.size) {
            fprintf(stderr, "[RECORD] Write error\n");
            log_message("[RECORD] ERROR: Write error to file");
        }
        fflush(out_file);  // Ensure data is written
        frame_count++;
        
        free(frame.data);
    }
    
    if (out_file) {
        fclose(out_file);
        printf("[RECORD] Final segment %d closed: %d frames\n", segment_num, frame_count);
        log_message("[RECORD] Final segment %d closed: %d frames", segment_num, frame_count);
    }
    
    // Turn off LED when stopped (Active Low: 1=OFF)
    gpio_write(LED_GPIO_PIN, 1);
    
    g_is_recording = 0;
    update_status_file();

    printf("[RECORD] Thread stopped\n");
    log_message("[RECORD] Thread stopped");
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    printf("=== Luckfox Pico Pro Video Streaming + Recording ===\n");
    printf("Version: 2.1 (Auto-SD, LED Blink, Config-driven)\n\n");
    
    // Step 0: Initialize GPIO for LED
    printf("Initializing LED GPIO %d...\n", LED_GPIO_PIN);
    gpio_export(LED_GPIO_PIN);
    gpio_set_direction(LED_GPIO_PIN, "out");
    gpio_write(LED_GPIO_PIN, 1); // Turn off initially (Active Low)
    
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    printf("System time: %04d-%02d-%02d %02d:%02d:%02d\n",
           tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
           tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    
    // Step 2: Check and Mount SD Card
    if (check_and_mount_sd() != 0) {
        fprintf(stderr, "CRITICAL: SD card not available. Recording disabled.\n");
        log_message("CRITICAL: SD card not available. Recording disabled.");
        ENABLE_RECORDING = 0;
        
        // Turn ON LED permanently to indicate error (Active Low: 0=ON)
        gpio_write(LED_GPIO_PIN, 0);
    } else {
        log_message("System started. SD card mounted.");
    }
    
    update_status_file();

    // Step 3: Load or create config file
    load_config(CONFIG_FILE_PATH);
    
    printf("\nConfiguration:\n");
    printf("  Resolution: %dx%d @ %d fps\n", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
    printf("  Bitrate: %d bps\n", VIDEO_BITRATE);
    printf("  RTSP: %s (port %d)\n", ENABLE_RTSP ? "Enabled" : "Disabled", RTSP_PORT);
    printf("  Recording: %s\n", ENABLE_RECORDING ? "Enabled" : "Disabled");
    printf("  Segment Duration: %d seconds\n", SEGMENT_DURATION);
    printf("  Record Path: %s\n", RECORD_PATH);
    printf("  Config File: %s\n", CONFIG_FILE_PATH);
    printf("  Timestamp OSD: %s\n", ENABLE_TIMESTAMP_OSD ? "Enabled" : "Disabled");
    
    printf("\nNOTE: This is a FRAMEWORK. Real implementation requires:\n");
    printf("  - Rockchip MPP SDK for H.264 encoding\n");
    printf("  - V4L2 camera drivers\n");
    printf("  - RTSP library (live555 or custom)\n");
    printf("  - RGA for OSD overlay (timestamp)\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize frame queue
    if (frame_queue_init(&g_frame_queue, VIDEO_FPS * 2) < 0) {
        fprintf(stderr, "Failed to initialize frame queue\n");
        return 1;
    }
    
    pthread_t cam_tid, rtsp_tid, rec_tid;
    
    // Start camera thread
    pthread_create(&cam_tid, NULL, camera_thread, NULL);
    usleep(100000); // Let camera start first
    
    // Start RTSP thread if enabled
    if (ENABLE_RTSP) {
        pthread_create(&rtsp_tid, NULL, rtsp_thread, NULL);
    }
    
    // Start recording thread if enabled
    if (ENABLE_RECORDING) {
        pthread_create(&rec_tid, NULL, record_thread, NULL);
    }
    
    // Wait for camera thread
    pthread_join(cam_tid, NULL);
    
    // Wait for other threads
    if (ENABLE_RTSP) pthread_join(rtsp_tid, NULL);
    if (ENABLE_RECORDING) pthread_join(rec_tid, NULL);
    
    frame_queue_destroy(&g_frame_queue);
    
    printf("\nShutdown complete.\n");
    return 0;
}
