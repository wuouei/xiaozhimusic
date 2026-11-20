#include "m4a_decoder.h"
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include "opus_resampler.h"
#include <cstring>

// ESP-ADF includes
#include "http_stream.h"
#include "i2s_stream.h"
#include "codec/mp3_decoder.h"
#include "codec/aac_decoder.h"
#include <algorithm>

const char* M4aDecoder::TAG = "M4aDecoder";

M4aDecoder::M4aDecoder() 
    : initialized_(false)
    , sample_rate_(44100)
    , channels_(2)
    , format_(AudioFormat::MP3)
    , pipeline_(nullptr)
    , m4a_decoder_(nullptr)
    , http_stream_(nullptr)
    , i2s_stream_(nullptr)
    , output_ringbuf_(nullptr)
    , stream_done_(false) {
}

M4aDecoder::~M4aDecoder() {
    Deinitialize();
}

// 根据 URL 后缀自动识别音频格式
// .m4a / .aac / .mp4 → AAC；.mp3 及其他 → MP3
AudioFormat M4aDecoder::DetectFormat(const std::string& url) {
    // 提取 URL 中不含 query 参数的路径部分（截断到 '?' 之前）
    std::string path = url;
    size_t qpos = path.find('?');
    if (qpos != std::string::npos) {
        path = path.substr(0, qpos);
    }
    
    // 转小写，便于匹配后缀
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower.find(".m4a") != std::string::npos ||
        lower.find(".aac") != std::string::npos ||
        lower.find(".mp4") != std::string::npos) {
        return AudioFormat::AAC;
    }
    return AudioFormat::MP3;
}

