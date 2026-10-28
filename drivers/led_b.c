/*
 * led_b.c : LED BAR 드라이버 (platform_driver + cdev + ioctl), 커널 5.15 기준
 *
 * 알아둘 것
 *  - led_b_dev.ko(장치)를 먼저 올려야 probe가 호출된다. 이름이 "led_bar"로 같아야 match.
 *  - LED 7개를 하위(LED1)부터 켠다. duty 값 -> 켤 개수는 duty_to_count()에서 정한다.
 *  - 값을 받는 경로 (둘 다 led_set_duty()로 모인다)
 *      (1) 모터 드라이버가 led_set_duty(duty)를 직접 호출 (모터의 실제 PWM duty: 0/40/60/80/100)
 *      (2) /dev/led_bar 의 ioctl(LED_IOC_SET_LEVEL) : 테스트/APP용 (같은 duty 값)
 *  - 모터는 symbol_get()으로 이 함수를 찾아 쓴다. led_b.ko가 안 올라가 있으면 모터는 LED 없이 동작한다.
 *    모터가 호출하는 시점에 올라가 있어야 반영되므로, 모터보다 먼저 올리는 것을 권장.
 *  - 켜질 때 애니메이션: 목표 칸 수까지 ANIM_STEP_MS 마다 한 칸씩 늘거나 줄어든다.
 *      led_set_duty()는 목표만 기록하고 바로 리턴(대기 없음), 실제 이동은 delayed_work가 한다.
 *  - 노드는 insmod 후 직접 만든다: sudo mknod /dev/led_bar c <major> 0  (major는 dmesg에 출력)
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/gpio.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include "led_b.h"
#include "led_ctl.h"

#define DEV_NAME      "led_bar"
#define ANIM_STEP_MS  80                /* 한 칸 움직이는 간격 */

static dev_t devt;
static struct cdev led_cdev;
static struct led_bar_pdata *pd;      /* probe에서 platform_data를 받아 둠 (NULL이면 probe 전) */
static DEFINE_MUTEX(led_lock);        /* target/cur 값과 GPIO 출력을 보호 */

static struct delayed_work anim_work; /* 애니메이션: 한 칸씩 이동 */
static int target_count;              /* 가고 싶은 칸 수 (led_lock 안에서만 접근) */
static int cur_count;                 /* 지금 켜져 있는 칸 수 (led_lock 안에서만 접근) */

/*
 * 모터가 쓰는 duty 값 -> 켤 LED 개수.
 * 팀 확정: 미풍(40) 1 / 약풍(60) 3 / 중풍(80) 5 / 강풍(100) 7개.
 * 값 의미나 개수가 바뀌면 여기만 고치면 됨.
 */
static int duty_to_count(unsigned int duty)
{
	switch (duty) {
	case LED_DUTY_OFF:    return 0;
	case LED_DUTY_BREEZE: return 1;
	case LED_DUTY_WEAK:   return 3;
	case LED_DUTY_MID:    return 5;
	case LED_DUTY_STRONG: return 7;
	default:              return -EINVAL;   /* 약속된 5개 값 외에는 거부 (LED는 그대로) */
	}
}

/* count개만 켜고 나머지는 끈다. active_low 모듈이면 출력값을 반대로 (led_lock 잡고 호출) */
static void led_apply(int count)
{
	int i, on;

	for (i = 0; i < LED_NUM; i++) {
		on = (i < count);
		gpio_set_value(pd->gpio[i], pd->active_low ? !on : on);
	}
}

/*
 * 예약된 시간마다 실행: 지금 칸 수를 목표 쪽으로 한 칸만 움직인다.
 * 아직 도착 전이면 다음 한 칸을 다시 예약, 도착하면 예약을 멈춘다(가만히 있을 땐 아무 작업도 안 돈다).
 * 도중에 목표가 바뀌어도 target_count만 읽으므로 자연스럽게 방향이 바뀐다.
 */
static void anim_fn(struct work_struct *work)
{
	bool done;

	mutex_lock(&led_lock);
	if (cur_count < target_count)
		cur_count++;
	else if (cur_count > target_count)
		cur_count--;
	led_apply(cur_count);
	done = (cur_count == target_count);
	mutex_unlock(&led_lock);

	if (!done)
		schedule_delayed_work(&anim_work, msecs_to_jiffies(ANIM_STEP_MS));
}

