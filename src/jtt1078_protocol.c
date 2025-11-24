/*
 * JT/T 1078 Protocol Implementation
 * 完整实现JT/T 1078视频通信协议
 */

#include "jtt1078_protocol.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <stdio.h>

// 字节序转换
uint16_t jtt1078_htons(uint16_t val) {
    return htons(val);
}

uint32_t jtt1078_htonl(uint32_t val) {
    return htonl(val);
}

uint64_t jtt1078_htonll(uint64_t val) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    return ((uint64_t)htonl(val & 0xFFFFFFFF) << 32) | htonl(val >> 32);
#else
    return val;
#endif
}

// 获取当前时间戳(毫秒)
uint64_t jtt1078_get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// SIM卡号字符串转BCD码
void jtt1078_sim_to_bcd(const char *sim_number, uint8_t *bcd_out) {
    memset(bcd_out, 0, 6);
    
    int len = strlen(sim_number);
    if (len > 12) len = 12;
    
    for (int i = 0; i < 6; i++) {
        int idx = i * 2;
        uint8_t high = (idx < len) ? (sim_number[idx] - '0') : 0;
        uint8_t low = (idx + 1 < len) ? (sim_number[idx + 1] - '0') : 0;
        bcd_out[i] = (high << 4) | low;
    }
}

// 初始化编码器
int jtt1078_encoder_init(jtt1078_encoder_t *encoder,
                         const char *sim_number,
                         uint8_t channel,
                         uint8_t video_format,
                         int (*send_callback)(const uint8_t *data, size_t len, void *user_data),
                         void *user_data) {
    if (!encoder || !sim_number || !send_callback) {
        return -1;
    }
    
    memset(encoder, 0, sizeof(jtt1078_encoder_t));
    
    // 设置基本参数
    strncpy(encoder->sim_number, sim_number, 12);
    encoder->channel = channel;
    encoder->video_format = video_format;
    encoder->audio_format = JTT1078_AUDIO_G711A;
    
    // 初始化序列号
    encoder->packet_seq = 0;
    encoder->rtp_seq = rand() & 0xFFFF;
    encoder->ssrc = rand();
    
    // 初始化时间戳
    encoder->start_time_ms = jtt1078_get_timestamp_ms();
    encoder->last_timestamp = 0;
    encoder->last_i_timestamp = 0;
    
    // 设置回调
    encoder->send_packet = send_callback;
    encoder->user_data = user_data;
    
    printf("[JTT1078] Encoder initialized: SIM=%s, Channel=%d, Format=%s\n",
           sim_number, channel, 
           video_format == JTT1078_VIDEO_H265 ? "H.265" : "H.264");
    
    return 0;
}

