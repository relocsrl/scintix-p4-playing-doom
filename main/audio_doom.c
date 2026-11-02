#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "audio_doom.h"
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define DOOM_AUDIO_SAMPLE_RATE 22050
#define DOOM_AUDIO_FRAMES_PER_BUFFER 256
#define DOOM_AUDIO_CHANNELS 16
#define DOOM_AUDIO_FIXED_SHIFT 16
#define DOOM_AUDIO_VOLUME_SCALE 160

/* Wiring (README "Audio wiring"): 40-pin header pin -> EchoBase pin. The EchoBase
 * names its data pins from the codec's side: our DOUT goes to its DIN (ES8311
 * DSDIN); its DOUT is the microphone (ASDOUT) and stays unconnected. */
#define JC_AUDIO_I2C_PORT I2C_NUM_0
#define JC_AUDIO_I2C_SDA GPIO_NUM_7      /* header 23 -> SDA */
#define JC_AUDIO_I2C_SCL GPIO_NUM_8      /* header 29 -> SCL */
#define JC_AUDIO_I2S_PORT I2S_NUM_0
#define JC_AUDIO_I2S_BCLK GPIO_NUM_12    /* header 27 (ID_SD) -> SCK */
#define JC_AUDIO_I2S_LRCLK GPIO_NUM_6    /* header 21 -> LRCK */
#define JC_AUDIO_I2S_DOUT GPIO_NUM_9     /* header 35 -> DIN (not DOUT) */
#define JC_AUDIO_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define JC_AUDIO_MCLK_MULTIPLE 256

/* M5 Atomic EchoBase: ES8311 has no external MCLK (clock derived from SCLK), and
 * the speaker amp (NS4150B) is muted/unmuted through a PI4IOE5V6408 I/O expander
 * on the same I2C bus, not via a GPIO PA-enable. */
#define ECHOBASE_IOEXP_ADDR 0x43
#define ECHOBASE_IOEXP_REG_IO_PP 0x07
#define ECHOBASE_IOEXP_REG_IO_DIR 0x03
#define ECHOBASE_IOEXP_REG_IO_OUT 0x05
#define ECHOBASE_IOEXP_REG_IO_PULLUP 0x0D
#define ECHOBASE_IOEXP_DIR_VALUE 0x6F

/* Health check: the codec and the expander are configured once; if the EchoBase
 * resets (power dip on its 3.3 V, a loose wire) they come back muted and the
 * game goes silent until a reboot. Every AUDIO_HEALTH_PERIOD_MS read back one
 * register of each and re-initialize what lost its setup. */
#define AUDIO_HEALTH_PERIOD_MS 2000
#define AUDIO_BOOT_TRIES 10       /* at boot: tries to find the codec... */
#define AUDIO_BOOT_RETRY_MS 300   /* ...this far apart (~3 s in all) */
#define ES8311_I2C_ADDR_7BIT (JC_AUDIO_ES8311_ADDR >> 1) /* esp_codec_dev takes the 8-bit form */
#define ES8311_REG00 0x00
#define ES8311_REG00_CSM_ON 0x80 /* set by the driver once configured; 0x1F after a reset */
#define JC_AUDIO_NVS_NAMESPACE "doom_audio"
#define JC_AUDIO_NVS_VOLUME_KEY "volume"

typedef struct {
    uint32_t sample_rate;
    uint32_t length;
    uint8_t samples[];
} cached_sfx_t;

typedef struct {
    cached_sfx_t *sfx;
    uint32_t pos;
    uint32_t step;
    uint16_t left;
    uint16_t right;
    uint32_t generation;
    bool active;
} audio_channel_t;

static const char *TAG = "doom_audio";
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static const snddevice_t s_sound_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

