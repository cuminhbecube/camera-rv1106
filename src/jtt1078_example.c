/*
 * JT/T 1078 Example - Luckfox Pico Integration
 * 集成JT/T 1078协议到Luckfox视频流系统
 */

#include "jtt1078_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

// TCP连接上下文
typedef struct {
    int sockfd;
    struct sockaddr_in server_addr;
    bool connected;
    pthread_mutex_t send_mutex;
} tcp_context_t;

// 全局TCP上下文
static tcp_context_t g_tcp_ctx = {
    .sockfd = -1,
    .connected = false,
};

/**
 * TCP发送回调函数
 * JT/T 1078协议通过此函数发送RTP/TCP数据包
 */
int jtt1078_tcp_send_callback(const uint8_t *data, size_t len, void *user_data) {
    tcp_context_t *ctx = (tcp_context_t *)user_data;
    
    if (!ctx || ctx->sockfd < 0 || !ctx->connected) {
        fprintf(stderr, "[TCP] Not connected\n");
        return -1;
    }
    
    pthread_mutex_lock(&ctx->send_mutex);
    
    ssize_t sent = send(ctx->sockfd, data, len, 0);
    
    pthread_mutex_unlock(&ctx->send_mutex);
    
    if (sent < 0) {
        fprintf(stderr, "[TCP] Send failed: %s\n", strerror(errno));
        return -1;
    }
    
    if ((size_t)sent != len) {
        fprintf(stderr, "[TCP] Partial send: %zd/%zu bytes\n", sent, len);
        return -1;
    }
    
    return 0;
}

/**
 * 连接到JT/T 1078服务器
 */
int jtt1078_tcp_connect(const char *server_ip, uint16_t port) {
    if (g_tcp_ctx.connected) {
        printf("[TCP] Already connected\n");
        return 0;
    }
    
    // 创建socket
    g_tcp_ctx.sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_ctx.sockfd < 0) {
        fprintf(stderr, "[TCP] Failed to create socket: %s\n", strerror(errno));
        return -1;
    }
    
    // 设置socket选项
    int opt = 1;
    setsockopt(g_tcp_ctx.sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 设置发送缓冲区
    int sendbuf = 256 * 1024; // 256KB
    setsockopt(g_tcp_ctx.sockfd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));
    
    // 配置服务器地址
    memset(&g_tcp_ctx.server_addr, 0, sizeof(g_tcp_ctx.server_addr));
    g_tcp_ctx.server_addr.sin_family = AF_INET;
    g_tcp_ctx.server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, server_ip, &g_tcp_ctx.server_addr.sin_addr) <= 0) {
        fprintf(stderr, "[TCP] Invalid server IP: %s\n", server_ip);
        close(g_tcp_ctx.sockfd);
        g_tcp_ctx.sockfd = -1;
        return -1;
    }
    
    // 连接服务器
    printf("[TCP] Connecting to %s:%d...\n", server_ip, port);
    
    if (connect(g_tcp_ctx.sockfd, 
                (struct sockaddr *)&g_tcp_ctx.server_addr, 
                sizeof(g_tcp_ctx.server_addr)) < 0) {
        fprintf(stderr, "[TCP] Connection failed: %s\n", strerror(errno));
        close(g_tcp_ctx.sockfd);
        g_tcp_ctx.sockfd = -1;
        return -1;
    }
    
    pthread_mutex_init(&g_tcp_ctx.send_mutex, NULL);
    g_tcp_ctx.connected = true;
    
    printf("[TCP] Connected to %s:%d\n", server_ip, port);
    return 0;
}

/**
 * 断开TCP连接
 */
void jtt1078_tcp_disconnect(void) {
    if (g_tcp_ctx.sockfd >= 0) {
        close(g_tcp_ctx.sockfd);
        g_tcp_ctx.sockfd = -1;
    }
    
    g_tcp_ctx.connected = false;
    pthread_mutex_destroy(&g_tcp_ctx.send_mutex);
    
    printf("[TCP] Disconnected\n");
}

/**
 * 示例：从rkipc获取H.265视频流并通过JT/T 1078发送
 */
