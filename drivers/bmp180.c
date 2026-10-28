/*
 * bmp180.c : BMP180 온도/기압 센서 I2C 드라이버 (i2c_driver + cdev + ioctl), 커널 5.15 기준
 *
 * 구조 (수업 8부 Bus - Device - Driver, 디바이스 트리 없이 코드로 등록)
 *   1) Device : module_init에서 "I2C 버스 N번, 주소 0x77, 이름 bmp180_fan" 장치를 등록
 *   2) Driver : i2c_driver를 등록. 장치 이름과 id_table 이름이 같으면 match
 *   3) match가 되면 커널이 bmp180_probe()를 호출 -> 칩 확인, 보정계수 읽기, /dev 장치 등록, 주기 측정 시작
 *
 * 알아둘 것
 *  - 로드: sudo insmod bmp180.ko bus=7   (센서가 보이는 I2C 버스 번호, i2cdetect -y -r 7 에서 77 확인)
 *  - 센서가 없거나 배선이 틀려 probe가 실패하면 insmod도 실패한다 (dmesg에 이유 출력)
 *  - 노드는 직접 만든다: sudo mknod /dev/bmp180 c <major> 0  (major는 dmesg나 /proc/devices에서 확인)
 *  - 값은 정수만 사용: temp_x10(0.1℃ 단위), pressure_pa(Pa). 커널에서는 float 금지.
 *  - 값을 주는 경로
 *      (1) /dev/bmp180 ioctl(BMP180_IOC_READ): 호출할 때마다 새로 측정 (APP, Auto 모드 온도용)
 *      (2) bmp180_get_data(): 1초마다 측정해 둔 최신 값을 복사해 줌 (LCD 드라이버용, 대기 없음)
 *          -> LCD 모듈보다 bmp180.ko를 먼저 insmod 해야 함 (심볼 의존)
 *  - 나중에 디바이스 트리를 쓰게 되면: module_init의 장치 등록 부분을 빼고,
 *    i2c_driver에 .of_match_table (compatible 문자열)만 추가하면 probe는 그대로 쓸 수 있다.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include "fan_common.h"
#include "bmp180_calc.h"

#define DEV_NAME        "bmp180"
#define BMP180_NAME     "bmp180_fan"   /* 장치 이름 = 드라이버 id_table 이름 (match 기준) */
#define BMP180_ADDR     0x77           /* 7비트 주소 (데이터시트의 0xEE/0xEF는 읽기/쓰기 포함 8비트 표기) */

#define REG_CALIB       0xAA        /* 보정계수 0xAA ~ 0xBF (22바이트) */
#define REG_CHIP_ID     0xD0
#define REG_CTRL_MEAS   0xF4
#define REG_OUT_MSB     0xF6

#define CHIP_ID_VALUE   0x55        /* 칩 ID 레지스터 값: 통신 확인용 */
#define CMD_TEMP        0x2E
#define OSS             1           /* 0~3 : 클수록 정밀하지만 느림. 선풍기엔 1이면 충분 */
#define CMD_PRESSURE    (0x34 + (OSS << 6))

#define POLL_MS         1000        /* LCD용 최신 값 갱신 주기 */

static int bus = 7;
module_param(bus, int, 0444);
MODULE_PARM_DESC(bus, "I2C bus number where BMP180 is connected");

static struct i2c_client *created_client;  /* module_init에서 만든 장치 (제거용) */
static struct i2c_client *client;          /* probe에서 받은 장치 (측정 함수들이 사용) */
static int probe_result = -ENODEV;         /* probe 결과: init이 insmod 성공/실패 판단에 사용 */

static struct bmp180_calib calib;     /* 보정계수: probe에서 한 번만 읽음 */
static dev_t devt;
static struct cdev bmp_cdev;
static DEFINE_MUTEX(bmp_lock);        /* I2C 측정 순서가 섞이지 않게 (ioctl과 주기 측정 동시 접근 보호) */

static struct bmp180_data latest;     /* 마지막 측정 결과 (LCD가 가져가는 값) */
static bool latest_valid;             /* 첫 측정이 끝났는지 */
static DEFINE_SPINLOCK(latest_lock);  /* 온도/압력이 같은 측정의 짝으로 복사되게 보호 */
static struct delayed_work poll_work;