static i2s_chan_handle_t s_i2s_tx;
static esp_codec_dev_handle_t s_codec;
static i2c_master_bus_handle_t s_i2c_bus;
static const audio_codec_ctrl_if_t *s_codec_ctrl_if;
static const audio_codec_data_if_t *s_codec_data_if;
static const audio_codec_gpio_if_t *s_codec_gpio_if;
static const audio_codec_if_t *s_codec_if;
static i2c_master_dev_handle_t s_codec_reg_dev; /* raw register reads for the health check */
static i2c_master_dev_handle_t s_ioexp_dev;
static bool s_has_ioexp;                        /* the expander answered at boot (EchoBase) */
static SemaphoreHandle_t s_codec_mutex;         /* s_codec vs. the web page's volume setter */
static volatile bool s_audio_hold;              /* health task: pause the mixer */
static volatile bool s_audio_held;              /* mixer: paused */
static TaskHandle_t s_audio_task;
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_channel_t s_channels[DOOM_AUDIO_CHANNELS];
static bool s_sound_initialized;
static bool s_use_sfx_prefix;
static uint32_t s_logged_sfx_count;
static uint8_t s_audio_volume = DOOM_AUDIO_DEFAULT_VOLUME;

static uint8_t clamp_volume(uint8_t volume)
{
    return volume > DOOM_AUDIO_MAX_VOLUME ? DOOM_AUDIO_MAX_VOLUME : volume;
}

/* Human-readable audio state for the settings page / API (no serial log at a demo). */
static char s_audio_status[112] = "starting";

static void set_audio_status(const char *text)
{
    strlcpy(s_audio_status, text, sizeof(s_audio_status));
}

const char *doom_audio_status(void)
{
    return s_audio_status;
}

uint8_t doom_audio_get_volume(void)
{
    portENTER_CRITICAL(&s_audio_lock);
    uint8_t volume = s_audio_volume;
    portEXIT_CRITICAL(&s_audio_lock);
    return volume;
}

void doom_audio_set_volume(uint8_t volume)
{
    volume = clamp_volume(volume);

    portENTER_CRITICAL(&s_audio_lock);
    s_audio_volume = volume;
    portEXIT_CRITICAL(&s_audio_lock);

    /* The health task may be re-creating the codec; it applies the stored
     * volume itself when it reopens it. */
    if (s_codec_mutex != NULL && xSemaphoreTake(s_codec_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_codec != NULL) {
            esp_codec_dev_set_out_vol(s_codec, volume);
        }
        xSemaphoreGive(s_codec_mutex);
    }
}

esp_err_t doom_audio_load_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(JC_AUDIO_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        doom_audio_set_volume(DOOM_AUDIO_DEFAULT_VOLUME);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t volume = DOOM_AUDIO_DEFAULT_VOLUME;
    err = nvs_get_u8(nvs, JC_AUDIO_NVS_VOLUME_KEY, &volume);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        doom_audio_set_volume(DOOM_AUDIO_DEFAULT_VOLUME);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        doom_audio_set_volume(volume);
    }
    return err;
}

esp_err_t doom_audio_save_settings(uint8_t volume)
{
    volume = clamp_volume(volume);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(JC_AUDIO_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, JC_AUDIO_NVS_VOLUME_KEY, volume);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        doom_audio_set_volume(volume);
    }
    return err;
}

static void write_boot_tone(void)
{
    uint8_t volume = doom_audio_get_volume();
    if (volume == 0) {
        return;
    }

    int16_t pcm[DOOM_AUDIO_FRAMES_PER_BUFFER * 2];
    const int amplitude = (3000 * volume) / DOOM_AUDIO_MAX_VOLUME;
    const int half_period = DOOM_AUDIO_SAMPLE_RATE / (880 * 2);
    int phase = 0;
    int sample = amplitude;
    const int buffers = (DOOM_AUDIO_SAMPLE_RATE / DOOM_AUDIO_FRAMES_PER_BUFFER) / 3;

    ESP_LOGI(TAG, "Playing audio test tone at volume %u", volume);
    for (int b = 0; b < buffers; b++) {
        for (int frame = 0; frame < DOOM_AUDIO_FRAMES_PER_BUFFER; frame++) {
            if (++phase >= half_period) {
                phase = 0;
                sample = -sample;
            }
            pcm[frame * 2] = sample;
            pcm[frame * 2 + 1] = sample;
        }
        size_t bytes_written = 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_write(s_i2s_tx, pcm, sizeof(pcm), &bytes_written, portMAX_DELAY));
    }
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(JC_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG, "new I2S channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DOOM_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, /* EchoBase ES8311 derives its clock from SCLK */
            .bclk = JC_AUDIO_I2S_BCLK,
            .ws = JC_AUDIO_I2S_LRCLK,
            .dout = JC_AUDIO_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = JC_AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S enable failed");
    return ESP_OK;
}

