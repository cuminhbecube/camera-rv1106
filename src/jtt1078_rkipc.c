/*
 * JT/T 1078 Integration with rkipc
 * Tích hợp JT/T 1078 với video encoder rkipc
 * 
 * This file shows how to integrate JT/T 1078 protocol with rkipc's
 * video encoder to stream H.265 video directly from camera.
 * 
 * Build:
 *   arm-rockchip830-linux-uclibcgnueabihf-gcc \
 *       -o jtt1078_rkipc \
 *       jtt1078_protocol.c \
 *       jtt1078_rkipc.c \
 *       -I/path/to/luckfox-pico/media/rkipc/include \
 *       -L/path/to/luckfox-pico/media/rkipc/lib \
 *       -lrockchip_mpp -leasymedia -lpthread -O2
 */

#include "jtt1078_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// Rockchip MPP headers (bạn cần điều chỉnh path)
// #include "rk_mpi_venc.h"
// #include "rk_comm_venc.h"

// Mock structures nếu chưa có MPP headers
typedef struct {
    uint8_t *pu8Addr;
    uint32_t u32Len;
    uint64_t u64PTS;
    int bFrameEnd;
    struct {
        int enH265EType;  // 0=P, 1=I/IDR
    } DataType;
} VENC_PACK_S;

typedef struct {
    VENC_PACK_S *pstPack;
    uint32_t u32PackCount;
} VENC_STREAM_S;

#define H265E_NALU_PSLICE  0
#define H265E_NALU_ISLICE  1

// Global variables
static volatile int g_running = 1;
static int g_tcp_sock = -1;
static jtt1078_encoder_t g_encoder;

// Signal handler
void signal_handler(int sig) {
    printf("\n[JTT1078] Caught signal %d, exiting...\n", sig);
    g_running = 0;
}

// TCP send callback
int jtt1078_tcp_send(const uint8_t *data, size_t len, void *user_data) {
    (void)user_data;
    
    if (g_tcp_sock < 0) {
        return -1;
    }
    
    size_t sent = 0;
    while (sent < len) {
        ssize_t ret = send(g_tcp_sock, data + sent, len - sent, 0);
        if (ret < 0) {
            perror("[JTT1078] send");
            return -1;
        }
        sent += ret;
    }
    
    return sent;
}

// Connect to JT/T 1078 server
int connect_to_server(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[JTT1078] socket");
        return -1;
    }
    
    // Set TCP_NODELAY for low latency
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    // Set send buffer
    int sendbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    
    printf("[JTT1078] Connecting to %s:%d...\n", ip, port);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[JTT1078] connect");
        close(sock);
        return -1;
    }
    
    printf("[JTT1078] Connected successfully\n");
    return sock;
}

// Main video streaming thread
void* venc_stream_thread(void *arg) {
    int venc_chn = 0;  // Video encoder channel 0
    VENC_STREAM_S stStream;
    
    printf("[JTT1078] Video streaming thread started\n");
    
    while (g_running) {
        // TODO: Replace with actual RK_MPI_VENC_GetStream()
        // For now, simulate getting frames
        
        // Actual code would be:
        /*
        int ret = RK_MPI_VENC_GetStream(venc_chn, &stStream, -1);
        if (ret != 0) {
            printf("[JTT1078] RK_MPI_VENC_GetStream failed: %d\n", ret);
            usleep(10000);
            continue;
        }
        
        // Process each NAL unit in the stream
        for (uint32_t i = 0; i < stStream.u32PackCount; i++) {
            VENC_PACK_S *pack = &stStream.pstPack[i];
            
            // Create video frame
            video_frame_t frame;
            frame.data = pack->pu8Addr;
            frame.size = pack->u32Len;
            frame.pts = pack->u64PTS;
            
            // Detect frame type
            if (pack->DataType.enH265EType == H265E_NALU_ISLICE) {
                frame.is_keyframe = true;
                frame.frame_type = JTT1078_DATA_TYPE_VIDEO;
            } else {
                frame.is_keyframe = false;
                frame.frame_type = JTT1078_DATA_TYPE_VIDEO_P;
            }
            
            // Encode and send via JT/T 1078
            if (jtt1078_encode_video_frame(&g_encoder, &frame) < 0) {
                printf("[JTT1078] Failed to send frame\n");
            }
        }
        
        // Release stream
        RK_MPI_VENC_ReleaseStream(venc_chn, &stStream);
        */
        
        // Simulation for testing without actual camera
        usleep(40000);  // 25fps = 40ms per frame
    }
    
    printf("[JTT1078] Video streaming thread stopped\n");
    return NULL;
}