// 根据格式创建对应的解码器元素，注册并链接到管道
bool M4aDecoder::CreateDecoderForFormat(AudioFormat fmt) {
    // 如果已有解码器，先注销并销毁
    if (m4a_decoder_ != nullptr) {
        audio_pipeline_unregister(pipeline_, m4a_decoder_);
        audio_element_deinit(m4a_decoder_);
        m4a_decoder_ = nullptr;
    }
    
    if (fmt == AudioFormat::AAC) {
        aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
        // 提高输出 ringbuffer 容量，避免 AAC 大码率下数据堆积
        aac_cfg.out_rb_size = 64 * 1024;
        m4a_decoder_ = aac_decoder_init(&aac_cfg);
        if (m4a_decoder_ == nullptr) {
            ESP_LOGE(TAG, "Failed to initialize AAC decoder");
            return false;
        }
        ESP_LOGI(TAG, "Created AAC decoder for .m4a format");
    } else {
        mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
        m4a_decoder_ = mp3_decoder_init(&mp3_cfg);
        if (m4a_decoder_ == nullptr) {
            ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
            return false;
        }
        ESP_LOGI(TAG, "Created MP3 decoder for .mp3 format");
    }
    
    // 将输出缓冲区连接到解码器（复用已有的 output_ringbuf_）
    audio_element_set_output_ringbuf(m4a_decoder_, output_ringbuf_);
    
    // 注册解码器元素到管道
    audio_pipeline_register(pipeline_, m4a_decoder_, "m4a");
    
    // 链接管道元素：http -> m4a
    const char *link_tag[2] = {"http", "m4a"};
    if (audio_pipeline_link(pipeline_, &link_tag[0], 2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link audio pipeline elements");
        audio_element_deinit(m4a_decoder_);
        m4a_decoder_ = nullptr;
        return false;
    }
    
    format_ = fmt;
    return true;
}

bool M4aDecoder::Initialize(int sample_rate, int channels) {
    if (initialized_) {
        ESP_LOGW(TAG, "M4A decoder already initialized");
        return true;
    }
    
    sample_rate_ = sample_rate;
    channels_ = channels;
    
    ESP_LOGI(TAG, "Initializing ESP-ADF audio decoder pipeline: sample_rate=%d, channels=%d", sample_rate_, channels_);
    
    // 创建音频管道
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_ = audio_pipeline_init(&pipeline_cfg);
    if (pipeline_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize audio pipeline");
        return false;
    }
    
    // 创建HTTP流（用于接收音频数据）
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    // 增大 HTTP 输出缓冲（默认仅 20KB）到 48KB，给网络抖动/短暂断流更多余量，
    // 配合"播放期间关闭 WiFi 省电"，降低在线音乐播不完的概率。
    // ringbuf 走 PSRAM（SPIRAM_MALLOC_ALWAYSINTERNAL=0），不占用紧张的内部 SRAM。
    http_cfg.out_rb_size = 64 * 1024;  // 320kbps 高码率需比原 48KB 更大缓冲，但不宜过大占用内存
    // 关键修复：ESP-ADF 的 http_stream 默认 crt_bundle_attach=NULL，
    // 遇到 https:// 音频直链（网易云 m701.music.126.net 等）时 esp_tls 报
    // "No server verification option set ... ESP_ERR_MBEDTLS_SSL_SETUP_FAILED"，
    // 导致 HTTP 连接失败、无音频数据。挂上证书 bundle（项目已启用
    // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE）即可正常验证服务器证书。
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_stream_ = http_stream_init(&http_cfg);
    if (http_stream_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTP stream");
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 创建一个输出环形缓冲区来存储解码后的PCM数据
    output_ringbuf_ = rb_create(65536, 1);  // 64KB缓冲区，充分缓冲44.1kHz音频，避免数据流中断导致杂音/中断
    if (!output_ringbuf_) {
        ESP_LOGE(TAG, "Failed to create output ring buffer");
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 注册 HTTP 流到管道（解码器在 PlayUrl 时根据 URL 格式动态创建）
    audio_pipeline_register(pipeline_, http_stream_, "http");
    
    i2s_stream_ = nullptr;
    m4a_decoder_ = nullptr;  // 解码器延迟到 PlayUrl 时创建
    
    // 清空缓冲区
    m4a_buffer_.clear();
    output_buffer_.clear();
    stream_done_ = false;
    format_ = AudioFormat::MP3;
    
    initialized_ = true;
    ESP_LOGI(TAG, "ESP-ADF audio pipeline initialized (decoder created on PlayUrl)");
    return true;
}

bool M4aDecoder::PlayUrl(const std::string& url) {
    if (!initialized_) {
        ESP_LOGE(TAG, "M4A decoder not initialized");
        return false;
    }
    
    if (!http_stream_) {
        ESP_LOGE(TAG, "HTTP stream is null");
        return false;
    }
    
    if (!pipeline_) {
        ESP_LOGE(TAG, "Audio pipeline is null");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting audio playback from URL: %s", url.c_str());
    
    // 关键修复：根据 URL 后缀自动识别格式，动态创建对应的解码器（AAC 或 MP3）
    // 音乐 API 返回的格式不固定：有的歌是 .mp3，有的歌是 .m4a（AAC）
    // 之前硬编码 MP3 解码器，遇到 .m4a 就报 "Not find an effective frame" 直接失败
    AudioFormat fmt = DetectFormat(url);
    ESP_LOGI(TAG, "Detected audio format: %s", (fmt == AudioFormat::AAC) ? "AAC (.m4a)" : "MP3 (.mp3)");
    
    // 重置流结束标志（新的一首歌开始）
    stream_done_ = false;
    
    if (!CreateDecoderForFormat(fmt)) {
        ESP_LOGE(TAG, "Failed to create decoder for format %s", (fmt == AudioFormat::AAC) ? "AAC" : "MP3");
        return false;
    }
    
    // 设置HTTP流的URL
    esp_err_t ret = audio_element_set_uri(http_stream_, url.c_str());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set URI for HTTP stream, error: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "URI set successfully, starting pipeline");
    
    // 启动音频管道（只包含HTTP和解码器）
    ret = audio_pipeline_run(pipeline_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio pipeline, error: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "Audio playback started successfully (without I2S output)");
    // 等待让解码器完全启动并开始处理数据
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI(TAG, "Audio decoder pipeline started, ready for data processing");
    
    // 暂时禁用重采样器，直接使用原始采样率
    resampler_.reset();
    
    return true;
}

void M4aDecoder::Stop() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping M4A playback");
    
    // 停止音频管道
    if (audio_pipeline_stop(pipeline_) == ESP_OK) {
        audio_pipeline_wait_for_stop(pipeline_);
    }
    
    ESP_LOGI(TAG, "M4A playback stopped");
}

void M4aDecoder::Deinitialize() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing ESP-ADF M4A decoder pipeline");
    
    // 停止管道
    if (pipeline_) {
        audio_pipeline_stop(pipeline_);
        audio_pipeline_wait_for_stop(pipeline_);
        audio_pipeline_terminate(pipeline_);
        
        // 注销所有元素（不包含I2S流；m4a_decoder_ 可能为 null，因为解码器是 PlayUrl 时才创建）
        if (http_stream_) {
            audio_pipeline_unregister(pipeline_, http_stream_);
        }
        if (m4a_decoder_) {
            audio_pipeline_unregister(pipeline_, m4a_decoder_);
        }
        
        // 销毁所有元素（不包含I2S流）
        if (m4a_decoder_) {
            audio_element_deinit(m4a_decoder_);
            m4a_decoder_ = nullptr;
        }
        if (http_stream_) {
            audio_element_deinit(http_stream_);
            http_stream_ = nullptr;
        }
        if (output_ringbuf_) {
            rb_destroy(output_ringbuf_);
            output_ringbuf_ = nullptr;
        }
        
        // 销毁管道
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
    }
    
    m4a_buffer_.clear();
    output_buffer_.clear();
    initialized_ = false;
    
    ESP_LOGI(TAG, "ESP-ADF M4A decoder pipeline deinitialized");
}

std::vector<int16_t> M4aDecoder::DecodeChunk(const std::vector<uint8_t>& m4a_data) {
    std::vector<int16_t> pcm_data;
    
    if (!initialized_ || !m4a_decoder_) {
        ESP_LOGE(TAG, "M4A decoder not initialized");
        return pcm_data;
    }
    
    if (m4a_data.empty()) {
        return pcm_data;
    }
    
    ESP_LOGI(TAG, "DecodeChunk called with %d bytes of M4A data", m4a_data.size());
    
    ringbuf_handle_t output_rb = audio_element_get_output_ringbuf(m4a_decoder_);
    if (!output_rb) {
        ESP_LOGI(TAG, "Failed to get output ringbuf from M4A decoder");
        return pcm_data;
    }
    
    char output_buffer[4096];
    int bytes_read = rb_read(output_rb, output_buffer, sizeof(output_buffer), 0); // 非阻塞读取
    if (bytes_read > 0) {
        // 将字节数据转换为int16_t PCM数据
        int16_t* pcm_samples = reinterpret_cast<int16_t*>(output_buffer);
        int sample_count = bytes_read / sizeof(int16_t);
        
        pcm_data.resize(sample_count);
        std::memcpy(pcm_data.data(), pcm_samples, bytes_read);
        
        ESP_LOGI(TAG, "M4A decode: got %d PCM samples from decoder", pcm_data.size());
    } else {
        ESP_LOGI(TAG, "M4A decode: no PCM data available (bytes_read=%d)", bytes_read);
    }
    return pcm_data;
}

// 判断解码是否已出错（仅错误时返回 true，播放完成不提前退出，让数据自然读完）
bool M4aDecoder::IsFinished() {
    if (!initialized_ || !m4a_decoder_) {
        return true;
    }
    
    audio_element_state_t state = audio_element_get_state(m4a_decoder_);
    // 仅在明确错误时退出；FINISHED 状态交给消费端在数据读完后自然结束，
    // 避免 HTTP 流下载缓冲阶段状态短暂波动导致提前退出、播不完整首
    if (state == AEL_STATE_ERROR) {
        return true;
    }
    return false;
}

// 判断解码器是否已进入结束状态（FINISHED 或 ERROR，而非 RUNNING/INIT）
// 用于"确定性结束"判定：一旦解码器明确结束，配合 ringbuffer 读空即可安全退出
bool M4aDecoder::HasReachedEndState() {
    if (!initialized_ || !m4a_decoder_) {
        return true;
    }
    audio_element_state_t state = audio_element_get_state(m4a_decoder_);
    return (state == AEL_STATE_FINISHED || state == AEL_STATE_ERROR);
}

// 判断数据流是否已完全结束：解码器进入结束状态，且输出 ringbuffer 已排空
// 优先使用 RB_DONE 信号（最可靠），否则回退到状态机 + ringbuffer 空判定
bool M4aDecoder::IsStreamEnded() {
    if (!initialized_ || !m4a_decoder_) {
        return true;
    }
    
    // 最高优先级：rb_read 已返回 RB_DONE，说明解码器写完且 buffer 读空
    if (stream_done_) {
        return true;
    }
    
    audio_element_state_t state = audio_element_get_state(m4a_decoder_);
    if (state != AEL_STATE_FINISHED && state != AEL_STATE_ERROR) {
        return false;  // 解码器仍在运行，数据流未结束
    }
    
    // 解码器已结束，检查 ringbuffer 是否还有残留数据
    if (output_ringbuf_) {
        int filled = rb_bytes_filled(output_ringbuf_);
        if (filled > 0) {
            return false;  // 还有数据待消费，不能退出
        }
    }
    
    return true;
}

// 新增方法：从M4A解码器获取PCM数据
std::vector<int16_t> M4aDecoder::GetPcmData() {
    std::vector<int16_t> pcm_data;
    
    if (!initialized_ || !output_ringbuf_) {
        return pcm_data;
    }
    
    // 从我们创建的输出缓冲区读取PCM数据，控制数据量
    // 读取 4096 字节（1024 个 stereo 帧 = 4 字节对齐），保证左右声道不错位
    char output_buffer[4096];
    int bytes_read = rb_read(output_ringbuf_, output_buffer, sizeof(output_buffer), 0); // 非阻塞读取
    ESP_LOGD(TAG, "rb_read returned: %d bytes", bytes_read);
    
    // 捕获 RB_DONE 信号：解码器写完且 ringbuffer 已读空，这是最可靠的"流结束"信号
    if (bytes_read == RB_DONE) {
        stream_done_ = true;
        ESP_LOGI(TAG, "Got RB_DONE from ring buffer, stream fully drained");
        return pcm_data;  // 返回空，由 IsStreamEnded() 判定结束
    }
    
    if (bytes_read > 0) {
        // 关键修复：将字节数向下对齐到 4 的倍数（stereo 帧 = 2 通道 x 2 字节 = 4 字节），
        // 避免跨 chunk 的左右声道错位（这是杂音的重要来源）
        int aligned_bytes = bytes_read & ~3;  // 向下取整到 4 的倍数
        if (aligned_bytes <= 0) {
            return pcm_data;  // 不足一帧，返回空，等下次
        }
        
        // 将字节数据转换为int16_t PCM数据
        int16_t* pcm_samples = reinterpret_cast<int16_t*>(output_buffer);
        int sample_count = aligned_bytes / sizeof(int16_t);
        
        // 安全检查，避免过大的内存分配，平衡样本数量
        if (sample_count > 0 && sample_count < 4096) {
            std::vector<int16_t> raw_pcm_data(sample_count);
            std::memcpy(raw_pcm_data.data(), pcm_samples, aligned_bytes);
            
            // 直接返回原始PCM数据，重采样由缓冲区管理任务处理
            pcm_data = std::move(raw_pcm_data);
            ESP_LOGD(TAG, "Got %d raw PCM samples from M4A decoder", pcm_data.size());
        } else {
            ESP_LOGI(TAG, "Invalid sample count: %d, skipping this chunk", sample_count);
        }
    } else if (bytes_read < 0) {
        // RB_TIMEOUT：ringbuffer 暂时空（下载缓冲中），这是正常现象，不视为结束
        // ESP_LOGW(TAG, "rb_read timeout: ring buffer temporarily empty");
    }
    
    return pcm_data;
}

// 设置重采样器
void M4aDecoder::SetResampler(int input_sample_rate, int output_sample_rate) {
    if (input_sample_rate != output_sample_rate) {
        ESP_LOGI(TAG, "Setting up resampler: %d Hz -> %d Hz", input_sample_rate, output_sample_rate);
        // 如果需要重采样，可以在这里实现
        // 目前支持标准44.1kHz输出
    } else {
        ESP_LOGI(TAG, "No resampling needed: using %d Hz (standard 44.1kHz)", input_sample_rate);
        resampler_.reset();
    }
}