/* The EchoBase speaker amp is muted/enabled through a PI4IOE5V6408 I/O expander
 * (I2C 0x43) on the same bus as the codec. Without this the output is silent.
 * Mirrors the M5 init: configure the pins as outputs and drive them high. */
static esp_err_t echobase_unmute(void)
{
    esp_err_t err;
    if (s_ioexp_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ECHOBASE_IOEXP_ADDR,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_ioexp_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "EchoBase I/O expander add failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    const uint8_t seq[][2] = {
        {ECHOBASE_IOEXP_REG_IO_PP, 0x00},      /* push-pull / high-Z config */
        {ECHOBASE_IOEXP_REG_IO_PULLUP, 0xFF},  /* enable pull-ups */
        {ECHOBASE_IOEXP_REG_IO_DIR, ECHOBASE_IOEXP_DIR_VALUE}, /* pin directions */
        {ECHOBASE_IOEXP_REG_IO_OUT, 0xFF},     /* outputs high -> unmute / amp on */
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        err = i2c_master_transmit(s_ioexp_dev, seq[i], sizeof(seq[i]), 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "EchoBase expander reg 0x%02x write failed: %s", seq[i][0], esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "EchoBase I/O expander configured (amp unmuted)");
    return ESP_OK;
}

/* Create the ES8311 driver and open the codec: the full register setup, so it
 * also brings a codec back after it was reset. */
static esp_err_t codec_open(void)
{
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_codec_ctrl_if,
        .gpio_if = s_codec_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .use_mclk = false,          /* EchoBase: clock from SCLK, no external MCLK */
        .pa_pin = -1,               /* amp enable is via the PI4IOE expander, not a GPIO */
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = JC_AUDIO_MCLK_MULTIPLE,
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_FAIL, TAG, "ES8311 init failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec_if,
        .data_if = s_codec_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_FAIL, TAG, "codec device init failed");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = DOOM_AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec open failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec, doom_audio_get_volume()) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec volume failed");
    return ESP_OK;
}

/* Undo codec_open (also disables the I2S channel: pause the mixer first). */
static void codec_close(void)
{
    if (s_codec != NULL) {
        esp_codec_dev_delete(s_codec); /* closes it */
        s_codec = NULL;
    }
    if (s_codec_if != NULL) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
}