/* 보정계수 22바이트를 읽어 11개 값으로 저장 */
static int bmp180_read_calib(void)
{
	u8 buf[22];
	u16 w[11];
	int i, ret;

	ret = i2c_smbus_read_i2c_block_data(client, REG_CALIB, sizeof(buf), buf);
	if (ret != sizeof(buf))
		return ret < 0 ? ret : -EIO;

	for (i = 0; i < 11; i++) {
		w[i] = (buf[2 * i] << 8) | buf[2 * i + 1];
		if (w[i] == 0 || w[i] == 0xFFFF) {   /* 데이터시트: 0 또는 0xFFFF면 통신 오류 */
			pr_err("bmp180: calib word %d invalid (0x%04x)\n", i, w[i]);
			return -EIO;
		}
	}

	calib.ac1 = w[0];  calib.ac2 = w[1];  calib.ac3 = w[2];
	calib.ac4 = w[3];  calib.ac5 = w[4];  calib.ac6 = w[5];
	calib.b1  = w[6];  calib.b2  = w[7];  calib.mb  = w[8];
	calib.mc  = w[9];  calib.md  = w[10];
	return 0;
}

/*
 * 한 번 측정. 반드시 온도 -> 압력 순서 (압력 계산에 온도 중간값 b5가 필요).
 * 호출하는 쪽에서 bmp_lock을 잡아야 한다.
 */
static int bmp180_measure(struct bmp180_data *out)
{
	u8 buf[3];
	s32 ut, up, b5;
	int ret, err;

	ret = i2c_smbus_write_byte_data(client, REG_CTRL_MEAS, CMD_TEMP);
	if (ret < 0)
		return ret;
	usleep_range(5000, 6000);                       /* 온도 변환 최대 4.5ms */
	ret = i2c_smbus_read_i2c_block_data(client, REG_OUT_MSB, 2, buf);
	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	ut = (buf[0] << 8) | buf[1];

	ret = i2c_smbus_write_byte_data(client, REG_CTRL_MEAS, CMD_PRESSURE);
	if (ret < 0)
		return ret;
	usleep_range(8000, 9000);                       /* 압력 변환 oss=1 최대 7.5ms */
	ret = i2c_smbus_read_i2c_block_data(client, REG_OUT_MSB, 3, buf);
	if (ret != 3)
		return ret < 0 ? ret : -EIO;
	up = ((buf[0] << 16) | (buf[1] << 8) | buf[2]) >> (8 - OSS);

	b5 = bmp180_calc_b5(&calib, ut, &err);          /* 보정 계산: bmp180_calc.h */
	if (err)
		return -EIO;
	out->temp_x10 = bmp180_calc_temp_x10(b5);
	out->pressure_pa = bmp180_calc_pressure(&calib, up, b5, OSS, &err);
	if (err)
		return -EIO;

	return 0;
}

/* 1초마다 측정해서 latest에 저장하고, 다음 주기를 다시 예약 (모드와 상관없이 계속 동작) */
static void bmp_poll_fn(struct work_struct *work)
{
	struct bmp180_data d;
	unsigned long flags;
	int ret;

	mutex_lock(&bmp_lock);
	ret = bmp180_measure(&d);
	mutex_unlock(&bmp_lock);

	if (!ret) {
		spin_lock_irqsave(&latest_lock, flags);
		latest = d;
		latest_valid = true;
		spin_unlock_irqrestore(&latest_lock, flags);
	} else {
		pr_warn_ratelimited("bmp180: poll measure failed (%d)\n", ret);
	}

	printk("p_pa : %d, temp : %d", d.pressure_pa, d.temp_x10);
	schedule_delayed_work(&poll_work, msecs_to_jiffies(POLL_MS));
}

/*
 * 다른 드라이버(LCD)용: 저장된 최신 값을 복사해 준다. 측정 대기 없이 바로 리턴.
 * 첫 측정 전(insmod 직후 약 15ms)이면 -EAGAIN.
 */
int bmp180_get_data(struct bmp180_data *out)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&latest_lock, flags);
	if (latest_valid)
		*out = latest;
	else
		ret = -EAGAIN;
	spin_unlock_irqrestore(&latest_lock, flags);

	return ret;
}
EXPORT_SYMBOL(bmp180_get_data);   /* 다른 모듈이 부를 수 있게 공개 */

/* 유저 공간(APP, 테스트 앱)에서 오는 요청: 호출할 때마다 새로 측정해서 돌려줌 */
static long bmp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct bmp180_data data;
	int ret;

	if (cmd != BMP180_IOC_READ)
		return -ENOTTY;

	mutex_lock(&bmp_lock);
	ret = bmp180_measure(&data);
	mutex_unlock(&bmp_lock);
	if (ret)
		return ret;

	/* copy_to_user(목적지, 출발지, 크기): 목적지가 먼저 */
	if (copy_to_user((void __user *)arg, &data, sizeof(data)))
		return -EFAULT;

	return 0;
}

