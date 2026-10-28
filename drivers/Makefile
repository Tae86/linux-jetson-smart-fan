# 담당 드라이버(LED BAR, BMP180) 빌드용 Makefile
# 전체 선풍기 통합 빌드는 팀 저장소(twotwoship/Linux_fan)를 참고하세요.
obj-m += bmp180.o
obj-m += led_b_dev.o
obj-m += led_b.o

KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