static esp_err_t init_codec(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = JC_AUDIO_I2C_PORT,
        .sda_io_num = JC_AUDIO_I2C_SDA,
        .scl_io_num = JC_AUDIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "I2C init failed");

    audio_codec_i2c_cfg_t codec_i2c_cfg = {
        .port = JC_AUDIO_I2C_PORT,
        .addr = JC_AUDIO_ES8311_ADDR,
        .bus_handle = s_i2c_bus,
    };
    s_codec_ctrl_if = audio_codec_new_i2c_ctrl(&codec_i2c_cfg);
    ESP_RETURN_ON_FALSE(s_codec_ctrl_if != NULL, ESP_FAIL, TAG, "codec I2C control init failed");

    audio_codec_i2s_cfg_t codec_i2s_cfg = {
        .port = JC_AUDIO_I2S_PORT,
        .tx_handle = s_i2s_tx,
        .rx_handle = NULL,
    };
    s_codec_data_if = audio_codec_new_i2s_data(&codec_i2s_cfg);
    ESP_RETURN_ON_FALSE(s_codec_data_if != NULL, ESP_FAIL, TAG, "codec I2S data init failed");

    s_codec_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_codec_gpio_if != NULL, ESP_FAIL, TAG, "codec GPIO init failed");

    /* A second handle on the codec address for plain register reads (the health
     * check), so a missing EchoBase does not flood the log through esp_codec_dev. */
    i2c_device_config_t reg_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR_7BIT,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &reg_cfg, &s_codec_reg_dev) != ESP_OK) {
        s_codec_reg_dev = NULL;
    }

    /* The EchoBase may answer late (powering up with the board) or not at all
     * (unplugged): try for a few seconds, probing quietly first. Without it the
     * game still starts, silent, with the mixer paused; the health task sets the
     * codec up as soon as it answers. Before, a codec missing at boot meant no
     * sound until a reboot even after plugging it in. */
    bool opened = false;
    for (int i = 0; i < AUDIO_BOOT_TRIES && !opened; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_BOOT_RETRY_MS));
        }
        if (i2c_master_probe(s_i2c_bus, ES8311_I2C_ADDR_7BIT, 50) != ESP_OK) {
            continue;
        }
        if (codec_open() == ESP_OK) {
            opened = true;
        } else {
            codec_close();
        }
    }
    if (!opened) {
        ESP_LOGW(TAG, "ES8311 not answering after %d tries over %d ms (EchoBase unplugged?): "
                      "starting silent, audio comes up when it answers (checked every %d s)",
                 AUDIO_BOOT_TRIES, AUDIO_BOOT_TRIES * AUDIO_BOOT_RETRY_MS, AUDIO_HEALTH_PERIOD_MS / 1000);
        /* What does answer on the bus: nothing = wiring / power / pull-ups; the
         * expander (0x43) but not the codec (0x18) = the codec itself. */
        char found[96] = "";
        int len = 0;
        for (uint16_t a = 0x08; a < 0x78 && len < (int)sizeof(found) - 6; a++) {
            if (i2c_master_probe(s_i2c_bus, a, 20) == ESP_OK) {
                len += snprintf(found + len, sizeof(found) - len, " 0x%02x", a);
            }
        }
        ESP_LOGW(TAG, "I2C bus scan (SDA GPIO%d, SCL GPIO%d):%s", JC_AUDIO_I2C_SDA, JC_AUDIO_I2C_SCL,
                 len ? found : " no device answers (wiring, 3.3 V / GND, pull-ups?)");
        char status[112];
        snprintf(status, sizeof(status), "ES8311 not answering since boot (I2C bus:%s); retrying every %d s",
                 len ? found : " nothing", AUDIO_HEALTH_PERIOD_MS / 1000);
        set_audio_status(status);
        s_audio_hold = true; /* the mixer must not write to the unopened I2S channel */
        return ESP_OK;
    }

    /* Unmute the EchoBase speaker amp via its I/O expander (same I2C bus). */
    s_has_ioexp = i2c_master_probe(s_i2c_bus, ECHOBASE_IOEXP_ADDR, 50) == ESP_OK;
    echobase_unmute();
    set_audio_status(s_has_ioexp ? "ok" : "ok (codec answers, EchoBase amp expander not found)");
    return ESP_OK;
}

static bool read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return dev != NULL && i2c_master_transmit_receive(dev, &reg, 1, val, 1, 50) == ESP_OK;
}

/* Re-create and re-open a codec that lost its setup. Closing it disables the
 * I2S channel, so the mixer is paused around it; if reopening fails the mixer
 * stays paused (writing to a disabled channel would spin) until a later try. */
