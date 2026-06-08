#include "iot.h" 
#include <esp_log.h>

#define TAG "IOT"

IOT::IOT() {

}

IOT::~IOT() {

}

uint32_t IOT::ledc_duty_pow(uint32_t duty, uint8_t m, uint8_t n)
{
    uint32_t result = 1;

    while (n--)
    {
        result *= m;
    }

    return (result * duty) / 100;
}

/**
 * @brief       初始化LEDC
 * @param       ledc_config: ledc配置结构体
 * @retval      无
 */
void IOT::ledc_init(ledc_config_t *ledc_config)
{
    ledc_config->duty = ledc_duty_pow(ledc_config->duty, 2, ledc_config->duty_resolution);

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,            /* 低速模式(ESP32P4仅支持低速模式) */
        .duty_resolution  = ledc_config->duty_resolution,   /* 占空比分辨率 */
        .timer_num        = ledc_config->timer_num,         /* 定时器选择 */
        .freq_hz          = ledc_config->freq_hz,           /* 设置频率 */
        .clk_cfg          = ledc_config->clk_cfg            /* 设置时钟源 */
    };
    /* 配置ledc定时器 */
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* 配置pwm通道 */
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = ledc_config->gpio_num,    /* 输出GPIO管脚 */
        .speed_mode     = LEDC_LOW_SPEED_MODE,      /* 低速模式 */
        .channel        = ledc_config->channel,     /* PWM输出通道 */
        .intr_type      = LEDC_INTR_DISABLE,        /* 关闭LEDC中断 */
        .timer_sel      = ledc_config->timer_num,   /* 那个定时器提供计数值 */
        .duty           = ledc_config->duty,        /* 占空比 */
        .hpoint         = 0                         /* 设置hpoint数值 */
    };
    /* Lpoint = duty + hpoint */

    /* 配置pwm通道 */
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

/**
 * @brief       设置占空比
 * @param       ledc_config: ledc配置结构体
 *              duty: 占空比
 * @retval      无
 */
void IOT::ledc_pwm_set_duty(ledc_config_t *ledc_config, uint16_t duty)
{
    ledc_config->duty = ledc_duty_pow(duty, 2, ledc_config->duty_resolution);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_config->channel, ledc_config->duty);    /* 设置占空比 */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_config->channel);                    /* 更新占空比 */
}



void IOT::LED_RED_PWM_init(void)
{
    uint32_t duty = ledc_duty_pow(0, 2, 14);               //10为占空比0~100，14为位数，最大计数值

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,            /* 低速模式(ESP32P4仅支持低速模式) */
        .duty_resolution  = LEDC_TIMER_14_BIT,    //14,                             /* 占空比分辨率 */
        .timer_num        = LEDC_TIMER_0,                   /* 定时器选择 */
        .freq_hz          = 5,                             /* 设置频率 */
        .clk_cfg          = LEDC_AUTO_CLK                   /* 设置时钟源 */
    };
    /* 配置ledc定时器 */
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* 配置pwm通道 */
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = LEDC_PWM_CH0_GPIO,        /* 输出GPIO管脚 */
        .speed_mode     = LEDC_LOW_SPEED_MODE,      /* 低速模式 */
        .channel        = LEDC_CHANNEL_0,           /* PWM输出通道 */
        .intr_type      = LEDC_INTR_DISABLE,        /* 关闭LEDC中断 */
        .timer_sel      = LEDC_TIMER_0,             /* 那个定时器提供计数值 */
        .duty           = duty,                     /* 占空比 */
        .hpoint         = 0                         /* 设置hpoint数值 */
    };
    /* Lpoint = duty + hpoint */

    /* 配置pwm通道 */
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

esp_err_t IOT::led_red_pwm_set_duty(uint16_t duty)
{
    uint16_t pduty = ledc_duty_pow(duty, 2, 14);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pduty );    /* 设置占空比 */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);                    /* 更新占空比 */
    return pduty;
}


void IOT::LED_GREEN_PWM_init(void)
{
    uint32_t duty = ledc_duty_pow(0, 2, 14);               //10为占空比0~100，14为位数，最大计数值

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,            /* 低速模式(ESP32P4仅支持低速模式) */
        .duty_resolution  = LEDC_TIMER_14_BIT,      //14,                             /* 占空比分辨率 */
        .timer_num        = LEDC_TIMER_1,                   /* 定时器选择 */
        .freq_hz          = 5,                             /* 设置频率 */
        .clk_cfg          = LEDC_AUTO_CLK                   /* 设置时钟源 */
    };
    /* 配置ledc定时器 */
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* 配置pwm通道 */
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = LEDC_PWM_CH1_GPIO,        /* 输出GPIO管脚 */
        .speed_mode     = LEDC_LOW_SPEED_MODE,      /* 低速模式 */
        .channel        = LEDC_CHANNEL_1,           /* PWM输出通道 */
        .intr_type      = LEDC_INTR_DISABLE,        /* 关闭LEDC中断 */
        .timer_sel      = LEDC_TIMER_1,             /* 那个定时器提供计数值 */
        .duty           = duty,                     /* 占空比 */
        .hpoint         = 0                         /* 设置hpoint数值 */
    };
    /* Lpoint = duty + hpoint */

    /* 配置pwm通道 */
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

esp_err_t IOT::led_green_pwm_set_duty(uint16_t duty)
{
    uint16_t pduty = ledc_duty_pow(duty, 2, 14);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, pduty );    /* 设置占空比 */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);                    /* 更新占空比 */
    return pduty;
}


