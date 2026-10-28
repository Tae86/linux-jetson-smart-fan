#ifndef LED_BAR_H
#define LED_BAR_H

/*
 * led_b_dev.ko(장치 등록) -> led_b.ko(드라이버)로 넘기는 설정.
 * 두 모듈이 같은 정의를 써야 하므로 이 헤더를 같이 include 한다.
 */

#define LED_NUM 7            /* LED BAR에서 사용하는 GPIO 개수 */

struct led_bar_pdata {
	int gpio[LED_NUM];   /* 전역 GPIO 번호, LED1 ~ LED7 순서 */
	int active_low;      /* 1이면 0을 써야 켜지는 모듈 */
};

#endif /* LED_BAR_H */
