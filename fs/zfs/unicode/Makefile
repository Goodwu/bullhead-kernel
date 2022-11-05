ifneq ($(KBUILD_EXTMOD),)
src = /root/src/zfs-2.1.6/module/unicode
obj = /root/src/zfs-2.1.6/module/unicode
endif

MODULE := zunicode

obj-$(CONFIG_ZFS) := $(MODULE).o

$(MODULE)-objs += u8_textprep.o
$(MODULE)-objs += uconv.o
