#include "wifi_board.h"
#include "codecs/es8388_audio_codec.h"
#include "codecs/es8311_audio_codec.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "led/single_led.h"
#include "esp32_camera.h"
#include "esp32_music.h"
#include "esp32_motor.h"
#include "iot/iot.h"
#include "otto_emoji_display.h"
#include "backlight.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_st7735.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <esp_lcd_panel_ops.h>
#include <esp_event.h>
#include <vector>
#include <esp_lcd_gc9a01.h>

#define TAG "atk_dnesp32s3"

class atk_dnesp32s3 : public WifiBoard {
public:
    Button boot_button_;
  
private:
    i2c_master_bus_handle_t i2c_bus_;
    OttoEmojiDisplay *display_;
    Music* music_;
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    esp_event_handler_instance_t wifi_evt_inst_ = nullptr;
    bool power_profile_applied_ = false;

    // WiFi 事件钩子：在 WIFI_EVENT_STA_START 时立刻将 TX 功率限到 8dBm 降低峰值电流；
    // 在 IP_EVENT_STA_GOT_IP（联网成功）时恢复背光亮度到用户设置
    static void WifiEventHook(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
        auto* self = static_cast<atk_dnesp32s3*>(arg);
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            int8_t dbm = 8;
            esp_err_t err = esp_wifi_set_max_tx_power(dbm);
            ESP_LOGI(TAG, "[WiFi-PROFILE] TX power cap set to %d dBm: %s",
                     dbm, esp_err_to_name(err));
            self->power_profile_applied_ = true;
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "[WiFi-PROFILE] Got IP, restoring user brightness");
            if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
                self->GetBacklight()->RestoreBrightness();
            }
        }
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeGc9a01Display() {
        ESP_LOGI(TAG, "Init GC9A01 display");
        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, 0, NULL);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;
        panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        uint8_t data_0x62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
        esp_lcd_panel_io_tx_param(panel_io, 0x62, data_0x62, sizeof(data_0x62));

        uint8_t data_0x63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
        esp_lcd_panel_io_tx_param(panel_io, 0x63, data_0x63, sizeof(data_0x63));

        display_ = new OttoEmojiDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {

        // 新增的双击 → 蓝牙配网
        boot_button_.OnDoubleClick([this]() {
            EnterWifiConfigMode();
        });

        // GetIOT()->SPK_EN_init();
        // SPK_EN(1);

        // 长按开关键 → 关机
        esp_timer_handle_t timer_handle_ = nullptr;
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* board = (atk_dnesp32s3*)arg;
                board->boot_button_.OnLongPress([board]() {
                    SPK_EN(0);
                });
            },
            .arg = this,
        };
        esp_timer_create(&timer_args, &timer_handle_);
        esp_timer_start_once(timer_handle_, 20 * 1000 * 1000);
            
        
        // boot_button_.OnLongPress([this]() {
        //   SPK_EN(0);
        // });
    }

    void SendCmd(uint8_t cmd, const uint8_t* data, size_t len) {
        esp_lcd_panel_io_tx_param(panel_io, cmd, data, len);
    }

    void InitializeSt7735Display() {
        // 参考 esp32-cgc 的 ST7735_128X128 完整方案：完全使用 ST7789 驱动默认初始化，
        // 通过 esp_lcd_panel_invert_color/swap_xy/mirror 配合配置宏控制所有行为。
        // 不手动发任何命令，避免与驱动内部命令序列冲突。
        //
        // 配置宏（见 config.h，与 esp32-cgc 的 ST7735_128X128 完全一致）：
        //   DISPLAY_INVERT_COLOR = false
        //   DISPLAY_RGB_ORDER    = LCD_RGB_ELEMENT_ORDER_BGR
        //   DISPLAY_MIRROR_X/Y   = true
        //   DISPLAY_SWAP_XY      = false
        //   DISPLAY_OFFSET_X/Y   = 2/3
        ESP_LOGI(TAG, "Initialize ST7735 display (0.9inch round 128x128, esp32-cgc compatible)");
        ESP_LOGI(TAG, "SPI: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d mode=%d freq=%d",
                 DISPLAY_SCLK_PIN, DISPLAY_MOSI_PIN, DISPLAY_CS_PIN,
                 DISPLAY_DC_PIN, DISPLAY_RESET_PIN, DISPLAY_BACKLIGHT_PIN,
                 DISPLAY_SPI_MODE, DISPLAY_SPI_SCLK_HZ);
        ESP_LOGI(TAG, "Flags: invert=%d rgb_order=%d mirror_x=%d mirror_y=%d swap_xy=%d offset=%d,%d",
                 DISPLAY_INVERT_COLOR ? 1 : 0,
                 DISPLAY_RGB_ORDER == LCD_RGB_ELEMENT_ORDER_BGR ? 1 : 0,
                 DISPLAY_MIRROR_X ? 1 : 0, DISPLAY_MIRROR_Y ? 1 : 0,
                 DISPLAY_SWAP_XY ? 1 : 0, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);

        // 液晶屏控制IO初始化
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 使用 ST7789 驱动（与 esp32-cgc 一致）
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RESET_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        // 完全复刻 esp32-cgc 的初始化路径：reset → init → invert → swap_xy → mirror
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        // 显式开启显示
        esp_lcd_panel_disp_on_off(panel, true);
        vTaskDelay(pdMS_TO_TICKS(120));

        ESP_LOGI(TAG, "Creating OttoEmojiDisplay...");
        display_ = new OttoEmojiDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        ESP_LOGI(TAG, "ST7735 display initialized successfully");
    }