static int bmp_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int bmp_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations bmp_fops = {
	.owner          = THIS_MODULE,
	.open           = bmp_open,
	.release        = bmp_release,
	.unlocked_ioctl = bmp_ioctl,
};

/* 장치 이름이 id_table과 맞으면 커널이 호출 (match -> probe) */
static int bmp180_probe(struct i2c_client *cl, const struct i2c_device_id *id)
{
	int ret;

	client = cl;
	latest_valid = false;

	/* 칩 ID가 0x55가 아니면 센서가 없거나 배선/버스 번호가 틀린 것 */
	ret = i2c_smbus_read_byte_data(client, REG_CHIP_ID);
	if (ret != CHIP_ID_VALUE) {
		pr_err("bmp180: chip id 0x%02x (expect 0x%02x), 배선/버스 번호 확인\n",
		       ret < 0 ? 0 : ret, CHIP_ID_VALUE);
		ret = ret < 0 ? ret : -ENODEV;
		goto err;
	}

	ret = bmp180_read_calib();
	if (ret)
		goto err;

	ret = alloc_chrdev_region(&devt, 0, 1, DEV_NAME);
	if (ret < 0)
		goto err;

	cdev_init(&bmp_cdev, &bmp_fops);
	bmp_cdev.owner = THIS_MODULE;
	ret = cdev_add(&bmp_cdev, devt, 1);
	if (ret) {
		unregister_chrdev_region(devt, 1);
		goto err;
	}

	/* 주기 측정 시작: 모든 준비가 끝난 맨 마지막에 */
	INIT_DELAYED_WORK(&poll_work, bmp_poll_fn);
	schedule_delayed_work(&poll_work, 0);

	pr_info("bmp180: probe ok, major=%d (mknod /dev/%s c %d 0)\n",
		MAJOR(devt), DEV_NAME, MAJOR(devt));
	probe_result = 0;
	return 0;

err:
	probe_result = ret;
	return ret;
}

static int bmp180_remove(struct i2c_client *cl)   /* 5.15: i2c remove는 int 리턴 */
{
	cancel_delayed_work_sync(&poll_work);   /* 주기 측정을 먼저 멈춘다 */
	cdev_del(&bmp_cdev);
	unregister_chrdev_region(devt, 1);
	return 0;
}

static const struct i2c_device_id bmp180_id[] = {
	{ BMP180_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bmp180_id);

static struct i2c_driver bmp180_driver = {
	.driver = {
		.name = BMP180_NAME,
	},
	.probe    = bmp180_probe,
	.remove   = bmp180_remove,
	.id_table = bmp180_id,
};

static int __init bmp180_init(void)
{
	struct i2c_adapter *adap;
	struct i2c_board_info info = {
		I2C_BOARD_INFO(BMP180_NAME, BMP180_ADDR),
	};
	int ret;

	/* 1) Device 등록: device tree 대신 "버스 N번에 주소 0x77 장치가 있다"고 코드로 알림 */
	adap = i2c_get_adapter(bus);
	if (!adap) {
		pr_err("bmp180: i2c bus %d not found\n", bus);
		return -ENODEV;
	}
	created_client = i2c_new_client_device(adap, &info);
	i2c_put_adapter(adap);
	if (IS_ERR(created_client)) {
		ret = PTR_ERR(created_client);
		pr_err("bmp180: client create failed (%d), 이미 다른 드라이버가 0x%02x 사용 중일 수 있음\n",
		       ret, BMP180_ADDR);
		return ret;
	}

	/* 2) Driver 등록: 이름이 맞는 장치가 이미 있으므로 여기서 바로 probe가 호출된다 */
	ret = i2c_add_driver(&bmp180_driver);
	if (ret) {
		i2c_unregister_device(created_client);
		return ret;
	}

	/* probe가 실패해도 i2c_add_driver는 성공을 돌려주므로, 결과를 직접 확인해 insmod를 실패시킴 */
	if (probe_result) {
		ret = probe_result;
		i2c_del_driver(&bmp180_driver);
		i2c_unregister_device(created_client);
		return ret;
	}

	return 0;
}

static void __exit bmp180_exit(void)
{
	i2c_del_driver(&bmp180_driver);          /* remove 호출: 주기 측정 중지, 장치 해제 */
	i2c_unregister_device(created_client);
}

module_init(bmp180_init);
module_exit(bmp180_exit);
MODULE_LICENSE("GPL");