static bool codec_recover(void)
{
    s_audio_hold = true;
    for (int i = 0; i < 40 && !s_audio_held; i++) { /* a write in flight ends within ~65 ms */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    codec_close();
    esp_err_t err = codec_open();
    xSemaphoreGive(s_codec_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Codec re-init failed: %s (retrying)", esp_err_to_name(err));
        set_audio_status("ES8311 answers but its setup failed; retrying");
        return false;
    }
    s_audio_hold = false;
    return true;
}

static void audio_health_task(void *arg)
{
    (void)arg;
    bool absent = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(AUDIO_HEALTH_PERIOD_MS));
        if (i2c_master_probe(s_i2c_bus, ES8311_I2C_ADDR_7BIT, 50) != ESP_OK ||
            (s_has_ioexp && i2c_master_probe(s_i2c_bus, ECHOBASE_IOEXP_ADDR, 50) != ESP_OK)) {
            if (!absent) {
                ESP_LOGW(TAG, "Audio module not answering on I2C (unplugged / no power?); "
                              "it will be set up again when it is back");
                set_audio_status("audio module not answering on I2C (unplugged / no power?); retrying");
                absent = true;
            }
            continue;
        }
        uint8_t reg00 = 0;
        bool codec_lost = !read_reg(s_codec_reg_dev, ES8311_REG00, &reg00) || !(reg00 & ES8311_REG00_CSM_ON);
        bool ioexp_lost = false;
        if (s_has_ioexp) {
            uint8_t dir = 0;
            ioexp_lost = !read_reg(s_ioexp_dev, ECHOBASE_IOEXP_REG_IO_DIR, &dir) || dir != ECHOBASE_IOEXP_DIR_VALUE;
        }
        if (absent || codec_lost || s_audio_hold) {
            ESP_LOGW(TAG, "Audio module reset detected (ES8311 REG00=0x%02x%s): re-initializing",
                     reg00, absent ? ", was not answering" : "");
            if (codec_recover()) {
                ESP_LOGI(TAG, "Audio module back");
                set_audio_status("ok (recovered)");
            }
            /* the expander too (it may have been missing at boot) */
            s_has_ioexp = i2c_master_probe(s_i2c_bus, ECHOBASE_IOEXP_ADDR, 50) == ESP_OK;
            ioexp_lost = s_has_ioexp;
        }
        if (ioexp_lost) {
            echobase_unmute();
        }
        absent = false;
    }
}

static cached_sfx_t *cache_sfx(sfxinfo_t *sfxinfo)
{
    if (sfxinfo->driver_data != NULL) {
        return (cached_sfx_t *)sfxinfo->driver_data;
    }

    int lumpnum = sfxinfo->lumpnum;
    unsigned int lumplen = W_LumpLength(lumpnum);
    uint8_t *lump = W_CacheLumpNum(lumpnum, PU_STATIC);
    if (lump == NULL || lumplen < 40 || lump[0] != 0x03 || lump[1] != 0x00) {
        return NULL;
    }

    uint32_t sample_rate = ((uint32_t)lump[3] << 8) | lump[2];
    uint32_t length = ((uint32_t)lump[7] << 24) | ((uint32_t)lump[6] << 16) |
                      ((uint32_t)lump[5] << 8) | lump[4];
    if (sample_rate == 0 || length > lumplen - 8 || length <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return NULL;
    }

    uint32_t trimmed_length = length - 32;
    cached_sfx_t *sfx = heap_caps_malloc(sizeof(cached_sfx_t) + trimmed_length,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sfx == NULL) {
        sfx = heap_caps_malloc(sizeof(cached_sfx_t) + trimmed_length, MALLOC_CAP_8BIT);
    }
    if (sfx == NULL) {
        W_ReleaseLumpNum(lumpnum);
        return NULL;
    }

    sfx->sample_rate = sample_rate;
    sfx->length = trimmed_length;
    memcpy(sfx->samples, lump + 24, trimmed_length);
    sfxinfo->driver_data = sfx;
    W_ReleaseLumpNum(lumpnum);
    return sfx;
}

static void calculate_pan(int vol, int sep, uint16_t *left, uint16_t *right)
{
    if (vol < 0) {
        vol = 0;
    } else if (vol > 127) {
        vol = 127;
    }
    if (sep < 0) {
        sep = 0;
    } else if (sep > 254) {
        sep = 254;
    }

    uint32_t scale = ((uint32_t)DOOM_AUDIO_VOLUME_SCALE * doom_audio_get_volume()) / DOOM_AUDIO_MAX_VOLUME;
    *left = (uint16_t)(((254 - sep) * vol * scale) / (127 * 254));
    *right = (uint16_t)((sep * vol * scale) / (127 * 254));
}

static int16_t clamp_s16(int32_t sample)
{
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }
    return (int16_t)sample;
}