public:
    atk_dnesp32s3() : boot_button_(BOOT_BUTTON_GPIO) {
        // 诊断重启循环：打印上次复位原因
        // 1=上电 3=软件重启 4=PANIC崩溃 5=中断看门狗 6=任务看门狗 11=电源跌落(BROWNOUT)
        const char* reason_str;
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:   reason_str = "POWERON(上电)"; break;
            case ESP_RST_SW:        reason_str = "SW(软件重启)"; break;
            case ESP_RST_PANIC:     reason_str = "PANIC(崩溃)"; break;
            case ESP_RST_INT_WDT:   reason_str = "INT_WDT(中断看门狗)"; break;
            case ESP_RST_TASK_WDT:  reason_str = "TASK_WDT(任务看门狗)"; break;
            case ESP_RST_WDT:       reason_str = "WDT(其他看门狗)"; break;
            case ESP_RST_BROWNOUT:  reason_str = "BROWNOUT(电源跌落)"; break;
            default:                reason_str = "OTHER"; break;
        }
        ESP_LOGI(TAG, "=== 上次复位原因: %s, 内部RAM剩余: %u ===",
                 reason_str, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        InitializeI2c();
        InitializeSpi();
        InitializeGc9a01Display();
        // InitializeSt7735Display();
        InitializeButtons();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            // 临时先设 50% 亮度降低背光电流（WiFi 启动峰值时刻避开 brownout）
            // 连接稳定后自动 RestoreBrightness 到用户设置（~75%）
            // GetBacklight()->SetBrightness(50);
            // TODO: 供电稳定后可删除上面一行，仅保留 RestoreBrightness()
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
	
	virtual void StartNetwork() override {
        // 在父类 StartNetwork 之前注册 WiFi/IP 事件钩子，
        // 保证 esp_wifi_start() 刚返回就调用 esp_wifi_set_max_tx_power(8dBm) 限峰电流
        if (!wifi_evt_inst_) {
            esp_event_handler_instance_register(
                WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHook, this, &wifi_evt_inst_);
            esp_event_handler_instance_register(
                IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHook, this, nullptr);
            ESP_LOGI(TAG, "[WiFi-PROFILE] hooks installed");
        }
        WifiBoard::StartNetwork();
    }

    virtual Camera *GetCamera() override { return nullptr; }

    virtual Music* GetMusic() override {
        static Music* music_ = new Esp32Music();
        return music_;
    }

    virtual Motor* GetMotor() override {
        static Motor* motor_ = new Esp32Motor();
        return motor_;
    }

    virtual IOT* GetIOT() override {
        static IOT* iot_ = new IOT();
        return iot_;
    }
};

DECLARE_BOARD(atk_dnesp32s3);
