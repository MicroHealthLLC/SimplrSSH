/*
 * Audio: Tab5
 * Mic: ES7210 (I2C 0x40), 16 kHz 16-bit mono I2S: the left slot carries MIC1, the Tab5's
 * front microphone. Speaker: ES8388 codec (I2C 0x10) and the NS4150B amplifier, switched on
 * through IO expander 0x43 only while playing. Both share one set of I2S pins (MCLK 30,
 * BCLK 27, WS 29, DOUT 26, DIN 28) and are never used at the same time; each is created
 * when it starts and freed when it stops, so audio costs no RAM otherwise.
 */

#include "audio.hpp"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "AUDIO";

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static const float MIC_GAIN_DB = 30.0f;
static const int SPEAKER_VOLUME = 70;   // 0-100

// One codec device (mic or speaker) with the interfaces it was built from
struct CodecDevice {
    i2s_chan_handle_t chan = NULL;
    const audio_codec_data_if_t* data_if = NULL;
    const audio_codec_ctrl_if_t* ctrl_if = NULL;
    const audio_codec_gpio_if_t* gpio_if = NULL;
    const audio_codec_if_t* codec_if = NULL;
    esp_codec_dev_handle_t dev = NULL;
};

static CodecDevice s_mic;
static CodecDevice s_speaker;

static void release(CodecDevice& d)
{
    if (d.dev) {
        esp_codec_dev_close(d.dev);
        esp_codec_dev_delete(d.dev);
    }
    if (d.codec_if) {
        audio_codec_delete_codec_if(d.codec_if);
    }
    if (d.ctrl_if) {
        audio_codec_delete_ctrl_if(d.ctrl_if);
    }
    if (d.gpio_if) {
        audio_codec_delete_gpio_if(d.gpio_if);
    }
    if (d.data_if) {
        audio_codec_delete_data_if(d.data_if);
    }
    if (d.chan) {
        i2s_del_channel(d.chan);  // The codec layer already disabled it
    }
    d = CodecDevice();
}

// I2S channel (receive or transmit) on the codecs' shared pins, and its codec data interface
static esp_err_t open_i2s(CodecDevice& d, bool transmit, uint32_t rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, transmit ? &d.chan : NULL, transmit ? NULL : &d.chan);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = BSP_I2S_MCLK;
    std_cfg.gpio_cfg.bclk = BSP_I2S_SCLK;
    std_cfg.gpio_cfg.ws = BSP_I2S_LCLK;
    std_cfg.gpio_cfg.dout = transmit ? BSP_I2S_DOUT : I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = transmit ? I2S_GPIO_UNUSED : BSP_I2S_DSIN;
    err = i2s_channel_init_std_mode(d.chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_PORT;
    (transmit ? i2s_cfg.tx_handle : i2s_cfg.rx_handle) = d.chan;
    d.data_if = audio_codec_new_i2s_data(&i2s_cfg);
    return d.data_if ? ESP_OK : ESP_FAIL;
}

static const audio_codec_ctrl_if_t* codec_control(uint8_t addr)
{
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = BSP_I2C_NUM;
    i2c_cfg.addr = addr;
    i2c_cfg.bus_handle = bsp_i2c_get_handle();
    return audio_codec_new_i2c_ctrl(&i2c_cfg);
}

static esp_err_t open_device(CodecDevice& d, esp_codec_dev_type_t type, uint32_t rate)
{
    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = type;
    dev_cfg.codec_if = d.codec_if;
    dev_cfg.data_if = d.data_if;
    d.dev = (d.codec_if && d.data_if) ? esp_codec_dev_new(&dev_cfg) : NULL;

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.sample_rate = rate;
    return d.dev && esp_codec_dev_open(d.dev, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio::mic_start()
{
    if (s_mic.dev) {
        return ESP_OK;
    }
    esp_err_t err = open_i2s(s_mic, false, MIC_RATE);
    if (err == ESP_OK) {
        s_mic.ctrl_if = codec_control(ES7210_CODEC_DEFAULT_ADDR);
        es7210_codec_cfg_t es_cfg = {};
        es_cfg.ctrl_if = s_mic.ctrl_if;
        es_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
        s_mic.codec_if = s_mic.ctrl_if ? es7210_codec_new(&es_cfg) : NULL;
        err = open_device(s_mic, ESP_CODEC_DEV_TYPE_IN, MIC_RATE);
    }
    if (err == ESP_OK) {
        esp_codec_dev_set_in_gain(s_mic.dev, MIC_GAIN_DB);
    } else {
        ESP_LOGE(TAG, "Microphone init failed");
        mic_stop();
    }
    return err;
}

size_t audio::mic_read(int16_t* mono, size_t max_samples)
{
    if (!s_mic.dev) {
        return 0;
    }
    // Small blocks, so the caller sees the recording progress
    size_t got = 0;
    while (got < max_samples) {
        size_t n = max_samples - got < 256 ? max_samples - got : 256;
        if (esp_codec_dev_read(s_mic.dev, mono + got, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            break;
        }
        got += n;
    }
    return got;
}

void audio::mic_stop()
{
    release(s_mic);
}

esp_err_t audio::speaker_start()
{
    if (s_speaker.dev) {
        return ESP_OK;
    }
    esp_err_t err = open_i2s(s_speaker, true, SPEAKER_RATE);
    if (err == ESP_OK) {
        s_speaker.ctrl_if = codec_control(ES8388_CODEC_DEFAULT_ADDR);
        s_speaker.gpio_if = audio_codec_new_gpio();
        es8388_codec_cfg_t es_cfg = {};
        es_cfg.ctrl_if = s_speaker.ctrl_if;
        es_cfg.gpio_if = s_speaker.gpio_if;
        es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
        es_cfg.master_mode = false;
        es_cfg.pa_pin = -1;   // The amplifier is switched through the IO expander
        es_cfg.hw_gain.pa_voltage = 5.0;
        es_cfg.hw_gain.codec_dac_voltage = 3.3;
        s_speaker.codec_if = (s_speaker.ctrl_if && s_speaker.gpio_if) ? es8388_codec_new(&es_cfg) : NULL;
        err = open_device(s_speaker, ESP_CODEC_DEV_TYPE_OUT, SPEAKER_RATE);
    }
    if (err == ESP_OK) {
        esp_codec_dev_set_out_vol(s_speaker.dev, SPEAKER_VOLUME);
        bsp_feature_enable(BSP_FEATURE_SPEAKER, true);
    } else {
        ESP_LOGE(TAG, "Speaker init failed");
        speaker_stop();
    }
    return err;
}

esp_err_t audio::speaker_write(const void* pcm, size_t bytes)
{
    if (!s_speaker.dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_write(s_speaker.dev, const_cast<void*>(pcm), bytes) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void audio::speaker_stop()
{
    if (s_speaker.dev) {
        bsp_feature_enable(BSP_FEATURE_SPEAKER, false);
    }
    release(s_speaker);
}