static void audio_task(void *arg)
{
    (void)arg;

    int16_t pcm[DOOM_AUDIO_FRAMES_PER_BUFFER * 2];
    audio_channel_t snapshot[DOOM_AUDIO_CHANNELS];

    while (true) {
        if (s_audio_hold) {
            s_audio_held = true; /* the health task is re-initializing the codec */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        s_audio_held = false;

        portENTER_CRITICAL(&s_audio_lock);
        memcpy(snapshot, s_channels, sizeof(snapshot));
        portEXIT_CRITICAL(&s_audio_lock);

        for (int frame = 0; frame < DOOM_AUDIO_FRAMES_PER_BUFFER; frame++) {
            int32_t mix_l = 0;
            int32_t mix_r = 0;

            for (int ch = 0; ch < DOOM_AUDIO_CHANNELS; ch++) {
                audio_channel_t *channel = &snapshot[ch];
                if (!channel->active || channel->sfx == NULL) {
                    continue;
                }

                uint32_t sample_index = channel->pos >> DOOM_AUDIO_FIXED_SHIFT;
                if (sample_index >= channel->sfx->length) {
                    channel->active = false;
                    continue;
                }

                int32_t centered = (int32_t)channel->sfx->samples[sample_index] - 128;
                mix_l += centered * channel->left;
                mix_r += centered * channel->right;
                channel->pos += channel->step;
            }

            pcm[frame * 2] = clamp_s16(mix_l);
            pcm[frame * 2 + 1] = clamp_s16(mix_r);
        }

        portENTER_CRITICAL(&s_audio_lock);
        for (int ch = 0; ch < DOOM_AUDIO_CHANNELS; ch++) {
            if (s_channels[ch].generation == snapshot[ch].generation) {
                s_channels[ch].pos = snapshot[ch].pos;
                s_channels[ch].active = snapshot[ch].active;
            }
        }
        portEXIT_CRITICAL(&s_audio_lock);

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_i2s_tx, pcm, sizeof(pcm), &bytes_written, portMAX_DELAY);
        if (err != ESP_OK || bytes_written != sizeof(pcm)) {
            ESP_LOGW(TAG, "I2S write incomplete: %s %u/%u",
                     esp_err_to_name(err), (unsigned)bytes_written, (unsigned)sizeof(pcm));
        }
    }
}

static int get_sfx_lump_num(sfxinfo_t *sfx)
{
    char name[9];
    if (sfx->link != NULL) {
        sfx = sfx->link;
    }

    if (s_use_sfx_prefix) {
        name[0] = 'd';
        name[1] = 's';
        memcpy(name + 2, sfx->name, 6);
        name[8] = '\0';
    } else {
        strlcpy(name, sfx->name, sizeof(name));
    }
    return W_GetNumForName(name);
}