void IOT::Servo_Switch_H_PWM_init(void)
{
    uint32_t duty = ledc_duty_pow(10, 2, 12);               //10为占空比0~100，14为位数，最大计数值

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,            /* 低速模式(ESP32P4仅支持低速模式) */
        .duty_resolution  = LEDC_TIMER_12_BIT,      //12,                             /* 占空比分辨率 */
        .timer_num        = LEDC_TIMER_2,                   /* 定时器选择 */
        .freq_hz          = 10000,                             /* 设置频率 */
        .clk_cfg          = LEDC_AUTO_CLK                   /* 设置时钟源 */
    };
    /* 配置ledc定时器 */
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* 配置pwm通道 */
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = LEDC_PWM_CH2_GPIO,        /* 输出GPIO管脚 */
        .speed_mode     = LEDC_LOW_SPEED_MODE,      /* 低速模式 */
        .channel        = LEDC_CHANNEL_2,           /* PWM输出通道 */
        .intr_type      = LEDC_INTR_DISABLE,        /* 关闭LEDC中断 */
        .timer_sel      = LEDC_TIMER_2,             /* 那个定时器提供计数值 */
        .duty           = duty,                     /* 占空比 */
        .hpoint         = 0                         /* 设置hpoint数值 */
    };
    /* Lpoint = duty + hpoint */

    /* 配置pwm通道 */
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

esp_err_t IOT::servo_switch_h_pwm_set_duty(uint16_t duty)
{
    uint16_t pduty = ledc_duty_pow(duty, 2, 12);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, pduty );    /* 设置占空比 */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);                    /* 更新占空比 */
    return pduty;
}





void IOT::Servo_Switch_V_PWM_init(void)
{
    uint32_t duty = ledc_duty_pow(10, 2, 12);               //10为占空比0~100，14为位数，最大计数值

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,            /* 低速模式(ESP32P4仅支持低速模式) */
        .duty_resolution  = LEDC_TIMER_12_BIT,      //12,                             /* 占空比分辨率 */
        .timer_num        = LEDC_TIMER_3,                   /* 定时器选择 */
        .freq_hz          = 100,                             /* 设置频率 */
        .clk_cfg          = LEDC_AUTO_CLK                   /* 设置时钟源 */
    };
    /* 配置ledc定时器 */
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* 配置pwm通道 */
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = LEDC_PWM_CH3_GPIO,        /* 输出GPIO管脚 */
        .speed_mode     = LEDC_LOW_SPEED_MODE,      /* 低速模式 */
        .channel        = LEDC_CHANNEL_3,           /* PWM输出通道 */
        .intr_type      = LEDC_INTR_DISABLE,        /* 关闭LEDC中断 */
        .timer_sel      = LEDC_TIMER_3,             /* 那个定时器提供计数值 */
        .duty           = duty,                     /* 占空比 */
        .hpoint         = 0                         /* 设置hpoint数值 */
    };
    /* Lpoint = duty + hpoint */

    /* 配置pwm通道 */
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

esp_err_t IOT::servo_switch_v_pwm_set_duty(uint16_t duty)
{
    uint16_t pduty = ledc_duty_pow(duty, 2, 12);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, pduty );    /* 设置占空比 */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);                    /* 更新占空比 */
    return pduty;
}

void IOT::Wheel_Speed_PWM_init(void)
{
    uint32_t duty = ledc_duty_pow(10, 2, 12);               //10为占空比0~100，14为位数，最大计数值

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,            /* 低速模式(ESP32P4仅支持低速模式) */
        .duty_resolution  = LEDC_TIMER_12_BIT,      //12,                             /* 占空比分辨率 */
        .timer_num        = LEDC_TIMER_1,                   /* 定时器选择 */
        .freq_hz          = 100,                             /* 设置频率 */
        .clk_cfg          = LEDC_AUTO_CLK                   /* 设置时钟源 */
    };
    /* 配置ledc定时器 */
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* 配置pwm通道 */
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = LEDC_PWM_CH4_GPIO,        /* 输出GPIO管脚 */
        .speed_mode     = LEDC_LOW_SPEED_MODE,      /* 低速模式 */
        .channel        = LEDC_CHANNEL_4,           /* PWM输出通道 */
        .intr_type      = LEDC_INTR_DISABLE,        /* 关闭LEDC中断 */
        .timer_sel      = LEDC_TIMER_1,             /* 那个定时器提供计数值 */
        .duty           = duty,                     /* 占空比 */
        .hpoint         = 0                         /* 设置hpoint数值 */
    };
    /* Lpoint = duty + hpoint */

    /* 配置pwm通道 */
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

esp_err_t IOT::wheel_speed_pwm_set_duty(uint16_t duty)
{
    uint16_t pduty = ledc_duty_pow(duty, 2, 12);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, pduty );    /* 设置占空比 */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4);                    /* 更新占空比 */
    return pduty;
}




/**
 * @brief       初始化ADC
 * @param       无
 * @retval      无
 */
void IOT::adc_init(void)
{
    adc_oneshot_unit_init_cfg_t adc_config = {  /* 初始化配置结构体 */
        .unit_id  = ADC_UNIT_1,                 /* ADC单元:ADC1/ADC2 */
        .ulp_mode = ADC_ULP_MODE_DISABLE,       /* 不支持ADC在ULP模式下工作 */
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_config, &adc_handle));    /* ADC初始化(单次转换模式) */

    /* 配置 ADC */
    adc_oneshot_chan_cfg_t config = {
        .atten    = ADC_ATTEN_DB_12,            /* ADC衰减 */
        .bitwidth = ADC_BITWIDTH_12,            /* ADC分辨率 */
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHAN, &config));     /* 配置ADC通道 */
}

#define LOST_VAL    1

/**
 * @brief       获取ADC转换且进行多次采样后排序去除最高和最低值再做均值滤波后的结果
 * @note        ESP32P4 ADC对噪声敏感,可能导致ADC读数出现较大偏差
 * @note        软件上:可通过多次采样进一步降低噪声影响;硬件上:可加旁路电容连在在ADC使用引脚上
 * @param       ch      : 通道号, 0~9
 * @param       times   : 获取次数
 * @retval      通道ch的times次转换结果平均值
 */
