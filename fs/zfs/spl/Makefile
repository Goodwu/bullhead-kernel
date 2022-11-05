ifneq ($(KBUILD_EXTMOD),)
src = /root/src/zfs-2.1.6/module/spl
obj = /root/src/zfs-2.1.6/module/spl
mfdir = $(obj)
else
mfdir = $(srctree)/$(src)
endif

MODULE := spl

obj-$(CONFIG_ZFS) := $(MODULE).o

include $(mfdir)/../os/linux/spl/Makefile
