#ifndef M4A_DECODER_H
#define M4A_DECODER_H

#include <vector>
#include <cstdint>
#include <memory>

// ESP-ADF includes
#include "audio_element.h"
#include "audio_pipeline.h"
#include "codec/mp3_decoder.h"
#include "codec/aac_decoder.h"

// Forward declaration
class OpusResampler;

// 音频格式枚举：根据 URL 后缀自动识别
enum class AudioFormat {
    MP3,   // .mp3
    AAC,   // .m4a / .aac / .mp4
};

class M4aDecoder {
public:
    M4aDecoder();
    ~M4aDecoder();

    bool Initialize(int sample_rate, int channels);
    void Deinitialize();
    
    bool PlayUrl(const std::string& url);
    void Stop();
    
    std::vector<int16_t> DecodeChunk(const std::vector<uint8_t>& m4a_data);
    std::vector<int16_t> GetPcmData(); // 从M4A解码器获取PCM数据
    
    // 设置重采样器
    void SetResampler(int input_sample_rate, int output_sample_rate);
    
    // 获取解码器元素句柄
    audio_element_handle_t GetDecoderElement() const { return m4a_decoder_; }
    
    // 判断解码是否已结束（播放完成或出错），用于让消费任务退出循环
    bool IsFinished();
    
    // 判断数据流是否已完全结束（解码器进入 FINISHED 状态 或 ERROR）
    // 消费任务应在此返回 true 且 ringbuffer 已读空时才退出，确保整首歌播完
    bool IsStreamEnded();
    
    // 判断解码器是否已进入结束状态（FINISHED/ERROR，而非 RUNNING）
    bool HasReachedEndState();

private:
    // 根据 URL 后缀自动识别音频格式（.m4a/.aac/.mp4 → AAC，.mp3 → MP3）
    AudioFormat DetectFormat(const std::string& url);
    
    // 根据格式创建对应的解码器元素，并注册/链接到管道
    bool CreateDecoderForFormat(AudioFormat fmt);
    
    bool initialized_;
    int sample_rate_;
    int channels_;
    
    // 当前使用的解码格式
    AudioFormat format_;
    
    // ESP-ADF音频管道组件
    audio_pipeline_handle_t pipeline_;
    audio_element_handle_t m4a_decoder_;
    audio_element_handle_t http_stream_;
    audio_element_handle_t i2s_stream_;
    
    // M4A数据缓冲区
    std::vector<uint8_t> m4a_buffer_;
    
    // 输出缓冲区
    std::vector<int16_t> output_buffer_;
    
    // ESP-ADF音频缓冲区
    ringbuf_handle_t output_ringbuf_;
    
    // 数据流结束标志：当 rb_read 返回 RB_DONE（解码器写完且 buffer 读空）时置位
    // 这是最可靠的"整首歌已读完"信号，比状态机判断更准确
    volatile bool stream_done_;
    
    // 重采样器
    std::unique_ptr<OpusResampler> resampler_;
    
    static const char* TAG;
};

#endif // M4A_DECODER_H
