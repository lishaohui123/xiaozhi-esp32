#ifndef __IOT_H
#define __IOT_H

#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"


#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"


#include "driver/i2c_master.h"
#include "esp_err.h"


/* 引脚以及重要参数定义 */
#define LEDC_PWM_TIMER          LEDC_TIMER_0        /* 使用定时器(0~3) */
#define LEDC_PWM_CH0_GPIO       GPIO_NUM_15          /* LED控制器通道对应GPIO */
#define LEDC_PWM_CH1_GPIO       GPIO_NUM_16          /* LED控制器通道对应GPIO */
#define LEDC_PWM_CH2_GPIO       GPIO_NUM_17          /* LED控制器通道对应GPIO */
#define LEDC_PWM_CH3_GPIO       GPIO_NUM_40          /* LED控制器通道对应GPIO */
#define LEDC_PWM_CH4_GPIO       GPIO_NUM_1          /* LED控制器通道对应GPIO */
#define LEDC_PWM_CH0_CHANNEL    LEDC_CHANNEL_0      /* LED控制器通道号(0~7) */

/* 管脚声明 */
#define ADC_CHAN    ADC_CHANNEL_7       /* 对应管脚为GPIO8 */


/* 引脚定义 */
#define LED_GPIO_PIN    GPIO_NUM_2  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */
enum GPIO_OUTPUT_STATE
{
    PIN_RESET,
    PIN_SET
};

/* LED端口定义 */
#define LED(x)          do { x ?                                      \
                             gpio_set_level(LED_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(LED_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */

/* LED取反定义 */
#define LED_TOGGLE()    do { gpio_set_level(LED_GPIO_PIN, !gpio_get_level(LED_GPIO_PIN)); } while(0)  /* LED翻转 */

/* 引脚定义 */
#define LCD_RESET0_GPIO_PIN    GPIO_NUM_19  /* L ED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define LCD_RESET0(x)          do { x ?                                      \
                             gpio_set_level(LCD_RESET0_GPIO_PIN, 1) :  \
                             gpio_set_level(LCD_RESET0_GPIO_PIN, 0); \
                        } while(0)  /* LED翻转 */

/* LED取反定义 */
#define LCD_RESET0_TOGGLE()    do { gpio_set_level(LCD_RESET0_GPIO_PIN, !gpio_get_level(LCD_RESET0_GPIO_PIN)); } while(0)  /* LED翻转 */


/* 引脚定义 */
#define LCD_RESET1_GPIO_PIN    GPIO_NUM_38  /* L ED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define LCD_RESET1(x)          do { x ?                                      \
                             gpio_set_level(LCD_RESET1_GPIO_PIN, 1) :  \
                             gpio_set_level(LCD_RESET1_GPIO_PIN, 0); \
                        } while(0)  /* LED翻转 */


