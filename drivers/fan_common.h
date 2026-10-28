#ifndef FAN_COMMON_H
#define FAN_COMMON_H
#include <linux/ioctl.h>
#define FAN_SPEED_OFF 0
#define FAN_SPEED_WEAK 70
#define FAN_SPEED_MID 90
#define FAN_SPEED_STRONG 100
#define FAN_SPEED_BREEZE 60
#define LED_IOC_MAGIC 'L'
#define LED_IOC_SET_LEVEL _IOW(LED_IOC_MAGIC, 1, int)

/* Kernel API exported by LED driver. Input: actual PWM duty (0..100%). */
int led_set_duty(unsigned int duty);
struct bmp180_data {
    int temp_x10;
    int pressure_pa;
};
#define BMP180_IOC_MAGIC 'B'
#define BMP180_IOC_READ _IOR(BMP180_IOC_MAGIC, 1, struct bmp180_data)
int bmp180_get_data(struct bmp180_data *out);
/* Rotary Encoder FIFO API exported by lt_en.ko */
#define _CW 1
#define _CCW 2
int _cw_ccw_event(void);

#endif
