#include "esp32_camera.h"
#include "mcp_server.h"
#include "display.h"
#include "board.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <img_converters.h>
#include <cstring>

#define TAG "Esp32Camera"

Esp32Camera::Esp32Camera(const camera_config_t& config) : config_(config) {
    // 摄像头改为按需上电：构造时只保存配置，不调用 esp_camera_init。
    // 原因：OV3660 一旦初始化，esp32-camera 会创建优先级 configMAX_PRIORITIES-2(≈23)
    // 的 cam_task 持续占用 CPU/PSRAM，并让传感器/XCLK 一直耗电，直接把音频任务
    // (audio_input 优先级8 / audio_output 优先级6)饿死，表现为「有反应但喇叭没声音」。
    // 用户实测拆掉摄像头即恢复正常，证实冲突源头在此。
    // 现在只在真正拍照问答时(Capture)才初始化，拍完释放，兼顾两个功能。
    memset(&preview_image_, 0, sizeof(preview_image_));
}

bool Esp32Camera::EnsureInitialized() {
    if (initialized_) {
        return true;
    }

    esp_err_t err = esp_camera_init(&config_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != nullptr && s->id.PID == GC0308_PID) {
        s->set_hmirror(s, 0);
    }
    // 恢复之前保存的镜像/翻转设置
    SetHMirror(hmirror_);
    SetVFlip(vflip_);

    // 初始化预览图片的内存
    preview_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_image_.header.cf = LV_COLOR_FORMAT_RGB565;
    preview_image_.header.flags = LV_IMAGE_FLAGS_ALLOCATED | LV_IMAGE_FLAGS_MODIFIABLE;

    switch (config_.frame_size) {
        case FRAMESIZE_SVGA:
            preview_image_.header.w = 800;
            preview_image_.header.h = 600;
            break;
        case FRAMESIZE_VGA:
            preview_image_.header.w = 640;
            preview_image_.header.h = 480;
            break;
        case FRAMESIZE_QVGA:
            preview_image_.header.w = 320;
            preview_image_.header.h = 240;
            break;
        case FRAMESIZE_128X128:
            preview_image_.header.w = 128;
            preview_image_.header.h = 128;
            break;
        case FRAMESIZE_240X240:
            preview_image_.header.w = 240;
            preview_image_.header.h = 240;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported frame size: %d, image preview will not be shown", config_.frame_size);
            preview_image_.data_size = 0;
            preview_image_.data = nullptr;
            initialized_ = true;
            return true;
    }

    preview_image_.header.stride = preview_image_.header.w * 2;
    preview_image_.data_size = preview_image_.header.w * preview_image_.header.h * 2;
    preview_image_.data = (uint8_t*)heap_caps_malloc(preview_image_.data_size, MALLOC_CAP_SPIRAM);
    if (preview_image_.data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for preview image");
    }
    initialized_ = true;
    return true;
}

void Esp32Camera::Release() {
    if (!initialized_) {
        return;
    }
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    if (fb_) {
        esp_camera_fb_return(fb_);
        fb_ = nullptr;
    }
    if (preview_image_.data) {
        heap_caps_free((void*)preview_image_.data);
        preview_image_.data = nullptr;
        preview_image_.data_size = 0;
    }
    esp_camera_deinit();
    initialized_ = false;
    ESP_LOGI(TAG, "Camera deinitialized, audio resources released");
}

Esp32Camera::~Esp32Camera() {
    Release();
}