uint32_t IOT::adc_get_result_average(uint32_t times)
{
    // uint32_t sum = 0;
    // uint16_t temp_val = 0;

    // /* 申请存放ADC原始数据buffer */
    // // int *rawdata = heap_caps_malloc(times * sizeof(int), MALLOC_CAP_INTERNAL); 
    // void *rawdata = heap_caps_malloc(times * sizeof(int), MALLOC_CAP_INTERNAL);     
    // if (NULL == rawdata)
    // {
    //     ESP_LOGE("adc", "Memory for adc is not enough");
    // }

    // for (uint32_t t = 0; t < times; t++)                /* 多次ADC采样 */
    // {
    //     adc_oneshot_read(adc_handle, ADC_CHANNEL_7 , &rawdata[t]);  /* 读取原始数据 */
    //     vTaskDelay(pdMS_TO_TICKS(5));
    // }

    // for (uint16_t i = 0; i < times - 1; i++)            /* 对数据进行排序 */
    // {
    //     for (uint16_t j = i + 1; j < times; j++)
    //     {
    //         if (rawdata[i] > rawdata[j])                /* 升序排列 */
    //         {
    //             temp_val   = rawdata[i];
    //             rawdata[i] = rawdata[j];
    //             rawdata[j] = temp_val;
    //         }
    //     }
    // }

    // for (uint32_t i = LOST_VAL; i < times - LOST_VAL; i++)      /* 去掉两端的丢弃值 */
    // {
    //     sum += rawdata[i];                                      /* 累加去掉丢弃值以后的数据 */
    // }

    // return sum / (times - 2 * LOST_VAL);                        /* 返回平均值 */
    return 0;
}




void IOT::led_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << LED_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    LED(0);                                                 /* 关闭LED氛围灯 */
}

void IOT::LCD_RESET0_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << LCD_RESET0_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    LCD_RESET0(0);
    vTaskDelay(5);
    LCD_RESET0(1);                                          /* 复位LCD0，高电平有效，低电平复位 */
}


void IOT::LCD_RESET1_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << LCD_RESET1_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    LCD_RESET0(0);
    vTaskDelay(5);
    LCD_RESET1(1);                                          /* 复位LCD1 */
}



void IOT::LED_RED_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << LED_RED_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    LED_RED(0);                                             /* 关闭状态指示灯红色 */
}



void IOT::LED_GREEN_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << LED_GREEN_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    LED_GREEN(0);                                           /* 关闭状态指示灯红色 */
}



void IOT::Servo_Switch_H_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;         /* 输入输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << Servo_Switch_H_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    Servo_Switch_H(0);                                      /* 关闭摇头电机 */
}

void IOT::S_FW_REV_V_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << S_FW_REV_V_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    S_FW_REV_V(0);                                          /* 摇头电机正反转控制低电平反转OUTB-A，高电平OUTA-B */
}

esp_err_t Servo_speed(esp_err_t pwm_en)
{
    S_FW_REV_V(pwm_en); 

    return pwm_en;
}

void IOT::Servo_Switch_V_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;         /* 输入输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << Servo_Switch_V_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    Servo_Switch_V(0);                                      /* 关闭点头电机 */
}




void IOT::S_FW_REV_H_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << S_FW_REV_H_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    S_FW_REV_H(0);                                          /* 点头电机正反转控制低电平反转OUTB-A，高电平OUTA-B */
}


void IOT::SPK_EN_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输入输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << SPK_EN_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    SPK_EN(0);                                              /* 关闭声音输出 */
}


void IOT::SPI_CS0_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << SPI_CS0_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    SPI_CS0(0);                                             /* LCD0屏*/
}


void IOT::SPI_CS1_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << SPI_CS1_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    SPI_CS1(0);                                             /* LCD1屏 */
}



void IOT::BACKL0_init(void) {
#if 0
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << BACKL0_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    BACKL0(0); /* 关闭屏0背光 */
#endif
    gpio_config_t bk_cfg = {
        .pin_bit_mask = 1ULL << BACKL0_GPIO_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bk_cfg);
    gpio_set_level(BACKL0_GPIO_PIN, 1);   // 高电平点亮背光
}




void IOT::BACKL1_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << BACKL1_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    BACKL1(0);                                              /* 关闭屏1背光*/
}



void IOT::Wheel_Speed_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << Wheel_Speed_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    Wheel_Speed(0);                                         /* 车轮转速控制 */
}


void IOT::Wheel_Switch_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;               /* 输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << Wheel_Switch_GPIO_PIN;   /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置GPIO */

    Wheel_Switch(0);                                        /* 关闭车轮电机 */
}


/**
 * @brief       发送命令到LCD，使用轮询方式阻塞等待传输完成(由于数据传输量很少，因此在轮询方式处理可提高速度。使用中断方式的开销要超过轮询方式)
 * @param       cmd 传输的8位命令数据
 * @retval      无
 */
void IOT::lcd_write_cmd(const uint8_t cmd)
{
    LCD_WR(0);
    spi2_write_cmd(MY_LCD_Handle, cmd);
}

/**
 * @brief       发送数据到LCD，使用轮询方式阻塞等待传输完成(由于数据传输量很少，因此在轮询方式处理可提高速度。使用中断方式的开销要超过轮询方式)
 * @param       data 传输的8位数据
 * @retval      无
 */
void IOT::lcd_write_data(const uint8_t *data, int len)
{
    LCD_WR(1);
    spi2_write_data(MY_LCD_Handle, data, len);
}

/**
 * @brief       发送数据到LCD，使用轮询方式阻塞等待传输完成(由于数据传输量很少，因此在轮询方式处理可提高速度。使用中断方式的开销要超过轮询方式)
 * @param       data 传输的16位数据
 * @retval      无
 */
void IOT::lcd_write_data16(uint16_t data)
{
    uint8_t dataBuf[2] = {0,0};
    dataBuf[0] = data >> 8;
    dataBuf[1] = data & 0xFF;
    LCD_WR(1);
    spi2_write_data(MY_LCD_Handle, dataBuf,2);
}

/**
 * @brief       设置窗口大小
 * @param       xstar：左上角x轴
 * @param       ystar：左上角y轴
 * @param       xend：右下角x轴
 * @param       yend：右下角y轴
 * @retval      无
 */
