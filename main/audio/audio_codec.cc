#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    int saved_volume = settings.GetInt("output_volume", -1);
    ESP_LOGI(TAG, "Loaded output_volume from NVS = %d", saved_volume);

    // NVS 里没有有效音量时（新设备 / 整片烧录后），使用正常可听的默认音量 70。
    // 之前 fallback 到 10：经 (10/100)^2=0.01 衰减后只有 1% 幅度，喇叭几乎无声，
    // 表现为「屏幕有反应、喊小智有反应，但完全听不到声音」。
    if (saved_volume <= 0) {
        output_volume_ = 70;
        Settings settings_w("audio", true);
        settings_w.SetInt("output_volume", output_volume_);
        ESP_LOGW(TAG, "No valid volume in NVS, set default to %d and saved", output_volume_);
    } else {
        output_volume_ = saved_volume;
    }
    ESP_LOGI(TAG, "Effective output volume = %d", output_volume_);

    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    }

    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    }

    EnableInput(true);
    EnableOutput(true);
    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
