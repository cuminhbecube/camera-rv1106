/*
 * JT/T 1078 Protocol Implementation
 * 道路运输车载视频通信协议 (Road Transport Vehicle Video Communication Protocol)
 * 
 * Specification: JT/T 1078-2016
 * RTP over TCP packaging for H.264/H.265 video streams
 */

#ifndef JTT1078_PROTOCOL_H
#define JTT1078_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// JT/T 1078 固定头标识
#define JTT1078_HEADER_FLAG         0x30316364  // "01cd" 

// 数据类型定义
#define JTT1078_DATA_TYPE_VIDEO     0x00        // 视频 I 帧
#define JTT1078_DATA_TYPE_VIDEO_P   0x01        // 视频 P 帧  
#define JTT1078_DATA_TYPE_VIDEO_B   0x02        // 视频 B 帧
#define JTT1078_DATA_TYPE_AUDIO     0x03        // 音频帧
#define JTT1078_DATA_TYPE_TRANS     0x04        // 透传数据

// 分包处理标识
#define JTT1078_PKT_ATOMIC          0x00        // 原子包（不分包）
#define JTT1078_PKT_FIRST           0x01        // 分包第一包
#define JTT1078_PKT_LAST            0x02        // 分包最后一包  
#define JTT1078_PKT_MIDDLE          0x03        // 分包中间包

// 视频编码格式
#define JTT1078_VIDEO_H264          0x01        // H.264
#define JTT1078_VIDEO_H265          0x02        // H.265/HEVC

// 音频编码格式
#define JTT1078_AUDIO_G711A         0x01        // G.711A
#define JTT1078_AUDIO_G711U         0x02        // G.711μ
#define JTT1078_AUDIO_AAC           0x13        // AAC

// 最大包大小定义
#define JTT1078_MAX_PACKET_SIZE     950         // TCP MTU考虑
#define JTT1078_HEADER_SIZE         30          // 固定头长度
#define JTT1078_MAX_PAYLOAD_SIZE    (JTT1078_MAX_PACKET_SIZE - JTT1078_HEADER_SIZE)

// RTP 固定头部
typedef struct {
    uint8_t  cc:4;          // CSRC计数
    uint8_t  x:1;           // 扩展位
    uint8_t  p:1;           // 填充位
    uint8_t  v:2;           // 版本(固定为2)
    
    uint8_t  pt:7;          // 负载类型
    uint8_t  m:1;           // 标记位
    
    uint16_t seq;           // 序列号
    uint32_t timestamp;     // 时间戳
    uint32_t ssrc;          // 同步源标识符
} __attribute__((packed)) rtp_header_t;

// JT/T 1078 扩展头部
typedef struct {
    uint32_t header_flag;   // 固定头标识 0x30316364
    
    uint8_t  v:2;           // 版本号
    uint8_t  p:1;           // 填充标志
    uint8_t  x:1;           // 扩展标志
    uint8_t  cc:4;          // CSRC计数
    
    uint8_t  pt:7;          // 负载类型
    uint8_t  m:1;           // 标记位
    
    uint16_t packet_seq;    // 包序号
    
    uint8_t  sim[6];        // SIM卡号(BCD[6])
    uint8_t  channel;       // 逻辑通道号
    
    uint8_t  data_type;     // 数据类型
    uint8_t  subpackage:2;  // 分包处理标识
    uint8_t  reserved:6;    // 保留位
    
    uint64_t timestamp;     // 时间戳(8字节)
    
    uint16_t last_i_frame_interval;     // 与上一个I帧的间隔
    uint16_t last_frame_interval;       // 与上一帧的间隔
    uint16_t data_length;               // 数据体长度
} __attribute__((packed)) jtt1078_header_t;

// 完整的JT/T 1078数据包
typedef struct {
    jtt1078_header_t header;
    uint8_t payload[JTT1078_MAX_PAYLOAD_SIZE];
    uint16_t payload_len;
} jtt1078_packet_t;