/* 引脚定义 */
#define LED_RED_GPIO_PIN    GPIO_NUM_15  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define LED_RED(x)          do { x ?                                      \
                             gpio_set_level(LED_RED_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(LED_RED_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define LED_GREEN_GPIO_PIN    GPIO_NUM_16  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define LED_GREEN(x)          do { x ?                                      \
                             gpio_set_level(LED_GREEN_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(LED_GREEN_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define Servo_Switch_H_GPIO_PIN    GPIO_NUM_17  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define Servo_Switch_H(x)          do { x ?                                      \
                             gpio_set_level(Servo_Switch_H_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(Servo_Switch_H_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define S_FW_REV_H_GPIO_PIN    GPIO_NUM_18  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define S_FW_REV_H(x)          do { x ?                                      \
                             gpio_set_level(S_FW_REV_H_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(S_FW_REV_H_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define Servo_Switch_V_GPIO_PIN    GPIO_NUM_40  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define Servo_Switch_V(x)          do { x ?                                      \
                             gpio_set_level(Servo_Switch_V_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(Servo_Switch_V_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define S_FW_REV_V_GPIO_PIN    GPIO_NUM_39  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define S_FW_REV_V(x)          do { x ?                                      \
                             gpio_set_level(S_FW_REV_V_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(S_FW_REV_V_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define SPK_EN_GPIO_PIN    GPIO_NUM_20  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define SPK_EN(x)          do { x ?                                      \
                             gpio_set_level(SPK_EN_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(SPK_EN_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define SPI_CS0_GPIO_PIN    GPIO_NUM_21  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define SPI_CS0(x)          do { x ?                                      \
                             gpio_set_level(SPI_CS0_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(SPI_CS0_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define SPI_CS1_GPIO_PIN    GPIO_NUM_47  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define SPI_CS1(x)          do { x ?                                      \
                             gpio_set_level(SPI_CS1_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(SPI_CS1_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */



#define BACKL0_GPIO_PIN    GPIO_NUM_48  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define BACKL0(x)          do { x ?                                      \
                             gpio_set_level(BACKL0_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(BACKL0_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define BACKL1_GPIO_PIN    GPIO_NUM_45  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define BACKL1(x)          do { x ?                                      \
                             gpio_set_level(BACKL1_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(BACKL1_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define Wheel_Speed_GPIO_PIN    GPIO_NUM_1 /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define Wheel_Speed(x)          do { x ?                                      \
                             gpio_set_level(Wheel_Speed_GPIO_PIN, PIN_SET) :  \
                             gpio_set_level(Wheel_Speed_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */


#define Wheel_Switch_GPIO_PIN    GPIO_NUM_4  /* LED连接的GPIO端口 */

/* 引脚的输出的电平状态 */

/* LED端口定义 */
#define Wheel_Switch(x)          do { x ?                                      \
                             gpio_set_level(Wheel_Switch_GPIO_PIN, 1) :  \
                             gpio_set_level(Wheel_Switch_GPIO_PIN, 0); \
                        } while(0)  /* LED翻转 */

/* LED取反定义 */
#define Wheel_Switch_TOGGLE()    do { gpio_set_level(Wheel_Switch_GPIO_PIN, !gpio_get_level(Wheel_Switch_GPIO_PIN)); } while(0)  /* LED翻转 */




/* 引脚定义 */
#define LCD_NUM_WR      GPIO_NUM_13   //SPI_MISO 屏幕数据读取
#define LCD_NUM_CS0     GPIO_NUM_21   //SPI_CS0  屏幕0使能引脚
#define LCD_NUM_CS1     GPIO_NUM_47   //SPI_CS0  屏幕1使能引脚
/* IO操作 */
#define LCD_WR(x)       do{ x ? \
                            (gpio_set_level(LCD_NUM_WR, 1)):    \
                            (gpio_set_level(LCD_NUM_WR, 0));    \
                        }while(0)

#define LCD_CS(x)       do{ x ? \
                            (gpio_set_level(LCD_NUM_CS0, 1)):    \
                            (gpio_set_level(LCD_NUM_CS0, 0));    \
                        }while(0)

/*#define LCD_PWR(x)       do{ x ? \
                           (gpio_set_level(LCD_NUM_CS, 1)):    \
                            (gpio_set_level(LCD_NUM_CS, 0)); 
                        }while(0)*/

#define LCD_RST(x)       do{ x ? \
                            ( gpio_set_level(LCD_RESET1_GPIO_PIN, 1)) :  \
                            ( gpio_set_level(LCD_RESET1_GPIO_PIN, 0)); \
                        }while(0)

/* 常用颜色值 */
#define WHITE           0xFFFF      /* 白色 */
#define BLACK           0x0000      /* 黑色 */
#define RED             0xF800      /* 红色 */
#define GREEN           0x07E0      /* 绿色 */
#define BLUE            0x001F      /* 蓝色 */ 
#define MAGENTA         0XF81F      /* 品红色/紫红色 = BLUE + RED */
#define YELLOW          0XFFE0      /* 黄色 = GREEN + RED */
#define CYAN            0X07FF      /* 青色 = GREEN + BLUE */  

/* 非常用颜色 */
#define BROWN           0XBC40      /* 棕色 */
#define BRRED           0XFC07      /* 棕红色 */
#define GRAY            0X8430      /* 灰色 */ 
#define DARKBLUE        0X01CF      /* 深蓝色 */
#define LIGHTBLUE       0X7D7C      /* 浅蓝色 */ 
#define GRAYBLUE        0X5458      /* 灰蓝色 */ 
#define LIGHTGREEN      0X841F      /* 浅绿色 */  
#define LGRAY           0XC618      /* 浅灰色(PANNEL),窗体背景色 */ 
#define LGRAYBLUE       0XA651      /* 浅灰蓝色(中间层颜色) */ 
#define LBBLUE          0X2B12      /* 浅棕蓝色(选择条目的反色) */ 

/* 扫描方向定义 */
#define L2R_U2D         0           /* 从左到右,从上到下 */
#define L2R_D2U         1           /* 从左到右,从下到上 */
#define R2L_U2D         2           /* 从右到左,从上到下 */
#define R2L_D2U         3           /* 从右到左,从下到上 */
#define U2D_L2R         4           /* 从上到下,从左到右 */
#define U2D_R2L         5           /* 从上到下,从右到左 */
#define D2U_L2R         6           /* 从下到上,从左到右 */
#define D2U_R2L         7           /* 从下到上,从右到左 */

#define DFT_SCAN_DIR    L2R_U2D     /* 默认的扫描方向 */

/* 屏幕选择 */
#define LCD_320X240     0
#define LCD_240X240     1

/* LCD缓存大小设置，修改此值时请注意！！！！修改这两个值时可能会影响以下函数 lcd_clear/lcd_fill/lcd_draw_line */
#define LCD_TOTAL_BUF_SIZE      (240 * 240 * 2)
#define LCD_BUF_SIZE            15360


/* 引脚定义 */
#define SPI_MOSI_GPIO_PIN   GPIO_NUM_11         /* SPI2_MOSI */
#define SPI_CLK_GPIO_PIN    GPIO_NUM_12         /* SPI2_CLK */
#define SPI_MISO_GPIO_PIN   GPIO_NUM_13         /* SPI2_MISO */


    /* 引脚与相关参数定义 */
#define IIC_NUM_PORT       I2C_NUM_0        /* IIC0 */
#define IIC_SPEED_CLK      400000           /* 速率400K */
#define IIC_SDA_GPIO_PIN   GPIO_NUM_41      /* IIC0_SDA引脚 */
#define IIC_SCL_GPIO_PIN   GPIO_NUM_42      /* IIC0_SCL引脚 */

#define __LCD_VERSION__  "1.0"
#define SPI_LCD_TYPE    1           /* SPI接口屏幕类型（1：2.4寸SPILCD  0：1.3寸SPILCD） */  

/* 导出相关变量 */
// extern lcd_obj_t lcd_self;
// extern uint8_t lcd_buf[LCD_TOTAL_BUF_SIZE];

// extern i2c_master_bus_handle_t bus_handle;  /* 总线句柄 */

/* LEDC配置结构体 */
typedef struct
{
    ledc_clk_cfg_t clk_cfg;             /* 时钟源配置（LEDC_USE_XTAL_CLK\LEDC_USE_PLL_DIV_CLK\LEDC_USE_RC_FAST_CLK或者LEDC_AUTO_CLK(自动选择)） */
    ledc_timer_t  timer_num;            /* 定时器（LEDC_TIMER_0~LEDC_TIMER_3） */
    uint32_t freq_hz;                   /* 频率（系统自动计算分频系数） */
    ledc_timer_bit_t duty_resolution;   /* 占空比分辨率 */
    ledc_channel_t channel;             /* 通道（LEDC_CHANNEL_0~LEDC_CHANNEL_7） */
    uint32_t duty;                      /* 初始占空比 */
    int gpio_num;                       /* PWM输出管脚 */
}ledc_config_t;

/* LCD信息结构体 */
typedef struct _lcd_obj_t
{
    uint16_t        width;          /* 宽度 */
    uint16_t        height;         /* 高度 */
    uint8_t         dir;            /* 横屏还是竖屏控制：0，竖屏；1，横屏。 */
    uint16_t        wramcmd;        /* 开始写gram指令 */
    uint16_t        setxcmd;        /* 设置x坐标指令 */
    uint16_t        setycmd;        /* 设置y坐标指令 */
    uint16_t        wr;             /* 命令/数据IO */
    uint16_t        cs;             /* 片选IO */
} lcd_obj_t;

/* LCD需要初始化一组命令/参数值。它们存储在此结构中  */
typedef struct
{
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes; /* 数据中没有数据；比特7＝设置后的延迟；0xFF=cmds结束 */
} lcd_init_cmd_t;


class IOT {
public:
    IOT();
    ~IOT();
    uint32_t ledc_duty_pow(uint32_t duty, uint8_t m, uint8_t n);
    esp_err_t led_red_pwm_set_duty(uint16_t duty);
    esp_err_t led_green_pwm_set_duty(uint16_t duty);
    esp_err_t servo_switch_h_pwm_set_duty(uint16_t duty);
    esp_err_t servo_switch_v_pwm_set_duty(uint16_t duty);
    esp_err_t wheel_speed_pwm_set_duty(uint16_t duty);
    uint32_t lcd_pow(uint8_t m, uint8_t n);
    void lcd_write_cmd(const uint8_t cmd);
    void lcd_display_dir(uint8_t dir);

    void lcd_off(void);
    void lcd_on(void);
    void Wheel_Speed_PWM_init(void);
    void lcd_hard_reset(void);
    void lcd_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color);

    /* 函数声明 */
    void ledc_init(ledc_config_t *ledc_config);                             /* ledc初始化 */
    void ledc_pwm_set_duty(ledc_config_t *ledc_config, uint16_t duty);      /* PWM占空比设置 */

    void LED_RED_PWM_init(void);
    void LED_GREEN_PWM_init(void);
    void Servo_Switch_H_PWM_init(void);
    void Servo_Switch_V_PWM_init(void);


    /* 函数声明 */
    void adc_init(void);                                               /* 初始化ADC */
    uint32_t adc_get_result_average(uint32_t times); /* 获取ADC转换且进行均值滤波后的结果 */

    /* 函数声明*/
    void led_init(void);    /* 初始化LED */

    /* 函数声明*/
    void LCD_RESET0_init(void);    /* 初始化LED */

    /* 函数声明*/
    void LCD_RESET1_init(void);    /* 初始化LED */

    /* 函数声明*/
    void LED_RED_init(void);    /* 初始化LED */

    /* 函数声明*/
    void LED_GREEN_init(void);    /* 初始化LED */

    /* 函数声明*/
    void Servo_Switch_H_init(void);    /* 初始化LED */

    /* 函数声明*/
    void S_FW_REV_H_init(void);    /* 初始化LED */

    /* 函数声明*/
    void Servo_Switch_V_init(void);    /* 初始化LED */

    /* 函数声明*/
    void S_FW_REV_V_init(void);    /* 初始化LED */

    /* 函数声明*/
    void SPK_EN_init(void);    /* 初始化LED */

    /* 函数声明*/
    void SPI_CS0_init(void);    /* 初始化LED */

    /* 函数声明*/
    void SPI_CS1_init(void);    /* 初始化LED */

    /* 函数声明*/
    void BACKL0_init(void);    /* 初始化LED */

    /* 函数声明*/
    void BACKL1_init(void);    /* 初始化LED */

    /* 函数声明*/
    void Wheel_Speed_init(void);    /* 初始化LED */

    /* 函数声明*/
    void Wheel_Switch_init(void);    /* 初始化LED */  
    
    /* 函数声明 */
    void lcd_init(void);                                                                                                    /* 初始化LCD */
    void lcd_clear(uint16_t color);                                                                                         /* 清屏函数 */
    void lcd_scan_dir(uint8_t dir);                                                                                         /* 设置LCD的自动扫描方向 */
    void lcd_write_data(const uint8_t *data, int len);                                                                      /* 发送数据到LCD */
    void lcd_write_data16(uint16_t data);                                                                                   /* 发送16位数据到LCD */
    void lcd_set_cursor(uint16_t xpos, uint16_t ypos);                                                                      /* 设置光标的位置 */
    void lcd_set_window(uint16_t xstar, uint16_t ystar,uint16_t xend,uint16_t yend);                                        /* 设置窗口大小 */
    void lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);                                      /* 在指定区域内填充单个颜色 */
    void lcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color);                     /* 显示len个数字 */
    void lcd_show_xnum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint8_t mode, uint16_t color);      /* 扩展显示len个数字 */
    void lcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color);   /* 显示字符串 */
    void lcd_draw_rectangle(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,uint16_t color);                             /* 画一个矩形 */
    void lcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color);                                              /* 画水平线 */
    void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,uint16_t color);                                  /* 画线函数(直线、斜线) */
    void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);                                                            /* 绘画一个像素点 */
    void lcd_show_char(uint16_t x, uint16_t y, uint8_t chr, uint8_t size, uint8_t mode, uint16_t color);                    /* 在指定位置显示一个字符 */

    /* 函数声明 */
    void spi2_init(void);                                                               /* 初始化SPI2 */
    void spi2_write_cmd(spi_device_handle_t handle, uint8_t cmd);                       /* SPI发送命令 */
    void spi2_write_data(spi_device_handle_t handle, const uint8_t *data, int len);     /* SPI发送数据 */
    uint8_t spi2_transfer_byte(spi_device_handle_t handle, uint8_t byte);               /* SPI处理数据 */

    /* 函数声明 */
    esp_err_t myiic_init(void);                 /* 初始化MYIIC */

    void TFT_init(void);	

    void lcd_pic(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, const uint8_t *data, int len);
    
public:
    adc_oneshot_unit_handle_t adc_handle = NULL;    /* ADC句柄 */
    i2c_master_bus_handle_t bus_handle;     /* 总线句柄 */
    spi_device_handle_t MY_LCD_Handle;
    uint8_t lcd_buf[LCD_TOTAL_BUF_SIZE];
    lcd_obj_t lcd_self;

};
#endif