// Parse config file
int parse_config(const char *config_file, char *server_ip, int *server_port, 
                 char *sim_number, int *channel) {
    FILE *fp = fopen(config_file, "r");
    if (!fp) {
        printf("[JTT1078] Config file not found: %s\n", config_file);
        return -1;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;
        
        char key[64], value[192];
        if (sscanf(line, "%[^=]=%s", key, value) == 2) {
            if (strcmp(key, "SERVER_IP") == 0) {
                strcpy(server_ip, value);
            } else if (strcmp(key, "SERVER_PORT") == 0) {
                *server_port = atoi(value);
            } else if (strcmp(key, "SIM_NUMBER") == 0) {
                strcpy(sim_number, value);
            } else if (strcmp(key, "CHANNEL") == 0) {
                *channel = atoi(value);
            }
        }
    }
    
    fclose(fp);
    return 0;
}

int main(int argc, char *argv[]) {
    char server_ip[64] = "192.168.1.100";
    int server_port = 6605;
    char sim_number[32] = "123456789012";
    int channel = 1;
    
    printf("=== JT/T 1078 rkipc Integration ===\n");
    
    // Parse config if exists
    if (access("/userdata/jtt1078.conf", F_OK) == 0) {
        printf("[JTT1078] Loading config from /userdata/jtt1078.conf\n");
        parse_config("/userdata/jtt1078.conf", server_ip, &server_port, 
                     sim_number, &channel);
    }
    
    // Override with command line args if provided
    if (argc >= 2) strcpy(server_ip, argv[1]);
    if (argc >= 3) server_port = atoi(argv[2]);
    if (argc >= 4) strcpy(sim_number, argv[3]);
    if (argc >= 5) channel = atoi(argv[4]);
    
    printf("[JTT1078] Server: %s:%d\n", server_ip, server_port);
    printf("[JTT1078] SIM: %s, Channel: %d\n", sim_number, channel);
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Connect to JT/T 1078 server
    g_tcp_sock = connect_to_server(server_ip, server_port);
    if (g_tcp_sock < 0) {
        printf("[JTT1078] Failed to connect to server\n");
        return 1;
    }
    
    // Initialize JT/T 1078 encoder
    if (jtt1078_encoder_init(&g_encoder,
                             sim_number,
                             channel,
                             JTT1078_VIDEO_H265,
                             jtt1078_tcp_send,
                             NULL) != 0) {
        printf("[JTT1078] Failed to initialize encoder\n");
        close(g_tcp_sock);
        return 1;
    }
    
    printf("[JTT1078] Encoder initialized successfully\n");
    
    // TODO: Initialize rkipc video encoder
    /*
    RK_MPI_SYS_Init();
    RK_MPI_VENC_CreateChn(0, &venc_attr);
    RK_MPI_VENC_StartRecvFrame(0);
    */
    
    // Start streaming thread
    pthread_t stream_thread;
    pthread_create(&stream_thread, NULL, venc_stream_thread, NULL);
    
    // Main loop - keep alive
    printf("[JTT1078] Streaming started. Press Ctrl+C to stop.\n");
    while (g_running) {
        sleep(1);
        
        // Print statistics every 10 seconds
        static int counter = 0;
        if (++counter >= 10) {
            printf("[JTT1078] Sent packets: %u, RTP seq: %u\n",
                   g_encoder.packet_seq,
                   g_encoder.rtp_seq);
            counter = 0;
        }
    }
    
    // Cleanup
    printf("[JTT1078] Cleaning up...\n");
    pthread_join(stream_thread, NULL);
    
    // TODO: Cleanup rkipc
    /*
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    RK_MPI_SYS_Exit();
    */
    
    close(g_tcp_sock);
    printf("[JTT1078] Stopped\n");
    
    return 0;
}