// 编码器上下文
typedef struct {
    // 基本参数
    char sim_number[13];        // SIM卡号(字符串)
    uint8_t channel;            // 通道号
    uint8_t video_format;       // 视频编码格式
    uint8_t audio_format;       // 音频编码格式
    
    // 序列号管理
    uint16_t packet_seq;        // 包序号
    uint32_t rtp_seq;           // RTP序列号
    uint32_t ssrc;              // RTP SSRC
    
    // 时间戳管理
    uint64_t start_time_ms;     // 起始时间(毫秒)
    uint64_t last_timestamp;    // 上一帧时间戳
    uint64_t last_i_timestamp;  // 上一个I帧时间戳
    
    // 帧间隔统计
    uint16_t frame_interval;    // 当前帧间隔(ms)
    uint16_t i_frame_interval;  // I帧间隔(ms)
    
    // 回调函数
    int (*send_packet)(const uint8_t *data, size_t len, void *user_data);
    void *user_data;
    
} jtt1078_encoder_t;

// 视频帧信息
typedef struct {
    uint8_t  *data;             // 帧数据
    uint32_t size;              // 数据大小
    uint8_t  frame_type;        // 帧类型(I/P/B)
    uint64_t pts;               // 显示时间戳(毫秒)
    bool     is_keyframe;       // 是否关键帧
} video_frame_t;

// 音频帧信息
typedef struct {
    uint8_t  *data;             // 音频数据
    uint32_t size;              // 数据大小
    uint64_t pts;               // 时间戳(毫秒)
} audio_frame_t;

/*
 * API Functions
 */

/**
 * 初始化JT/T 1078编码器
 * @param encoder 编码器上下文
 * @param sim_number SIM卡号(12位字符串)
 * @param channel 逻辑通道号(0-127)
 * @param video_format 视频编码格式
 * @param send_callback 发送回调函数
 * @param user_data 用户数据指针
 * @return 0成功, -1失败
 */
int jtt1078_encoder_init(jtt1078_encoder_t *encoder,
                         const char *sim_number,
                         uint8_t channel,
                         uint8_t video_format,
                         int (*send_callback)(const uint8_t *data, size_t len, void *user_data),
                         void *user_data);

/**
 * 编码并发送视频帧
 * @param encoder 编码器上下文
 * @param frame 视频帧数据
 * @return 发送的包数量, <0表示失败
 */
int jtt1078_encode_video_frame(jtt1078_encoder_t *encoder, const video_frame_t *frame);

/**
 * 编码并发送音频帧  
 * @param encoder 编码器上下文
 * @param frame 音频帧数据
 * @return 发送的包数量, <0表示失败
 */
int jtt1078_encode_audio_frame(jtt1078_encoder_t *encoder, const audio_frame_t *frame);

/**
 * 创建JT/T 1078数据包
 * @param encoder 编码器上下文
 * @param packet 输出数据包
 * @param data 负载数据
 * @param data_len 数据长度
 * @param data_type 数据类型
 * @param subpackage 分包标识
 * @return 0成功, -1失败
 */
int jtt1078_create_packet(jtt1078_encoder_t *encoder,
                          jtt1078_packet_t *packet,
                          const uint8_t *data,
                          uint16_t data_len,
                          uint8_t data_type,
                          uint8_t subpackage);

/**
 * 发送JT/T 1078数据包
 * @param encoder 编码器上下文  
 * @param packet 数据包
 * @return 0成功, -1失败
 */
int jtt1078_send_packet(jtt1078_encoder_t *encoder, const jtt1078_packet_t *packet);

/**
 * 将SIM卡号字符串转换为BCD码
 * @param sim_str SIM卡号字符串(12位数字)
 * @param bcd_out 输出BCD码缓冲区(6字节)
 */
void jtt1078_sim_to_bcd(const char *sim_number, uint8_t *bcd_out);

/**
 * 获取当前时间戳(毫秒)
 * @return 时间戳
 */
uint64_t jtt1078_get_timestamp_ms(void);

/**
 * 字节序转换辅助函数
 */
uint16_t jtt1078_htons(uint16_t val);
uint32_t jtt1078_htonl(uint32_t val);
uint64_t jtt1078_htonll(uint64_t val);

#endif // JTT1078_PROTOCOL_H
