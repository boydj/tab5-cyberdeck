/*
 * T-Display P4 audio out: ES8311 codec (I2C 0x18 on the audio bus) fed by
 * I2S0 with MCLK at 256 x fs, into the NS4150B speaker amplifier. Pins from
 * LilyGo's lilygo_device_driver (v1/config.h).
 *
 * The codec and amplifier hang off one switched supply (XL9535 IO6, driven by
 * hal_speaker_amp). It is only on while the player has the device open, and
 * the codec is set up again after every power-up, since cutting the supply
 * loses its registers. There is no headphone jack detection on this board.
 */
#include "aout.h"

#include "deck_hal.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aout";

#define I2S_MCLK GPIO_NUM_13
#define I2S_SCLK GPIO_NUM_12
#define I2S_LCLK GPIO_NUM_9
#define I2S_DOUT GPIO_NUM_10 /* to the codec DAC */
#define I2S_DIN  GPIO_NUM_11 /* from the codec ADC (mic), unused */

static i2s_chan_handle_t s_tx;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec;
static esp_codec_dev_handle_t s_dev;
static bool s_open;
static int s_volume = 60;

static bool i2s_setup(void)
{
    if (s_tx) return true;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear        = true; /* silence, not stale samples, on underrun */
    chan.dma_desc_num      = 8;    /* ~90 ms of buffer at 44.1 kHz rides out SD hiccups */
    chan.dma_frame_num     = 512;
    if (i2s_new_channel(&chan, &s_tx, NULL) != ESP_OK) return false;
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100), /* MCLK = 256 x fs */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_MCLK,
                .bclk = I2S_SCLK,
                .ws   = I2S_LCLK,
                .dout = I2S_DOUT,
                .din  = I2S_GPIO_UNUSED,
            },
    };
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return false;
    }
    return true;
}

static void codec_teardown(void)
{
    if (s_dev) esp_codec_dev_delete(s_dev);
    if (s_codec) audio_codec_delete_codec_if(s_codec);
    if (s_ctrl_if) audio_codec_delete_ctrl_if(s_ctrl_if);
    if (s_gpio_if) audio_codec_delete_gpio_if(s_gpio_if);
    if (s_data_if) audio_codec_delete_data_if(s_data_if);
    s_dev     = NULL;
    s_codec   = NULL;
    s_ctrl_if = NULL;
    s_gpio_if = NULL;
    s_data_if = NULL;
}

/* Called with the audio supply on. */
static bool codec_setup(void)
{
    if (s_dev) return true;
    if (!i2s_setup()) return false;

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .tx_handle = s_tx};
    s_data_if                     = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = (i2c_master_bus_handle_t)hal_codec_i2c()};
    s_ctrl_if                     = audio_codec_new_i2c_ctrl(&i2c_cfg);
    s_gpio_if                     = audio_codec_new_gpio();
    if (s_data_if == NULL || s_ctrl_if == NULL || s_gpio_if == NULL) {
        codec_teardown();
        return false;
    }

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if     = s_ctrl_if,
        .gpio_if     = s_gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = -1, /* the amplifier supply is on the IO expander */
        .master_mode = false,
        .use_mclk    = true,
        .mclk_div    = 256,
        .hw_gain     = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    s_codec = es8311_codec_new(&codec_cfg);
    if (s_codec == NULL) {
        codec_teardown();
        return false;
    }
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = s_codec, .data_if = s_data_if};
    s_dev                       = esp_codec_dev_new(&dev_cfg);
    if (s_dev == NULL) {
        codec_teardown();
        return false;
    }
    return true;
}

bool aout_open(uint32_t rate, int channels)
{
    if (s_open) esp_codec_dev_close(s_dev);
    s_open = false;
    hal_speaker_amp(true);
    vTaskDelay(pdMS_TO_TICKS(10)); /* supply up before talking to the codec */
    if (!codec_setup()) {
        ESP_LOGE(TAG, "codec setup failed");
        hal_speaker_amp(false);
        return false;
    }
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = (uint8_t)channels, .sample_rate = rate};
    s_open = esp_codec_dev_open(s_dev, &fs) == ESP_CODEC_DEV_OK;
    if (!s_open) {
        ESP_LOGE(TAG, "codec open %lu Hz x%d failed", (unsigned long)rate, channels);
        codec_teardown();
        hal_speaker_amp(false);
        return false;
    }
    esp_codec_dev_set_out_vol(s_dev, s_volume);
    ESP_LOGI(TAG, "%lu Hz x%d to speaker", (unsigned long)rate, channels);
    return true;
}

void aout_write(const int16_t *pcm, size_t count)
{
    if (!s_open) return;
    esp_codec_dev_write(s_dev, (void *)pcm, (int)(count * sizeof(int16_t))); /* bytes */
}

void aout_close(void)
{
    if (s_open) esp_codec_dev_close(s_dev);
    s_open = false;
    codec_teardown(); /* the supply goes off, so set the codec up from scratch next time */
    hal_speaker_amp(false);
}

void aout_volume(int percent)
{
    s_volume = percent;
    if (s_open) esp_codec_dev_set_out_vol(s_dev, percent);
}

void aout_route(bool headphones) { (void)headphones; } /* speaker only */
