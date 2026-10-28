/*
 * led_b_dev.c : LED BAR "장치(Device)" 등록 모듈 (8부 2과, device tree 대신)
 *
 * 알아둘 것
 *  - 이 모듈은 "LED BAR 장치가 있고 핀은 이것이다"라고 커널에 알리는 역할만 한다.
 *    실제 제어는 led_b.ko(드라이버)가 한다.
 *  - 로드 순서: led_b_dev.ko 먼저, led_b.ko 나중. 제거는 반대로.
 *  - 핀 번호는 insmod 인자로 받는다(코드에 박지 않음):
 *      sudo insmod led_b_dev.ko gpios=458,459,401,400,483,482,481 active_low=0
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "led_b.h"

/* gpios= 로 받은 7개 번호 (module_param_array: 개수는 gpios_cnt에 저장) */
static int gpios[LED_NUM];
static int gpios_cnt;
module_param_array(gpios, int, &gpios_cnt, 0444);
MODULE_PARM_DESC(gpios, "global GPIO numbers of LED1..LED7");

static int active_low;
module_param(active_low, int, 0444);

static struct led_bar_pdata pdata;

/* release 콜백이 없으면 unregister 때 커널이 경고를 출력한다 (static 객체라 할 일 없음) */
static void led_bar_dev_release(struct device *dev)
{
}

static struct platform_device led_bar_pdev = {
	.name = "led_bar",              /* 드라이버의 .driver.name과 같아야 match -> probe */
	.id   = -1,
	.dev  = {
		.platform_data = &pdata,    /* 드라이버가 dev_get_platdata()로 꺼내 씀 */
		.release       = led_bar_dev_release,
	},
};

static int __init led_bar_dev_init(void)
{
	int i;

	/* 7개가 아니면 등록하지 않는다 (배열 범위 초과 방지) */
	if (gpios_cnt != LED_NUM) {
		pr_err("led_bar_dev: gpios= 에 %d개를 넘겨야 함 (현재 %d개)\n",
		       LED_NUM, gpios_cnt);
		return -EINVAL;
	}
	for (i = 0; i < LED_NUM; i++)
		pdata.gpio[i] = gpios[i];
	pdata.active_low = active_low;

	return platform_device_register(&led_bar_pdev);
}

static void __exit led_bar_dev_exit(void)
{
	platform_device_unregister(&led_bar_pdev);
}

module_init(led_bar_dev_init);
module_exit(led_bar_dev_exit);
MODULE_LICENSE("GPL");