void Esp32Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    // 按需上电：拍照时才初始化摄像头，用完在 Explain 后释放
    if (!EnsureInitialized()) {
        return false;
    }
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    // 摄像头刚上电，OV3660 传感器需要时间稳定并输出第一帧。
    // 常驻方案下开机即初始化，用户拍照时早已稳定；按需方案下必须等待，
    // 否则 esp_camera_fb_get() 会超时导致「摄像头打开失败」。
    vTaskDelay(pdMS_TO_TICKS(300));

    int frames_to_get = 2;
    // Try to get a stable frame (带重试，进一步提高首帧成功率)
    for (int attempt = 0; attempt < 3; attempt++) {
        bool ok = true;
        for (int i = 0; i < frames_to_get; i++) {
            if (fb_ != nullptr) {
                esp_camera_fb_return(fb_);
                fb_ = nullptr;
            }
            fb_ = esp_camera_fb_get();
            if (fb_ == nullptr) {
                ok = false;
                break;
            }
        }
        if (ok) {
            break;
        }
        ESP_LOGW(TAG, "Camera capture attempt %d failed, retrying", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (fb_ == nullptr) {
        ESP_LOGE(TAG, "Camera capture failed after retries");
        return false;
    }

    // 如果预览图片 buffer 为空，则跳过预览
    // 但仍返回 true，因为此时图像可以上传至服务器
    if (preview_image_.data_size == 0) {
        ESP_LOGW(TAG, "Skip preview because of unsupported frame size");
        return true;
    }
    if (preview_image_.data == nullptr) {
        ESP_LOGE(TAG, "Preview image data is not initialized");
        return true;
    }
    // 显示预览图片
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        auto src = (uint16_t*)fb_->buf;
        auto dst = (uint16_t*)preview_image_.data;
        size_t pixel_count = fb_->len / 2;
        for (size_t i = 0; i < pixel_count; i++) {
            // 交换每个16位字内的字节
            dst[i] = __builtin_bswap16(src[i]);
        }
        display->SetPreviewImage(&preview_image_);
    }
    return true;
}
bool Esp32Camera::SetHMirror(bool enabled) {
    hmirror_ = enabled;  // 始终记录设置
    if (!initialized_) {
        return true;  // 未上电时只记录，等 EnsureInitialized 时应用
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return false;
    }

    esp_err_t err = s->set_hmirror(s, enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set horizontal mirror: %d", err);
        return false;
    }

    ESP_LOGI(TAG, "Camera horizontal mirror set to: %s", enabled ? "enabled" : "disabled");
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    vflip_ = enabled;  // 始终记录设置
    if (!initialized_) {
        return true;  // 未上电时只记录，等 EnsureInitialized 时应用
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return false;
    }

    esp_err_t err = s->set_vflip(s, enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set vertical flip: %d", err);
        return false;
    }

    ESP_LOGI(TAG, "Camera vertical flip set to: %s", enabled ? "enabled" : "disabled");
    return true;
}

/**
 * @brief 将摄像头捕获的图像发送到远程服务器进行AI分析和解释
 * 
 * 该函数将当前摄像头缓冲区中的图像编码为JPEG格式，并通过HTTP POST请求
 * 以multipart/form-data的形式发送到指定的解释服务器。服务器将根据提供的
 * 问题对图像进行AI分析并返回结果。
 * 
 * 实现特点：
 * - 使用独立线程编码JPEG，与主线程分离
 * - 采用分块传输编码(chunked transfer encoding)优化内存使用
 * - 通过队列机制实现编码线程和发送线程的数据同步
 * - 支持设备ID、客户端ID和认证令牌的HTTP头部配置
 * 
 * @param question 要向AI提出的关于图像的问题，将作为表单字段发送
 * @return std::string 服务器返回的JSON格式响应字符串
 *         成功时包含AI分析结果，失败时包含错误信息
 *         格式示例：{"success": true, "result": "分析结果"}
 *                  {"success": false, "message": "错误信息"}
 * 
 * @note 调用此函数前必须先调用SetExplainUrl()设置服务器URL
 * @note 函数会等待之前的编码线程完成后再开始新的处理
 * @warning 如果摄像头缓冲区为空或网络连接失败，将返回错误信息
 */
std::string Esp32Camera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        Release();
        return "{\"success\": false, \"message\": \"Image explain URL or token is not set\"}";
    }

    // 创建局部的 JPEG 队列, 40 entries is about to store 512 * 40 = 20480 bytes of JPEG data
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        Release();
        return "{\"success\": false, \"message\": \"Failed to create JPEG queue\"}";
    }

    // We spawn a thread to encode the image to JPEG
    encoder_thread_ = std::thread([this, jpeg_queue]() {
        frame2jpg_cb(fb_, 80, [](void* arg, size_t index, const void* data, size_t len) -> unsigned int {
            auto jpeg_queue = (QueueHandle_t)arg;
            JpegChunk chunk = {
                .data = (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM),
                .len = len
            };
            memcpy(chunk.data, data, len);
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
            return len;
        }, jpeg_queue);
    });

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    // 构造multipart/form-data请求体
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    // 配置HTTP客户端，使用分块传输编码
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        // Clear the queue
        encoder_thread_.join();
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        vQueueDelete(jpeg_queue);
        Release();
        return "{\"success\": false, \"message\": \"Failed to connect to explain URL\"}";
    }
    
    {
        // 第一块：question字段
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        // 第二块：文件字段头部
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    // 第三块：JPEG数据
    size_t total_sent = 0;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            break; // The last chunk
        }
        http->Write((const char*)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    // Wait for the encoder thread to finish
    encoder_thread_.join();
    // 清理队列
    vQueueDelete(jpeg_queue);

    {
        // 第四块：multipart尾部
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    // 结束块
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        http->Close();
        Release();
        return "{\"success\": false, \"message\": \"Failed to upload photo\"}";
    }

    std::string result = http->ReadAll();
    http->Close();

    // Get remain task stack size
    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
        fb_->width, fb_->height, total_sent, remain_stack_size, question.c_str(), result.c_str());

    // 拍照问答完成，立即释放摄像头，避免高优先级 cam_task 持续抢占音频资源
    Release();
    return result;
}