static boolean init_sound(boolean use_sfx_prefix)
{
    if (s_sound_initialized) {
        return true;
    }

    s_use_sfx_prefix = use_sfx_prefix;
    memset(s_channels, 0, sizeof(s_channels));

    s_codec_mutex = xSemaphoreCreateMutex();
    if (s_codec_mutex == NULL || init_i2s() != ESP_OK || init_codec() != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed");
        return false;
    }

    if (!s_audio_hold) {
        write_boot_tone();  /* not with the codec missing: the I2S channel is not open */
    }

    BaseType_t created = xTaskCreatePinnedToCore(audio_task, "doom_audio", 4096, NULL,
                                                tskIDLE_PRIORITY + 2, &s_audio_task, 0);
    if (created != pdTRUE) {
        ESP_LOGE(TAG, "Audio task create failed");
        return false;
    }
    if (xTaskCreatePinnedToCore(audio_health_task, "audio_health", 4096, NULL,
                                tskIDLE_PRIORITY + 1, NULL, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Audio health task create failed (no auto-recovery)");
    }

    s_sound_initialized = true;
    if (s_audio_hold) {
        ESP_LOGW(TAG, "Doom SFX audio waiting for the ES8311 (silent until it answers)");
    } else {
        ESP_LOGI(TAG, "Doom SFX audio ready: ES8311 I2S %u Hz", DOOM_AUDIO_SAMPLE_RATE);
    }
    return true;
}

static void shutdown_sound(void)
{
    s_sound_initialized = false;
}

static void update_sound(void)
{
}

static void update_sound_params(int channel, int vol, int sep)
{
    if (!s_sound_initialized || channel < 0 || channel >= DOOM_AUDIO_CHANNELS) {
        return;
    }

    uint16_t left = 0;
    uint16_t right = 0;
    calculate_pan(vol, sep, &left, &right);

    portENTER_CRITICAL(&s_audio_lock);
    s_channels[channel].left = left;
    s_channels[channel].right = right;
    s_channels[channel].generation++;
    portEXIT_CRITICAL(&s_audio_lock);
}

static int start_sound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    if (!s_sound_initialized || channel < 0 || channel >= DOOM_AUDIO_CHANNELS) {
        return -1;
    }

    cached_sfx_t *sfx = cache_sfx(sfxinfo);
    if (sfx == NULL) {
        return -1;
    }

    uint16_t left = 0;
    uint16_t right = 0;
    calculate_pan(vol, sep, &left, &right);

    uint32_t step = (uint32_t)(((uint64_t)sfx->sample_rate << DOOM_AUDIO_FIXED_SHIFT) /
                               DOOM_AUDIO_SAMPLE_RATE);
    if (step == 0) {
        step = 1;
    }

    portENTER_CRITICAL(&s_audio_lock);
    s_channels[channel].sfx = sfx;
    s_channels[channel].pos = 0;
    s_channels[channel].step = step;
    s_channels[channel].left = left;
    s_channels[channel].right = right;
    s_channels[channel].active = true;
    s_channels[channel].generation++;
    portEXIT_CRITICAL(&s_audio_lock);

    if (s_logged_sfx_count < 20) {
        ESP_LOGI(TAG, "Start SFX channel=%d lump=%d rate=%u len=%u vol=%d sep=%d",
                 channel, sfxinfo->lumpnum, (unsigned)sfx->sample_rate,
                 (unsigned)sfx->length, vol, sep);
        s_logged_sfx_count++;
    }
    return channel;
}

static void stop_sound(int channel)
{
    if (channel < 0 || channel >= DOOM_AUDIO_CHANNELS) {
        return;
    }

    portENTER_CRITICAL(&s_audio_lock);
    s_channels[channel].active = false;
    s_channels[channel].generation++;
    portEXIT_CRITICAL(&s_audio_lock);
}

static boolean sound_is_playing(int channel)
{
    if (channel < 0 || channel >= DOOM_AUDIO_CHANNELS) {
        return false;
    }

    portENTER_CRITICAL(&s_audio_lock);
    bool active = s_channels[channel].active;
    portEXIT_CRITICAL(&s_audio_lock);
    return active;
}

static void precache_sounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;
}

sound_module_t DG_sound_module = {
    (snddevice_t *)s_sound_devices,
    sizeof(s_sound_devices) / sizeof(s_sound_devices[0]),
    init_sound,
    shutdown_sound,
    get_sfx_lump_num,
    update_sound,
    update_sound_params,
    start_sound,
    stop_sound,
    sound_is_playing,
    precache_sounds,
};

static boolean init_music(void)
{
    return false;
}

static void shutdown_music(void)
{
}

static void set_music_volume(int volume)
{
    (void)volume;
}

static void pause_music(void)
{
}

static void resume_music(void)
{
}

static void *register_song(void *data, int len)
{
    (void)data;
    (void)len;
    return NULL;
}

static void unregister_song(void *handle)
{
    (void)handle;
}

static void play_song(void *handle, boolean looping)
{
    (void)handle;
    (void)looping;
}

static void stop_song(void)
{
}

static boolean music_is_playing(void)
{
    return false;
}

music_module_t DG_music_module = {
    (snddevice_t *)s_sound_devices,
    sizeof(s_sound_devices) / sizeof(s_sound_devices[0]),
    init_music,
    shutdown_music,
    set_music_volume,
    pause_music,
    resume_music,
    register_song,
    unregister_song,
    play_song,
    stop_song,
    music_is_playing,
    NULL,
};