/*
 * duty 값 하나를 받아 목표로 기록하는 공통 함수.
 * 모터 드라이버(커널에서 직접 호출)와 ioctl(유저 공간)이 같이 사용한다.
 * 기다리지 않고 바로 리턴하며, mutex를 쓰므로 잠들 수 없는 곳(인터럽트 등)에서는 호출 금지.
 * (모터는 작업큐/ioctl/write 안에서 호출하므로 문제 없음)
 * 이미 애니메이션이 예약되어 있으면 그대로 두고 목표만 바뀐다.
 * 잘못된 값이면 -EINVAL, 이때 LED는 바뀌지 않는다.
 */
int led_set_duty(unsigned int duty)
{
	int count = duty_to_count(duty);

	if (count < 0)
		return count;

	mutex_lock(&led_lock);
	target_count = count;
	mutex_unlock(&led_lock);

	schedule_delayed_work(&anim_work, 0);
	return 0;
}
EXPORT_SYMBOL(led_set_duty);

/* 유저 공간(APP, 테스트 앱)에서 오는 요청 */
static long led_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int value;

	if (cmd != LED_IOC_SET_LEVEL)
		return -ENOTTY;

	/* copy_from_user(목적지, 출발지, 크기): 목적지가 먼저 */
	if (copy_from_user(&value, (int __user *)arg, sizeof(value)))
		return -EFAULT;

	return led_set_duty(value);       /* 음수는 unsigned로 커져서 -EINVAL 처리됨 */
}

static int led_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int led_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations led_fops = {
	.owner          = THIS_MODULE,
	.open           = led_open,
	.release        = led_release,
	.unlocked_ioctl = led_ioctl,
};

/* platform_device(led_b_dev.ko)와 이름이 맞으면 커널이 호출 */
static int led_probe(struct platform_device *pdev)
{
	int i, ret;

	pd = dev_get_platdata(&pdev->dev);
	if (!pd)
		return -EINVAL;

	/* GPIO 7개를 확보하고 출력으로 설정. 처음엔 모두 끈 상태(active_low면 1이 꺼짐) */
	for (i = 0; i < LED_NUM; i++) {
		ret = gpio_request(pd->gpio[i], "led_bar");
		if (ret) {
			dev_err(&pdev->dev, "gpio %d request 실패 (%d)\n",
				pd->gpio[i], ret);
			goto err_gpio;
		}
		ret = gpio_direction_output(pd->gpio[i], pd->active_low);
		if (ret) {
			gpio_free(pd->gpio[i]);
			goto err_gpio;
		}
	}

	/* 애니메이션 상태 초기화: 모두 꺼진 상태에서 시작. 호출이 들어오기 전(cdev_add 전)에 준비 */
	cur_count = 0;
	target_count = 0;
	INIT_DELAYED_WORK(&anim_work, anim_fn);

	/* 장치 번호를 커널에게 받고(동적) cdev 등록 */
	ret = alloc_chrdev_region(&devt, 0, 1, DEV_NAME);
	if (ret < 0)
		goto err_gpio;

	cdev_init(&led_cdev, &led_fops);
	led_cdev.owner = THIS_MODULE;
	ret = cdev_add(&led_cdev, devt, 1);
	if (ret) {
		unregister_chrdev_region(devt, 1);
		goto err_gpio;
	}

	dev_info(&pdev->dev, "probe ok, major=%d (mknod /dev/%s c %d 0)\n",
		 MAJOR(devt), DEV_NAME, MAJOR(devt));
	return 0;

err_gpio:                       /* 실패 시 이미 확보한 GPIO만 되돌린다 */
	while (--i >= 0)
		gpio_free(pd->gpio[i]);
	return ret;
}

static int led_remove(struct platform_device *pdev)   /* 5.15: remove는 int 리턴 */
{
	int i;

	cdev_del(&led_cdev);                /* 새 ioctl이 더 안 들어오게 먼저 막는다 */
	cancel_delayed_work_sync(&anim_work);   /* 애니메이션 중지: 모듈이 사라진 뒤 함수가 실행되지 않게 */
	mutex_lock(&led_lock);
	led_apply(0);                       /* 내릴 때 LED를 모두 즉시 끈다 */
	mutex_unlock(&led_lock);
	unregister_chrdev_region(devt, 1);
	for (i = 0; i < LED_NUM; i++)
		gpio_free(pd->gpio[i]);

	return 0;
}

static struct platform_driver led_driver = {
	.probe  = led_probe,
	.remove = led_remove,
	.driver = {
		.name = "led_bar",   /* platform_device 이름과 같아야 match -> probe */
	},
};

module_platform_driver(led_driver);
MODULE_LICENSE("GPL");
