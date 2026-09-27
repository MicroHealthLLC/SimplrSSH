/*
 * Audio Implementation
 * Mic: ES7210 (I2C 0x40) configured through esp_codec_dev, 16 kHz, two mics,
 * mixed down to mono. Speaker: plain I2S to the MAX98357A amplifier.
 */

#include "audio.hpp"
#include "utilities.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "AUDIO";

static i2s_chan_handle_t s_rx = NULL;
static i2s_chan_handle_t s_tx = NULL;
static const audio_codec_data_if_t* s_data_if = NULL;
static const audio_codec_ctrl_if_t* s_ctrl_if = NULL;
static const audio_codec_if_t* s_codec_if = NULL;
static esp_codec_dev_handle_t s_mic = NULL;

esp_err_t audio::mic_start()
{
    if (s_mic) {
        return ESP_OK;
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = BOARD_ES7210_MCLK;
    std_cfg.gpio_cfg.bclk = BOARD_ES7210_SCK;
    std_cfg.gpio_cfg.ws = BOARD_ES7210_LRCK;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = BOARD_ES7210_DIN;
    err = i2s_channel_init_std_mode(s_rx, &std_cfg);

    if (err == ESP_OK) {
        audio_codec_i2s_cfg_t i2s_cfg = {};
        i2s_cfg.port = I2S_NUM_0;
        i2s_cfg.rx_handle = s_rx;
        s_data_if = audio_codec_new_i2s_data(&i2s_cfg);

        audio_codec_i2c_cfg_t i2c_cfg = {};
        i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
        i2c_cfg.bus_handle = bsp_i2c_get_handle();
        s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

        es7210_codec_cfg_t es_cfg = {};
        es_cfg.ctrl_if = s_ctrl_if;
        es_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
        s_codec_if = s_ctrl_if ? es7210_codec_new(&es_cfg) : NULL;

        esp_codec_dev_cfg_t dev_cfg = {};
        dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
        dev_cfg.codec_if = s_codec_if;
        dev_cfg.data_if = s_data_if;
        s_mic = (s_codec_if && s_data_if) ? esp_codec_dev_new(&dev_cfg) : NULL;

        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 2;
        fs.sample_rate = MIC_RATE;
        if (!s_mic || esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
            err = ESP_FAIL;
        } else {
            esp_codec_dev_set_in_gain(s_mic, 30.0f);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Microphone init failed");
        mic_stop();
    }
    return err;
}

size_t audio::mic_read(int16_t* mono, size_t max_samples)
{
    if (!s_mic) {
        return 0;
    }
    // Read stereo frames in small blocks and average the two mics
    int16_t frames[256 * 2];
    size_t got = 0;
    while (got < max_samples) {
        size_t n = max_samples - got < 256 ? max_samples - got : 256;
        if (esp_codec_dev_read(s_mic, frames, n * 4) != ESP_CODEC_DEV_OK) {
            break;
        }
        for (size_t i = 0; i < n; i++) {
            mono[got + i] = (int16_t)(((int32_t)frames[2 * i] + frames[2 * i + 1]) / 2);
        }
        got += n;
    }
    return got;
}

void audio::mic_stop()
{
    if (s_mic) {
        esp_codec_dev_close(s_mic);
        esp_codec_dev_delete(s_mic);
        s_mic = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_rx) {
        i2s_del_channel(s_rx);  // The codec layer already disabled it
        s_rx = NULL;
    }
}

esp_err_t audio::speaker_start()
{
    if (s_tx) {
        return ESP_OK;
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = BOARD_I2S_BCK;
    std_cfg.gpio_cfg.ws = BOARD_I2S_WS;
    std_cfg.gpio_cfg.dout = BOARD_I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Speaker init failed");
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    return err;
}

esp_err_t audio::speaker_write(const void* pcm, size_t bytes)
{
    size_t written = 0;
    return s_tx ? i2s_channel_write(s_tx, pcm, bytes, &written, portMAX_DELAY) : ESP_ERR_INVALID_STATE;
}

void audio::speaker_stop()
{
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
}