void IOT::lcd_set_window(uint16_t xstar, uint16_t ystar,uint16_t xend,uint16_t yend)
{	
    uint8_t databuf[4] = {0,0,0,0};
    databuf[0] = xstar >> 8;
    databuf[1] = 0xFF & xstar;
    databuf[2] = xend >> 8;
    databuf[3] = 0xFF & xend;
    lcd_write_cmd(lcd_self.setxcmd);
    lcd_write_data(databuf,4);

    databuf[0] = ystar >> 8;
    databuf[1] = 0xFF & ystar;
    databuf[2] = yend >> 8;
    databuf[3] = 0xFF & yend;
    lcd_write_cmd(lcd_self.setycmd);
    lcd_write_data(databuf,4);

    lcd_write_cmd(lcd_self.wramcmd);    /* 开始写入GRAM */
}   

/**
 * @brief       以一种颜色清空LCD屏
 * @param       color 清屏颜色
 * @retval      无
 */
void IOT::lcd_clear(uint16_t color)
{
    uint16_t i, j;
    uint8_t data[2] = {0};

    data[0] = color >> 8;
    data[1] = color;
    
    lcd_set_window(0, 0, lcd_self.width - 1, lcd_self.height - 1);

    for(j = 0; j < LCD_BUF_SIZE / 2; j++)
    {
        lcd_buf[j * 2] =  data[0];
        lcd_buf[j * 2 + 1] =  data[1];
    }

    for(i = 0; i < (LCD_TOTAL_BUF_SIZE / LCD_BUF_SIZE); i++)
    {
        lcd_write_data(lcd_buf, LCD_BUF_SIZE);
    }
}

/**
 * @brief       在指定区域内填充单个颜色
 * @param       (sx,sy),(ex,ey):填充矩形对角坐标,区域大小为:(ex - sx + 1) * (ey - sy + 1)
 * @param       color:要填充的颜色(32位颜色,方便兼容LTDC)
 * @retval      无
 */
void IOT::lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    uint16_t i;
    uint16_t j;
    uint16_t width;
    uint16_t height;

    width = ex - sx + 1;
    height = ey - sy + 1;
    lcd_set_window(sx, sy, ex, ey);

    for (i = 0; i < height; i++)
    {
        for (j = 0; j < width; j++)
        {
            lcd_write_data16(color);
        }
    }
    lcd_set_window(sx, sy, ex, ey);
}


/**
 * @brief       设置光标的位置
 * @param       Xpos：左上角x轴
 * @param       Ypos：左上角y轴
 * @retval      无
 */
void IOT::lcd_set_cursor(uint16_t xpos, uint16_t ypos)
{
    lcd_set_window(xpos,ypos,xpos,ypos);	
} 

/**
 * @brief       设置LCD的自动扫描方向(对RGB屏无效)
 * @param       dir:0~7,代表8个方向(具体定义见lcd.h)
 * @retval      无
 */
void IOT::lcd_scan_dir(uint8_t dir)
{
    uint8_t regval = 0;
    uint8_t dirreg = 0;
    uint16_t temp;

    /* 横屏时，对1963不改变扫描方向, 其他IC改变扫描方向！竖屏时1963改变方向, 其他IC不改变扫描方向 */
    if (lcd_self.dir == 1)
    {
        dir = 5;
    }

    /* 根据扫描方式 设置 0X36/0X3600 寄存器 bit 5,6,7 位的值 */
    switch (dir)
    {
        case L2R_U2D:                           /* 从左到右,从上到下 */
            regval |= (0 << 7) | (0 << 6) | (0 << 5);
            break;

        case L2R_D2U:                           /* 从左到右,从下到上 */
            regval |= (1 << 7) | (0 << 6) | (0 << 5);
            break;

        case R2L_U2D:                           /* 从右到左,从上到下 */
            regval |= (0 << 7) | (1 << 6) | (0 << 5);
            break;

        case R2L_D2U:                           /* 从右到左,从下到上 */
            regval |= (1 << 7) | (1 << 6) | (0 << 5);
            break;

        case U2D_L2R:                           /* 从上到下,从左到右 */
            regval |= (0 << 7) | (0 << 6) | (1 << 5);
            break;

        case U2D_R2L:                           /* 从上到下,从右到左 */
            regval |= (0 << 7) | (1 << 6) | (1 << 5);
            break;

        case D2U_L2R:                           /* 从下到上,从左到右 */
            regval |= (1 << 7) | (0 << 6) | (1 << 5);
            break;

        case D2U_R2L:                           /* 从下到上,从右到左 */
            regval |= (1 << 7) | (1 << 6) | (1 << 5);
            break;
    }

    dirreg = 0x36;                              /* 对绝大部分驱动IC, 由0X36寄存器控制 */
    
    uint8_t date_send[1] = {regval};
    
    lcd_write_cmd(dirreg);
    lcd_write_data(date_send,1);
    
    if (regval & 0x20)
    {
        if (lcd_self.width < lcd_self.height)   /* 交换X,Y */
        {
            temp = lcd_self.width;
            lcd_self.width = lcd_self.height;
            lcd_self.height = temp;
        }
    }
    else
    {
        if (lcd_self.width > lcd_self.height)   /* 交换X,Y */
        {
            temp = lcd_self.width;
            lcd_self.width = lcd_self.height;
            lcd_self.height = temp;
        }
    }
    
    lcd_set_window(0, 0, lcd_self.width,lcd_self.height);
}

/**
 * @brief       设置LCD显示方向
 * @param       dir:0,竖屏; 1,横屏
 * @retval      无
 */
void IOT::lcd_display_dir(uint8_t dir)
{
    lcd_self.dir = dir;
    
    if (lcd_self.dir == 0)                  /* 竖屏 */
    {
        lcd_self.width      = 240;
        lcd_self.height     = 320;
        lcd_self.wramcmd    = 0X2C;
        lcd_self.setxcmd    = 0X2A;
        lcd_self.setycmd    = 0X2B;
    }
    else                                    /* 横屏 */
    {
        lcd_self.width      = 320;          /* 默认宽度 */
        lcd_self.height     = 240;          /* 默认高度 */
        lcd_self.wramcmd    = 0X2C;
        lcd_self.setxcmd    = 0X2A;
        lcd_self.setycmd    = 0X2B;
    }

    lcd_scan_dir(DFT_SCAN_DIR);             /* 默认扫描方向 */
}

/**
 * @brief       硬件复位
 * @param       self_in：LCD结构体
 * @retval      无
 */
