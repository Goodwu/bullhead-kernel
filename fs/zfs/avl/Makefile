ifneq ($(KBUILD_EXTMOD),)
src = /root/src/zfs-2.1.6/module/avl
obj = /root/src/zfs-2.1.6/module/avl
endif

MODULE := zavl

obj-$(CONFIG_ZFS) := $(MODULE).o

$(MODULE)-objs += avl.o
