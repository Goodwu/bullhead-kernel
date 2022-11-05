ifneq ($(KBUILD_EXTMOD),)
src = /root/src/zfs-2.1.6/module/nvpair
obj = /root/src/zfs-2.1.6/module/nvpair
endif

MODULE := znvpair

obj-$(CONFIG_ZFS) := $(MODULE).o

$(MODULE)-objs += nvpair.o
$(MODULE)-objs += fnvpair.o
$(MODULE)-objs += nvpair_alloc_spl.o
$(MODULE)-objs += nvpair_alloc_fixed.o