void IOT::lcd_hard_reset(void)
{
    /* 复位显示屏 */
    LCD_RESET0(0);
    LCD_RESET1(0);
    vTaskDelay(100);
    LCD_RESET0(1);
    LCD_RESET1(1);
    SPI_CS1(0);
    vTaskDelay(100);
}

/**
 * @brief       绘画一个像素点
 * @param       self_in：LCD结构体
 * @param       x：x轴坐标
 * @param       y：y轴坐标
 * @param       color：颜色值
 * @retval      无
 */
void IOT::lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    lcd_set_cursor(x, y);
    lcd_write_data16(color);
}

/**
 * @brief       画线函数(直线、斜线)
 * @param       x1,y1   起点坐标
 * @param       x2,y2   终点坐标
 * @param       color 填充颜色
 * @retval      无
 */
void IOT::lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint16_t t; 
    int xerr = 0, yerr = 0, delta_x, delta_y, distance; 
    
    int incx, incy, urow, ucol; 

    delta_x = x2 - x1;                      /* 计算坐标增量 */
    delta_y = y2 - y1; 
    urow = x1; 
    ucol = y1; 
    
    if (delta_x > 0)
    {
        incx = 1;                           /* 设置单步方向 */
    }
    else if (delta_x == 0)
    {
        incx = 0;                           /* 垂直线 */
    }
    else
    {
        incx =-1;
        delta_x =-delta_x;
    } 
    if(delta_y > 0)
    {
        incy = 1; 
    }
    else if(delta_y == 0)
    {
        incy = 0;                           /* 水平线 */
    }
    else
    {
        incy =-1;
        delta_y=-delta_y;
    } 
    
    if( delta_x>delta_y)
    {
        distance = delta_x;                 /* 选取基本增量坐标轴 */
    }
    else
    {
        distance = delta_y; 
    }
    
    for (t = 0;t <= distance + 1;t++ )      /* 画线输出 */
    {
        lcd_draw_pixel(urow,ucol,color);    /* 画点 */ 
        xerr += delta_x ; 
        yerr += delta_y ; 
        
        if(xerr>distance)
        { 
            xerr -= distance; 
            urow += incx; 
        } 
        
        if (yerr > distance)
        { 
            yerr -= distance; 
            ucol += incy; 
        } 
    } 
}

/**
 * @brief       画水平线
 * @param       x0,y0: 起点坐标
 * @param       len  : 线长度
 * @param       color: 矩形的颜色
 * @retval      无
 */
void IOT::lcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color)
{
    if ((len == 0) || (x > lcd_self.width) || (y > lcd_self.height))return;

    lcd_fill(x, y, x + len - 1, y, color);
}

/**
 * @brief       画一个矩形
 * @param       x1,y1   起点坐标
 * @param       x2,y2   终点坐标
 * @param       color 填充颜色
 * @retval      无
 */
void IOT::lcd_draw_rectangle(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,uint16_t color)
{
    lcd_draw_line(x0, y0, x1, y0,color);
    lcd_draw_line(x0, y0, x0, y1,color);
    lcd_draw_line(x0, y1, x1, y1,color);
    lcd_draw_line(x1, y0, x1, y1,color);
}

/**
 * @brief       画一个圆
 * @param       x0,y0   圆心坐标
 * @param       r   圆半径
 * @param       color 填充颜色
 * @retval      无
 */
void IOT::lcd_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color)
{
    int a, b;
    int di;
    a = 0;
    b = r;
    di = 3 - (r << 1);

    while (a <= b)
    {
        lcd_draw_pixel(x0 - b, y0 - a, color);
        lcd_draw_pixel(x0 + b, y0 - a, color);
        lcd_draw_pixel(x0 - a, y0 + b, color);
        lcd_draw_pixel(x0 - b, y0 - a, color);
        lcd_draw_pixel(x0 - a, y0 - b, color);
        lcd_draw_pixel(x0 + b, y0 + a, color);
        lcd_draw_pixel(x0 + a, y0 - b, color);
        lcd_draw_pixel(x0 + a, y0 + b, color);
        lcd_draw_pixel(x0 - b, y0 + a, color);
        a++;

        if (di < 0)
        {
            di += 4 * a + 6;
        }
        else
        {
            di += 10 + 4 * (a - b);
            b--;
        }

        lcd_draw_pixel(x0 + a, y0 + b, color);
    }
}

/**
 * @brief       在指定位置显示一个字符
 * @param       x,y  : 坐标
 * @param       chr  : 要显示的字符:" "--->"~"
 * @param       size : 字体大小 12/16/24/32
 * @param       mode : 叠加方式(1); 非叠加方式(0);
 * @param       color : 字符的颜色;
 * @retval      无
 */