// 创建JT/T 1078数据包头
int jtt1078_create_packet(jtt1078_encoder_t *encoder,
                          jtt1078_packet_t *packet,
                          const uint8_t *data,
                          uint16_t data_len,
                          uint8_t data_type,
                          uint8_t subpackage) {
    if (!encoder || !packet || !data) {
        return -1;
    }
    
    if (data_len > JTT1078_MAX_PAYLOAD_SIZE) {
        fprintf(stderr, "[JTT1078] Payload too large: %d > %d\n", 
                data_len, JTT1078_MAX_PAYLOAD_SIZE);
        return -1;
    }
    
    memset(packet, 0, sizeof(jtt1078_packet_t));
    
    jtt1078_header_t *hdr = &packet->header;
    
    // 固定头标识 "01cd" (0x30316364)
    hdr->header_flag = jtt1078_htonl(JTT1078_HEADER_FLAG);
    
    // RTP版本信息
    hdr->v = 2;         // RTP version 2
    hdr->p = 0;         // 无填充
    hdr->x = 0;         // 无扩展
    hdr->cc = 0;        // 无CSRC
    
    // 负载类型
    if (encoder->video_format == JTT1078_VIDEO_H265) {
        hdr->pt = 98;   // H.265动态负载类型
    } else {
        hdr->pt = 96;   // H.264动态负载类型  
    }
    
    // 标记位(帧结束标志)
    hdr->m = (subpackage == JTT1078_PKT_ATOMIC || 
              subpackage == JTT1078_PKT_LAST) ? 1 : 0;
    
    // 包序号(循环递增)
    hdr->packet_seq = jtt1078_htons(encoder->packet_seq++);
    
    // SIM卡号(BCD码)
    jtt1078_sim_to_bcd(encoder->sim_number, hdr->sim);
    
    // 逻辑通道号
    hdr->channel = encoder->channel;
    
    // 数据类型
    hdr->data_type = data_type;
    
    // 分包标识
    hdr->subpackage = subpackage;
    hdr->reserved = 0;
    
    // 时间戳(相对于起始时间的毫秒数)
    uint64_t current_ts = jtt1078_get_timestamp_ms();
    uint64_t relative_ts = current_ts - encoder->start_time_ms;
    hdr->timestamp = jtt1078_htonll(relative_ts);
    
    // 计算帧间隔
    if (encoder->last_timestamp > 0) {
        encoder->frame_interval = (uint16_t)(relative_ts - encoder->last_timestamp);
    }
    
    if (data_type == JTT1078_DATA_TYPE_VIDEO && encoder->last_i_timestamp > 0) {
        encoder->i_frame_interval = (uint16_t)(relative_ts - encoder->last_i_timestamp);
    }
    
    // 更新时间戳记录
    encoder->last_timestamp = relative_ts;
    if (data_type == JTT1078_DATA_TYPE_VIDEO) {
        encoder->last_i_timestamp = relative_ts;
    }
    
    // 帧间隔信息
    hdr->last_i_frame_interval = jtt1078_htons(encoder->i_frame_interval);
    hdr->last_frame_interval = jtt1078_htons(encoder->frame_interval);
    
    // 数据体长度
    hdr->data_length = jtt1078_htons(data_len);
    
    // 拷贝负载数据
    memcpy(packet->payload, data, data_len);
    packet->payload_len = data_len;
    
    return 0;
}

// 发送数据包
int jtt1078_send_packet(jtt1078_encoder_t *encoder, const jtt1078_packet_t *packet) {
    if (!encoder || !packet || !encoder->send_packet) {
        return -1;
    }
    
    // 计算总长度
    size_t total_len = sizeof(jtt1078_header_t) + packet->payload_len;
    
    // 分配发送缓冲区
    uint8_t *send_buf = (uint8_t *)malloc(total_len);
    if (!send_buf) {
        fprintf(stderr, "[JTT1078] Failed to allocate send buffer\n");
        return -1;
    }
    
    // 拷贝头部和负载
    memcpy(send_buf, &packet->header, sizeof(jtt1078_header_t));
    memcpy(send_buf + sizeof(jtt1078_header_t), packet->payload, packet->payload_len);
    
    // 调用发送回调
    int ret = encoder->send_packet(send_buf, total_len, encoder->user_data);
    
    free(send_buf);
    
    if (ret < 0) {
        fprintf(stderr, "[JTT1078] Send failed\n");
        return -1;
    }
    
    return 0;
}