void *jtt1078_streaming_thread(void *arg) {
    jtt1078_encoder_t *encoder = (jtt1078_encoder_t *)arg;
    
    printf("[Streaming] Thread started\n");
    
    // 这里应该对接rkipc的视频输出
    // 示例代码展示如何发送一帧数据
    
    // 模拟H.265 NAL units (实际应从RK_MPI_VENC_GetStream获取)
    uint8_t sample_frame[] = {
        0x00, 0x00, 0x00, 0x01,  // Start code
        0x40, 0x01,              // VPS NAL header (H.265)
        // ... VPS data ...
    };
    
    while (1) {
        // 实际实现中，这里应该：
        // 1. 调用 RK_MPI_VENC_GetStream() 获取编码后的视频帧
        // 2. 判断帧类型(I/P/B帧)
        // 3. 提取NAL units
        // 4. 封装为video_frame_t
        // 5. 调用 jtt1078_encode_video_frame() 发送
        
        video_frame_t frame;
        frame.data = sample_frame;
        frame.size = sizeof(sample_frame);
        frame.frame_type = JTT1078_DATA_TYPE_VIDEO; // I帧
        frame.pts = jtt1078_get_timestamp_ms();
        frame.is_keyframe = true;
        
        // 发送视频帧
        int ret = jtt1078_encode_video_frame(encoder, &frame);
        if (ret < 0) {
            fprintf(stderr, "[Streaming] Failed to send frame\n");
            break;
        }
        
        // 模拟帧率 (实际应根据编码器输出)
        usleep(40000); // ~25fps
    }
    
    printf("[Streaming] Thread stopped\n");
    return NULL;
}

/**
 * 主函数示例
 */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <server_ip> <port> <sim_number> [channel]\n", argv[0]);
        printf("Example: %s 192.168.1.100 6605 123456789012 1\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    uint16_t port = atoi(argv[2]);
    const char *sim_number = argv[3];
    uint8_t channel = (argc > 4) ? atoi(argv[4]) : 1;
    
    printf("=== JT/T 1078 Video Streaming ===\n");
    printf("Server: %s:%d\n", server_ip, port);
    printf("SIM: %s\n", sim_number);
    printf("Channel: %d\n", channel);
    printf("===================================\n\n");
    
    // 1. 连接服务器
    if (jtt1078_tcp_connect(server_ip, port) < 0) {
        fprintf(stderr, "Failed to connect to server\n");
        return 1;
    }
    
    // 2. 初始化JT/T 1078编码器
    jtt1078_encoder_t encoder;
    if (jtt1078_encoder_init(&encoder,
                             sim_number,
                             channel,
                             JTT1078_VIDEO_H265,
                             jtt1078_tcp_send_callback,
                             &g_tcp_ctx) < 0) {
        fprintf(stderr, "Failed to initialize encoder\n");
        jtt1078_tcp_disconnect();
        return 1;
    }
    
    // 3. 启动流媒体线程
    pthread_t streaming_tid;
    pthread_create(&streaming_tid, NULL, jtt1078_streaming_thread, &encoder);
    
    // 4. 等待用户中断
    printf("\nPress Ctrl+C to stop...\n\n");
    pthread_join(streaming_tid, NULL);
    
    // 5. 清理
    jtt1078_tcp_disconnect();
    
    printf("Program terminated\n");
    return 0;
}

/*
 * 编译命令:
 * arm-rockchip830-linux-uclibcgnueabihf-gcc \
 *     -o jtt1078_example \
 *     jtt1078_protocol.c \
 *     jtt1078_example.c \
 *     -lpthread -I.
 * 
 * 实际集成到rkipc的步骤:
 * 
 * 1. 在 rkipc 的视频编码回调中获取H.265帧:
 *    - 使用 RK_MPI_VENC_GetStream() 获取编码帧
 *    - 提取 VENC_PACK_S 结构中的数据
 * 
 * 2. 判断帧类型:
 *    - pstPack->DataType.enH265EType 判断NAL类型
 *    - IDR/I帧、P帧、B帧分类
 * 
 * 3. 封装为 video_frame_t:
 *    frame.data = pstPack->pu8Addr;
 *    frame.size = pstPack->u32Len;
 *    frame.is_keyframe = (NAL类型为IDR/VPS/SPS/PPS);
 *    frame.pts = pstStream->pstPack[i].u64PTS;
 * 
 * 4. 发送:
 *    jtt1078_encode_video_frame(&g_encoder, &frame);
 * 
 * 5. 释放帧:
 *    RK_MPI_VENC_ReleaseStream();
 */