void IOT::lcd_show_char(uint16_t x, uint16_t y, uint8_t chr, uint8_t size, uint8_t mode, uint16_t color)
{
    // uint8_t temp = 0,t1 = 0, t = 0;
    // uint8_t *pfont = 0;
    // uint8_t csize = 0;                                      /* 得到字体一个字符对应点阵集所占的字节数 */
    // uint16_t colortemp = 0;
    // uint8_t sta = 0;

    // csize = (size / 8 + ((size % 8) ? 1 : 0)) * (size / 2); /* 得到字体一个字符对应点阵集所占的字节数 */
    // chr = chr - ' ';                                        /* 得到偏移后的值（ASCII字库是从空格开始取模，所以-' '就是对应字符的字库） */

    // if ((x > (lcd_self.width - size / 2)) || (y > (lcd_self.height - size)))
    // {
    //     return;
    // }

    // lcd_set_window(x, y, x + size / 2 - 1, y + size - 1);   /* (x,y,x+8-1,y+16-1) */

    // switch (size)
    // {
    //     case 12:
    //         pfont = (uint8_t *)asc2_1206[chr];              /* 调用1206字体 */
    //         sta = 6;
    //         break;

    //     case 16:
    //         pfont = (uint8_t *)asc2_1608[chr];              /* 调用1608字体 */
    //         sta = 8;
    //         break;

    //     case 24:
    //         pfont = (uint8_t *)asc2_2412[chr];              /* 调用2412字体 */
    //         break;

    //     case 32:
    //         pfont = (uint8_t *)asc2_3216[chr];              /* 调用3216字体 */
    //         sta = 8;
    //         break;

    //     default:
    //         return ;
    // }

    // if (size != 24)
    // {
    //     csize = (size / 8 + ((size % 8) ? 1 : 0)) * (size / 2);
        
    //     for (t = 0; t < csize; t++)
    //     {
    //         temp = pfont[t];                                /* 获取字符的点阵数据 */

    //         for (t1 = 0; t1 < sta; t1++)
    //         {
    //             if (temp & 0x80)
    //             {
    //                 colortemp = color;
    //             }
    //             else if (mode == 0)                     /* 无效点,不显示 */
    //             {
    //                 colortemp = 0xFFFF;
    //             }

    //             lcd_write_data16(colortemp);
    //             temp <<= 1;
    //         }
    //     }
    // }
    // else
    // {
    //     csize = (size * 16) / 8;
        
    //     for (t = 0; t < csize; t++)
    //     {
    //         temp = asc2_2412[chr][t];

    //         if (t % 2 == 0)
    //         {
    //             sta = 8;
    //         }
    //         else
    //         {
    //             sta = 4;
    //         }

    //         for (t1 = 0; t1 < sta; t1++)
    //         {
    //             if(temp & 0x80)
    //             {
    //                 colortemp = color;
    //             }
    //             else if (mode == 0)                         /* 无效点,不显示 */
    //             {
    //                 colortemp = 0xFFFF;
    //             }

    //             lcd_write_data16(colortemp);
    //             temp <<= 1;
    //         }
    //     }
    // }
}

/**
 * @brief       m^n函数
 * @param       m,n     输入参数
 * @retval      m^n次方
 */
uint32_t IOT::lcd_pow(uint8_t m, uint8_t n)
{
    uint32_t result = 1;

    while(n--)result *= m;

    return result;
}

/**
 * @brief       显示len个数字
 * @param       x,y : 起始坐标
 * @param       num : 数值(0 ~ 2^32)
 * @param       len : 显示数字的位数
 * @param       size: 选择字体 12/16/24/32
 * @retval      无
 */
void IOT::lcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color)
{
    uint8_t t, temp;
    uint8_t enshow = 0;

    for (t = 0; t < len; t++)                                               /* 按总显示位数循环 */
    {
        temp = (num / lcd_pow(10, len - t - 1)) % 10;                       /* 获取对应位的数字 */

        if (enshow == 0 && t < (len - 1))                                   /* 没有使能显示,且还有位要显示 */
        {
            if (temp == 0)
            {
                lcd_show_char(x + (size / 2)*t, y, ' ', size, 0, color);    /* 显示空格,占位 */
                continue;                                                   /* 继续下个一位 */
            }
            else
            {
                enshow = 1;                                                 /* 使能显示 */
            }

        }

        lcd_show_char(x + (size / 2)*t, y, temp + '0', size, 0, color);     /* 显示字符 */
    }
}

/**
 * @brief       扩展显示len个数字(高位是0也显示)
 * @param       x,y : 起始坐标
 * @param       num : 数值(0 ~ 2^32)
 * @param       len : 显示数字的位数
 * @param       size: 选择字体 12/16/24/32
 * @param       mode: 显示模式
 *              [7]:0,不填充;1,填充0.
 *              [6:1]:保留
 *              [0]:0,非叠加显示;1,叠加显示.
 * @param       color : 数字的颜色;
 * @retval      无
 */
void IOT::lcd_show_xnum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint8_t mode, uint16_t color)
{
    uint8_t t, temp;
    uint8_t enshow = 0;

    for (t = 0; t < len; t++)                                                           /* 按总显示位数循环 */
    {
        temp = (num / lcd_pow(10, len - t - 1)) % 10;                                   /* 获取对应位的数字 */

        if (enshow == 0 && t < (len - 1))                                               /* 没有使能显示,且还有位要显示 */
        {
            if (temp == 0)
            {
                if (mode & 0X80)                                                        /* 高位需要填充0 */
                {
                    lcd_show_char(x + (size / 2)*t, y, '0', size, mode & 0X01, color);  /* 用0占位 */
                }
                else
                {
                    lcd_show_char(x + (size / 2)*t, y, ' ', size, mode & 0X01, color);  /* 用空格占位 */
                }
                continue;
            }
            else
            {
                enshow = 1;                                                             /* 使能显示 */
            }
        }
        lcd_show_char(x + (size / 2)*t, y, temp + '0', size, mode & 0X01, color);
    }
}


/**
 * @brief       显示字符串
 * @param       x,y         : 起始坐标
 * @param       width,height: 区域大小
 * @param       size        : 选择字体 12/16/24/32
 * @param       p           : 字符串首地址
 * @retval      无
 */
void IOT::lcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color)
{
    uint8_t x0 = x;
    width += x;
    height += y;

    while ((*p <= '~') && (*p >= ' '))   /* 判断是不是非法字符! */
    {
        if (x >= width)
        {
            x = x0;
            y += size;
        }

        if (y >= height)break;  /* 退出 */

        lcd_show_char(x, y, *p, size, 0, color);
        x += size / 2;
        p++;
    }
}

/**
 * @brief       打开LCD
 * @param       self_in：SPI控制块
 * @retval      mp_const_none：初始化成功
 */
void IOT::lcd_on(void)
{
    //LCD_PWR(1);
    vTaskDelay(10);
}

/**
 * @brief       关闭LCD
 * @param       self_in：SPI控制块
 * @retval      mp_const_none：初始化成功
 */
void IOT::lcd_off(void)
{
    //LCD_PWR(0);
    vTaskDelay(10);
}

/**
 * @brief       LCD初始化
 * @param       无
 * @retval      无
 */