// 编码并发送视频帧
int jtt1078_encode_video_frame(jtt1078_encoder_t *encoder, const video_frame_t *frame) {
    if (!encoder || !frame || !frame->data) {
        return -1;
    }
    
    // 确定数据类型
    uint8_t data_type;
    if (frame->is_keyframe || frame->frame_type == JTT1078_DATA_TYPE_VIDEO) {
        data_type = JTT1078_DATA_TYPE_VIDEO;  // I帧
    } else if (frame->frame_type == JTT1078_DATA_TYPE_VIDEO_P) {
        data_type = JTT1078_DATA_TYPE_VIDEO_P; // P帧
    } else {
        data_type = JTT1078_DATA_TYPE_VIDEO_B; // B帧
    }
    
    uint32_t remaining = frame->size;
    uint32_t offset = 0;
    int packet_count = 0;
    
    // 计算需要分包的数量
    int total_packets = (remaining + JTT1078_MAX_PAYLOAD_SIZE - 1) / JTT1078_MAX_PAYLOAD_SIZE;
    
    printf("[JTT1078] Encoding video frame: type=%d, size=%u, packets=%d\n",
           data_type, frame->size, total_packets);
    
    while (remaining > 0) {
        uint16_t chunk_size = (remaining > JTT1078_MAX_PAYLOAD_SIZE) ? 
                              JTT1078_MAX_PAYLOAD_SIZE : remaining;
        
        // 确定分包标识
        uint8_t subpackage;
        if (total_packets == 1) {
            subpackage = JTT1078_PKT_ATOMIC;     // 不分包
        } else if (offset == 0) {
            subpackage = JTT1078_PKT_FIRST;      // 第一包
        } else if (chunk_size == remaining) {
            subpackage = JTT1078_PKT_LAST;       // 最后一包
        } else {
            subpackage = JTT1078_PKT_MIDDLE;     // 中间包
        }
        
        // 创建数据包
        jtt1078_packet_t packet;
        if (jtt1078_create_packet(encoder, &packet,
                                  frame->data + offset,
                                  chunk_size,
                                  data_type,
                                  subpackage) < 0) {
            fprintf(stderr, "[JTT1078] Failed to create packet\n");
            return -1;
        }
        
        // 发送数据包
        if (jtt1078_send_packet(encoder, &packet) < 0) {
            fprintf(stderr, "[JTT1078] Failed to send packet %d\n", packet_count);
            return -1;
        }
        
        offset += chunk_size;
        remaining -= chunk_size;
        packet_count++;
    }
    
    printf("[JTT1078] Video frame sent: %d packets\n", packet_count);
    return packet_count;
}

// 编码并发送音频帧
int jtt1078_encode_audio_frame(jtt1078_encoder_t *encoder, const audio_frame_t *frame) {
    if (!encoder || !frame || !frame->data) {
        return -1;
    }
    
    uint32_t remaining = frame->size;
    uint32_t offset = 0;
    int packet_count = 0;
    
    int total_packets = (remaining + JTT1078_MAX_PAYLOAD_SIZE - 1) / JTT1078_MAX_PAYLOAD_SIZE;
    
    while (remaining > 0) {
        uint16_t chunk_size = (remaining > JTT1078_MAX_PAYLOAD_SIZE) ? 
                              JTT1078_MAX_PAYLOAD_SIZE : remaining;
        
        // 音频一般不分包，或按相同逻辑分包
        uint8_t subpackage = (total_packets == 1) ? JTT1078_PKT_ATOMIC :
                            (offset == 0) ? JTT1078_PKT_FIRST :
                            (chunk_size == remaining) ? JTT1078_PKT_LAST :
                            JTT1078_PKT_MIDDLE;
        
        jtt1078_packet_t packet;
        if (jtt1078_create_packet(encoder, &packet,
                                  frame->data + offset,
                                  chunk_size,
                                  JTT1078_DATA_TYPE_AUDIO,
                                  subpackage) < 0) {
            return -1;
        }
        
        if (jtt1078_send_packet(encoder, &packet) < 0) {
            return -1;
        }
        
        offset += chunk_size;
        remaining -= chunk_size;
        packet_count++;
    }
    
    return packet_count;
}

// 打印数据包信息(调试用)
void jtt1078_print_packet_info(const jtt1078_packet_t *packet) {
    const jtt1078_header_t *hdr = &packet->header;
    
    printf("=== JT/T 1078 Packet ===\n");
    printf("Header Flag: 0x%08X\n", ntohl(hdr->header_flag));
    printf("Version: %d\n", hdr->v);
    printf("Payload Type: %d\n", hdr->pt);
    printf("Marker: %d\n", hdr->m);
    printf("Sequence: %d\n", ntohs(hdr->packet_seq));
    printf("Channel: %d\n", hdr->channel);
    printf("Data Type: %d\n", hdr->data_type);
    printf("Subpackage: %d\n", hdr->subpackage);
    printf("Timestamp: %llu ms\n", (unsigned long long)jtt1078_htonll(hdr->timestamp));
    printf("I-Frame Interval: %d ms\n", ntohs(hdr->last_i_frame_interval));
    printf("Frame Interval: %d ms\n", ntohs(hdr->last_frame_interval));
    printf("Data Length: %d bytes\n", ntohs(hdr->data_length));
    printf("Payload Length: %d bytes\n", packet->payload_len);
    printf("========================\n");
}
