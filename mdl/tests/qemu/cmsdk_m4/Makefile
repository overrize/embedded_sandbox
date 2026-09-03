# QEMU `-M mps2-an386` acceptance build: loads a deliberately-crashing
# module, confirms M3's fault-recovery path executes for real (not just
# compiles), and exits with a pass/fail code via semihosting.
#
# Unlike the AT32 HIL builds, this target needs NO vendor BSP -- only
# generic CMSIS-Core (core_cm4.h, reached via the vendor BSP's copy on
# disk in this sandbox purely for convenience; any CMSIS-Core
# distribution's copy works identically, see inc/cmsis_device.h) and the
# vendored FreeRTOS-MPU port, which is itself vendor-independent.

MODULE  ?= fault_null_deref

TOOLCHAIN := arm-none-eabi-
CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE    := $(TOOLCHAIN)size
PYTHON  := python

VENDOR  := ../../../../AT32F435_437_Firmware_Library_V2.2.6
MDL     := ../../..
M2      := ../../hil/at32f435_m2
MODDIR  := ../../modules/$(MODULE)
FRSRC   := $(VENDOR)/middlewares/freertos/source
FRPORT  := $(MDL)/third_party/freertos_mpu_port

BUILD   := build

CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

INCLUDES := \
  -I$(VENDOR)/libraries/cmsis/cm4/core_support \
  -Iinc \
  -I$(MDL)/core \
  -I$(MDL)/arch/arm_cm4 \
  -I$(MDL)/host \
  -I$(MDL)/transport \
  -I$(FRSRC)/include \
  -I$(FRPORT)

CFLAGS := $(CPU_FLAGS) $(INCLUDES) \
  -std=c11 -O1 -g3 -Wall -Wextra \
  -ffunction-sections -fdata-sections \
  -MMD -MP

ASFLAGS := $(CPU_FLAGS) -x assembler-with-cpp

LDSCRIPT := linker/cmsdk_m4.ld
LDFLAGS := $(CPU_FLAGS) -T$(LDSCRIPT) -Wl,-L$(MDL)/linker \
  -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

C_SRCS := \
  main.c \
  system_cmsdk_m4.c \
  host_api_qemu.c \
  $(M2)/freertos_hooks.c \
  $(MDL)/core/registry.c \
  $(MDL)/core/loader.c \
  $(MDL)/core/crc32.c \
  $(MDL)/core/module_task.c \
  $(MDL)/core/supervisor.c \
  $(MDL)/transport/protocol.c \
  $(MDL)/arch/arm_cm4/mpu_armv7m.c \
  $(MDL)/arch/arm_cm4/fault_arm.c \
  $(MDL)/arch/arm_cm4/reloc_arm.c \
  $(MDL)/arch/arm_cm4/ctxsw_arm.c \
  $(FRSRC)/tasks.c \
  $(FRSRC)/queue.c \
  $(FRSRC)/list.c \
  $(FRSRC)/timers.c \
  $(FRSRC)/event_groups.c \
  $(FRSRC)/stream_buffer.c \
  $(FRSRC)/portable/memmang/heap_4.c \
  $(FRSRC)/portable/common/mpu_wrappers.c \
  $(FRPORT)/port.c

ASM_SRCS := startup_cmsdk_m4.s

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS))) \
        $(patsubst %.s,$(BUILD)/%.o,$(notdir $(ASM_SRCS))) \
        $(BUILD)/module_mdl.o

vpath %.c . $(M2) $(MDL)/core $(MDL)/arch/arm_cm4 $(FRSRC) \
  $(FRSRC)/portable/memmang $(FRSRC)/portable/common $(FRPORT)

.PHONY: all clean
all: $(BUILD)/firmware.elf
	$(SIZE) $(BUILD)/firmware.elf

$(BUILD):
	mkdir -p $(BUILD)

MODULE_CFLAGS := $(CPU_FLAGS) \
  -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative \
  -ffunction-sections -fdata-sections -nostdlib -nostartfiles \
  -O1 -g -Wall -Wextra -I$(MDL)/host

MODULE_LDFLAGS := $(CPU_FLAGS) -nostdlib -shared -Wl,-Bsymbolic \
  -Wl,--no-undefined -Wl,--gc-sections -static-libgcc

$(MODDIR)/build/module.o: $(MODDIR)/module.c
	mkdir -p $(MODDIR)/build
	$(CC) $(MODULE_CFLAGS) -c $< -o $@

$(MODDIR)/build/module.so: $(MODDIR)/build/module.o
	$(CC) $(MODULE_LDFLAGS) $< -o $@

$(MODDIR)/build/module.mdl: $(MODDIR)/build/module.so
	$(PYTHON) $(MDL)/tools/packer.py $< $@

$(BUILD)/module_mdl.o: $(MODDIR)/build/module.mdl | $(BUILD)
	cd $(MODDIR)/build && $(OBJCOPY) -I binary -O elf32-littlearm --binary-architecture arm \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  module.mdl $(abspath $@)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/firmware.elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD)

run: $(BUILD)/firmware.elf
	qemu-system-arm -M mps2-an386 -nographic \
	  -semihosting-config enable=on,target=native \
	  -kernel $(BUILD)/firmware.elf

-include $(OBJS:.o=.d)