void IOT::lcd_init(void)
{
    int cmd = 0;
    esp_err_t ret = 0;
    
    lcd_self.dir = 0;
    lcd_self.wr = LCD_NUM_WR;                                       /* 配置WR引脚 */
    lcd_self.cs = LCD_NUM_CS0;                                       /* 配置CS引脚 */
    
    gpio_config_t gpio_init_struct;

    /* SPI驱动接口配置 */
    spi_device_interface_config_t devcfg = {
        .mode = 0,                                                  /* SPI模式0 */
        .clock_speed_hz = 60 * 1000 * 1000,                         /* SPI时钟 */
        .spics_io_num = lcd_self.cs,                                /* SPI设备引脚 */
        .queue_size = 7,                                            /* 事务队列尺寸 7个 */
    };
    
    /* 添加SPI总线设备 */
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &MY_LCD_Handle);   /* 配置SPI总线设备 */
    ESP_ERROR_CHECK(ret);

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;                 /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;                       /* 配置输出模式 */
    gpio_init_struct.pin_bit_mask = 1ull << lcd_self.wr;            /* 配置引脚位掩码 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;          /* 失能下拉 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;               /* 使能下拉 */
    gpio_config(&gpio_init_struct);                                 /* 引脚配置 */

    lcd_hard_reset();                                               /* LCD硬件复位 */

    /* 初始化代码 */
#if SPI_LCD_TYPE                                                    /* 对2.4寸LCD寄存器进行设置 */
    lcd_init_cmd_t ili_init_cmds[] =
    {
        {0x11, {0}, 0x80},
        {0x36, {0x00}, 1},
        {0x3A, {0x65}, 1},
        {0X21, {0}, 0x80},
        {0x29, {0}, 0x80},
        {0, {0}, 0xff},
    };

#else                                                               /* 不为0则视为使用1.3寸SPILCD屏，那么屏幕将不会反显 */
    lcd_init_cmd_t ili_init_cmds[] =
    {
        {0x11, {0}, 0x80},
        {0x36, {0x00}, 1},
        {0x3A, {0x65}, 1},
        {0xB2, {0x0C, 0x0C, 0x00, 0x33,0x33}, 5},
        {0xB7, {0x75}, 1},
        {0xBB, {0x1C}, 1},
        {0xC0, {0x2c}, 1},
        {0xC2, {0x01}, 1},
        {0xC3, {0x0F}, 1},
        {0xC4, {0x20}, 1},
        {0xC6, {0X01}, 1},
        {0xD0, {0xA4,0xA1}, 2},
        {0xE0, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 14},
        {0xE1, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 14},
        {0X21, {0}, 0x80},
        {0x29, {0}, 0x80},
        {0, {0}, 0xff},
    };
#endif

    /* 循环发送设置所有寄存器 */
    while (ili_init_cmds[cmd].databytes != 0xff)
    {
        lcd_write_cmd(ili_init_cmds[cmd].cmd);
        lcd_write_data(ili_init_cmds[cmd].data, ili_init_cmds[cmd].databytes & 0x1F);
        
        if (ili_init_cmds[cmd].databytes & 0x80)
        {
            vTaskDelay(120);
        }
        
        cmd++;
    }

    lcd_display_dir(1);                                             /* 设置屏幕方向 */
    //LCD_PWR(1);
    lcd_clear(WHITE);                                               /* 清屏 */
}





/**
 ****************************************************************************************************
 * @file        spi.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2023-08-26
 * @brief       SPI驱动代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 ESP32-S3 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

/**
 * @brief       初始化SPI
 * @param       无
 * @retval      无
 */
void IOT::spi2_init(void)
{
    esp_err_t ret = 0;
    spi_bus_config_t spi_bus_conf = {0};

    /* SPI总线配置 */
    spi_bus_conf.miso_io_num = SPI_MISO_GPIO_PIN;                               /* SPI_MISO引脚 */
    spi_bus_conf.mosi_io_num = SPI_MOSI_GPIO_PIN;                               /* SPI_MOSI引脚 */
    spi_bus_conf.sclk_io_num = SPI_CLK_GPIO_PIN;                                /* SPI_SCLK引脚 */
    spi_bus_conf.quadwp_io_num = -1;                                            /* SPI写保护信号引脚，该引脚未使能 */
    spi_bus_conf.quadhd_io_num = -1;                                            /* SPI保持信号引脚，该引脚未使能 */
    spi_bus_conf.max_transfer_sz = 240 * 240 * 2;                               /* 配置最大传输大小，以字节为单位 */
    
    /* 初始化SPI总线 */
    ret = spi_bus_initialize(SPI2_HOST, &spi_bus_conf, SPI_DMA_CH_AUTO);        /* SPI总线初始化 */
    ESP_ERROR_CHECK(ret);                                                       /* 校验参数值 */
}

/**
 * @brief       SPI发送命令
 * @param       handle : SPI句柄
 * @param       cmd    : 要发送命令
 * @retval      无
 */
void IOT::spi2_write_cmd(spi_device_handle_t handle, uint8_t cmd)
{
    esp_err_t ret;
    spi_transaction_t t = {0};

    t.length = 8;                                       /* 要传输的位数 一个字节 8位 */
    t.tx_buffer = &cmd;                                 /* 将命令填充进去 */
    ret = spi_device_polling_transmit(handle, &t);      /* 开始传输 */
    ESP_ERROR_CHECK(ret);                               /* 一般不会有问题 */
}

/**
 * @brief       SPI发送数据
 * @param       handle : SPI句柄
 * @param       data   : 要发送的数据
 * @param       len    : 要发送的数据长度 
 * @retval      无
 */
void IOT::spi2_write_data(spi_device_handle_t handle, const uint8_t *data, int len)
{
    esp_err_t ret;
    spi_transaction_t t = {0};

    if (len == 0)
    {
        return;                                     /* 长度为0 没有数据要传输 */
    }

    t.length = len * 8;                             /* 要传输的位数 一个字节 8位 */
    t.tx_buffer = data;                             /* 将命令填充进去 */
    ret = spi_device_polling_transmit(handle, &t);  /* 开始传输 */
    ESP_ERROR_CHECK(ret);                           /* 一般不会有问题 */
}

/**
 * @brief       SPI处理数据
 * @param       handle       : SPI句柄
 * @param       data         : 要发送的数据 
 * @retval      t.rx_data[0] : 接收到的数据
 */
