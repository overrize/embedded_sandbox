# M3 build: fault recovery + software watchdog + resource reclaim.
# Reuses M2's linker script/FreeRTOSConfig.h verbatim (no MPU-layout
# changes needed for M3) and M0's device conf header.
#
# MODULE selects which test module gets packed and embedded -- override
# on the command line to try a different negative test, e.g.:
#   make MODULE=fault_infinite_loop
# Defaults to fault_null_deref (the simplest, most reliable of the 6).

MODULE  ?= fault_null_deref

TOOLCHAIN := arm-none-eabi-
CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE    := $(TOOLCHAIN)size
PYTHON  := python

VENDOR  := ../../../../at32f435_lib/AT32F435_437_Firmware_Library_V2.2.6
MDL     := ../../..
M0      := ../at32f435_m0
M2      := ../at32f435_m2
MODDIR  := ../../modules/$(MODULE)
FRSRC   := $(VENDOR)/middlewares/freertos/source
FRPORT  := $(MDL)/third_party/freertos_mpu_port

BUILD   := build

CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

INCLUDES := \
  -I$(VENDOR)/libraries/cmsis/cm4/core_support \
  -I$(VENDOR)/libraries/cmsis/cm4/device_support \
  -I$(VENDOR)/libraries/drivers/inc \
  -I$(M0)/inc \
  -I$(M2)/inc \
  -I$(MDL)/core \
  -I$(MDL)/arch/arm_cm4 \
  -I$(MDL)/host \
  -I$(MDL)/transport \
  -I$(FRSRC)/include \
  -I$(FRPORT)

DEFINES := -DAT32F435VCT7

CFLAGS := $(CPU_FLAGS) $(DEFINES) $(INCLUDES) \
  -std=c11 -O1 -g3 -Wall -Wextra \
  -ffunction-sections -fdata-sections \
  -MMD -MP

ASFLAGS := $(CPU_FLAGS) -x assembler-with-cpp

LDSCRIPT := $(M2)/linker/AT32F435xC_MDL_MPU.ld
LDFLAGS := $(CPU_FLAGS) -T$(LDSCRIPT) -Wl,-L$(MDL)/linker \
  -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

C_SRCS := \
  main.c \
  $(M2)/freertos_hooks.c \
  $(MDL)/core/registry.c \
  $(MDL)/core/sandbox.c \
  $(MDL)/core/loader.c \
  $(MDL)/core/crc32.c \
  $(MDL)/core/module_task.c \
  $(MDL)/core/supervisor.c \
  $(MDL)/transport/protocol.c \
  $(MDL)/transport/console.c \
  $(MDL)/arch/arm_cm4/mpu_armv7m.c \
  $(MDL)/arch/arm_cm4/fault_arm.c \
  $(MDL)/arch/arm_cm4/reloc_arm.c \
  $(MDL)/arch/arm_cm4/ctxsw_arm.c \
  $(MDL)/host/host_api.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_gpio.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_crm.c \
  $(VENDOR)/libraries/cmsis/cm4/device_support/system_at32f435_437.c \
  $(FRSRC)/tasks.c \
  $(FRSRC)/queue.c \
  $(FRSRC)/list.c \
  $(FRSRC)/timers.c \
  $(FRSRC)/event_groups.c \
  $(FRSRC)/stream_buffer.c \
  $(FRSRC)/portable/memmang/heap_4.c \
  $(FRSRC)/portable/common/mpu_wrappers.c \
  $(FRPORT)/port.c

ASM_SRCS := \
  $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc/startup_at32f435_437.s

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS))) \
        $(patsubst %.s,$(BUILD)/%.o,$(notdir $(ASM_SRCS))) \
        $(BUILD)/module_mdl.o

vpath %.c . $(M2) $(MDL)/core $(MDL)/arch/arm_cm4 $(MDL)/host $(MDL)/transport \
  $(VENDOR)/libraries/drivers/src $(VENDOR)/libraries/cmsis/cm4/device_support \
  $(FRSRC) $(FRSRC)/portable/memmang $(FRSRC)/portable/common $(FRPORT)
vpath %.s $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc

.PHONY: all clean module
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

module: $(MODDIR)/build/module.mdl

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

-include $(OBJS:.o=.d)
