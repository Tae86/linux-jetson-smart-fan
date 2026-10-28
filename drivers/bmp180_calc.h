/*
 * bmp180_calc.h : BMP180 보정 계산 (데이터시트 BST-BMP180-DS000-12, p.15)
 * 정수 연산만 사용. 커널/유저 공간 양쪽에서 쓸 수 있게 타입만 분기한다.
 */
#ifndef BMP180_CALC_H
#define BMP180_CALC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef int16_t  s16;
typedef uint16_t u16;
typedef int32_t  s32;
typedef uint32_t u32;
#endif

struct bmp180_calib {
	s16 ac1, ac2, ac3;
	u16 ac4, ac5, ac6;
	s16 b1, b2, mb, mc, md;
};

/* 온도 계산 중간값 b5. 압력 계산에도 필요. 실패(0으로 나눔)면 0 리턴 */
static inline s32 bmp180_calc_b5(const struct bmp180_calib *c, s32 ut, int *err)
{
	s32 x1, x2;

	x1 = (((s32)ut - (s32)c->ac6) * (s32)c->ac5) >> 15;
	if (x1 + c->md == 0) {
		*err = 1;
		return 0;
	}
	x2 = ((s32)c->mc * 2048) / (x1 + c->md);
	*err = 0;
	return x1 + x2;
}

/* 0.1℃ 단위 온도 */
static inline s32 bmp180_calc_temp_x10(s32 b5)
{
	return (b5 + 8) >> 4;
}

/* Pa 단위 압력. 실패(b4 == 0)면 *err = 1 */
static inline s32 bmp180_calc_pressure(const struct bmp180_calib *c, s32 up,
				       s32 b5, int oss, int *err)
{
	s32 b6, x1, x2, x3, b3, p;
	u32 b4, b7;

	b6 = b5 - 4000;
	x1 = ((s32)c->b2 * ((b6 * b6) >> 12)) >> 11;
	x2 = ((s32)c->ac2 * b6) >> 11;
	x3 = x1 + x2;
	b3 = ((((s32)c->ac1 * 4 + x3) * (1 << oss)) + 2) / 4;
	x1 = ((s32)c->ac3 * b6) >> 13;
	x2 = ((s32)c->b1 * ((b6 * b6) >> 12)) >> 16;
	x3 = ((x1 + x2) + 2) >> 2;
	b4 = ((u32)c->ac4 * (u32)(x3 + 32768)) >> 15;
	if (b4 == 0) {
		*err = 1;
		return 0;
	}
	b7 = ((u32)up - (u32)b3) * (u32)(50000 >> oss);
	if (b7 < 0x80000000U)
		p = (s32)((b7 * 2) / b4);
	else
		p = (s32)((b7 / b4) * 2);
	x1 = (p >> 8) * (p >> 8);
	x1 = (x1 * 3038) >> 16;
	x2 = (-7357 * p) >> 16;
	*err = 0;
	return p + ((x1 + x2 + 3791) >> 4);
}

#endif /* BMP180_CALC_H */