uint8_t IOT::spi2_transfer_byte(spi_device_handle_t handle, uint8_t data)
{
    spi_transaction_t t;

    memset(&t, 0, sizeof(t));

    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 8;
    t.tx_data[0] = data;
    spi_device_transmit(handle, &t);

    return t.rx_data[0];
}




/**
 * @brief       初始化MYIIC
 * @param       无
 * @retval      ESP_OK:初始化成功
 */
esp_err_t IOT::myiic_init(void)
{
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port                       = IIC_NUM_PORT,         /* I2C端口 */
        .sda_io_num                     = IIC_SDA_GPIO_PIN,     /* SDA管脚 */
        .scl_io_num                     = IIC_SCL_GPIO_PIN,     /* SCL管脚 */ 
        .clk_source                     = I2C_CLK_SRC_DEFAULT,  /* 时钟源 */
        .glitch_ignore_cnt              = 7,                    /* 故障周期 */
        // .flags.enable_internal_pullup   = 1    //true                 /* 内部上拉 */
    };
    /* 新建I2C总线 */
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &bus_handle));

    return ESP_OK;
}

void IOT::TFT_init(void)	//GC9A01
  {
    int cmd = 0;
    esp_err_t ret = 0;
    
    lcd_self.dir = 0;
    lcd_self.wr = LCD_NUM_WR;                                       /* 配置WR引脚 */
    lcd_self.cs = GPIO_NUM_21; // LCD_NUM_CS1; /* 配置CS引脚 */
    lcd_self.wramcmd = 0x2C;
    lcd_self.setxcmd = 0x2A;
    lcd_self.setycmd = 0x2B;
    lcd_self.width  = 240;
    lcd_self.height = 240;
    
    gpio_config_t gpio_init_struct;

    /* SPI驱动接口配置 */
    spi_device_interface_config_t devcfg = {
        .mode = 0,                                                  /* SPI模式0 */
        .clock_speed_hz = 20 * 1000 * 1000,                         /* SPI时钟 */
        .spics_io_num = lcd_self.cs,                                /* SPI设备引脚 */
        .queue_size = 7,                                            /* 事务队列尺寸 7个 */
    };
    
    /* 添加SPI总线设备 */
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &MY_LCD_Handle);   /* 配置SPI总线设备 */
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "spi_bus_add_device %d", ret);

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;                 /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;                       /* 配置输出模式 */
    gpio_init_struct.pin_bit_mask = 1ull << lcd_self.wr;            /* 配置引脚位掩码 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;          /* 失能下拉 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;               /* 使能下拉 */
    gpio_config(&gpio_init_struct);                                 /* 引脚配置 */

    lcd_hard_reset();                                               /* LCD硬件复位 */
#if 0
     lcd_init_cmd_t ili_init_cmds[] =
    {
        {0xEF, {0}, 0x80},
        {0xEB, {0x14}, 1},
        {0xFE, {0}, 0x80},
        {0xEF, {0}, 0x80},
        {0xEB, {0x14}, 1},
        {0x84, {0x40}, 1},
        {0x85, {0xFF}, 1},
        {0x86, {0xFF}, 1},
        {0x87, {0xFF}, 1},
        {0x88, {0x0A}, 1},
        {0x89, {0x21}, 1},
        {0x8A, {0x00}, 1},
        {0x8B, {0x80}, 1},
        {0x8C, {0x01}, 1},
        {0x8D, {0x01}, 1},
        {0x8E, {0xFF}, 1},
        {0x8F, {0xFF}, 1},
        {0XB6, {0x00,0x20}, 2},
        {0x8E, {0xFF}, 1},
        {0x36, {0x08}, 1},
        {0x3A, {0x05}, 1},
        {0x90, {0x08, 0x08,0x08,0x08}, 4},
        {0xBD, {0x06}, 1},
        {0xBC, {0x00}, 1},
        {0xFF, {0x60,0x01,0x04}, 1},
        {0xC3, {0x13}, 1},
        {0xC4, {0x13}, 1},
        {0xC9, {0x22}, 1},
        {0xBE, {0x11}, 1},
        {0xE1, {0x10,0x0E}, 2},
        {0xDF, {0x21,0x0C,0x02}, 1},
        {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},
        {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},
        {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},
        {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},
        {0xED, {0x1B, 0x0B}, 2},
        {0XAE, {0x77}, 1},
        {0XCD, {0x63}, 1},
        {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9},
        {0xE8, {0x34}, 1},
        {0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12},
        {0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12},
        {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7},
        {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10},
        {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10},
        {0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7},
        {0x98, {0x3e,0x07}, 2},
        {0x34, {0}, 1},
        {0x21, {0}, 1},
        {0x11, {0}, 1},
        {0, {0}, 0xff},
    };
#endif
    lcd_init_cmd_t ili_init_cmds[] = {
         {0xFE, {0}, 0},                       // 解锁
         {0xEF, {0}, 0},    {0x36, {0x08}, 1}, // 地址模式
         {0x3A, {0x05}, 1},                    // 16 位色
         {0x21, {0}, 0x80},                    // 反转显示
         {0x11, {0}, 0x80},                    // 退出睡眠
         {0x29, {0}, 0x80},                    // 开显示
         {0, {0}, 0xFF},
     };
     
	while (ili_init_cmds[cmd].databytes != 0xff)
    {
        lcd_write_cmd(ili_init_cmds[cmd].cmd);
        lcd_write_data(ili_init_cmds[cmd].data, ili_init_cmds[cmd].databytes & 0x1F);
        
        if (ili_init_cmds[cmd].databytes & 0x80)
        {
            vTaskDelay(120);
        }
        
        cmd++;
    }
	  vTaskDelay(120); 
	  lcd_write_cmd(0x29); 	//显示开
	  vTaskDelay(20);
}

void IOT::lcd_pic(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, const uint8_t *data, int len)
{
    uint16_t i;
    uint16_t j;
    uint16_t width;
    uint16_t height;

    width = ex - sx + 1;
    height = ey - sy + 1;
    lcd_set_window(sx, sy, ex, ey);

    for (i = 0; i < height; i++)
    {
        for (j = 0; j < width; j++)
        {
            lcd_write_data(data, len);
        }
    }
    lcd_set_window(sx, sy, ex, ey);
}
