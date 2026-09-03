# M2 build: unprivileged module task via FreeRTOS-MPU's
# xTaskCreateRestricted(), SVC-gated host_api. Reuses M1's already-built
# module.mdl (same "hello" test module) and M0's device conf header.

TOOLCHAIN := arm-none-eabi-
CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE    := $(TOOLCHAIN)size

VENDOR  := ../../../../AT32F435_437_Firmware_Library_V2.2.6
MDL     := ../../..
M0      := ../at32f435_m0
M1MOD   := ../../modules/hello
FRSRC   := $(VENDOR)/middlewares/freertos/source
FRPORT  := $(MDL)/third_party/freertos_mpu_port

BUILD   := build

CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

INCLUDES := \
  -I$(VENDOR)/libraries/cmsis/cm4/core_support \
  -I$(VENDOR)/libraries/cmsis/cm4/device_support \
  -I$(VENDOR)/libraries/drivers/inc \
  -I$(M0)/inc \
  -Iinc \
  -I$(MDL)/core \
  -I$(MDL)/arch/arm_cm4 \
  -I$(MDL)/host \
  -I$(FRSRC)/include \
  -I$(FRPORT)

DEFINES := -DAT32F435RGT7

CFLAGS := $(CPU_FLAGS) $(DEFINES) $(INCLUDES) \
  -std=c11 -O1 -g3 -Wall -Wextra \
  -ffunction-sections -fdata-sections \
  -MMD -MP

ASFLAGS := $(CPU_FLAGS) -x assembler-with-cpp

LDSCRIPT := linker/AT32F435xG_MDL_MPU.ld
LDFLAGS := $(CPU_FLAGS) -T$(LDSCRIPT) -Wl,-L$(MDL)/linker \
  -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

C_SRCS := \
  main.c \
  freertos_hooks.c \
  $(MDL)/core/registry.c \
  $(MDL)/core/loader.c \
  $(MDL)/core/crc32.c \
  $(MDL)/core/module_task.c \
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

vpath %.c . $(MDL)/core $(MDL)/arch/arm_cm4 $(MDL)/host \
  $(VENDOR)/libraries/drivers/src $(VENDOR)/libraries/cmsis/cm4/device_support \
  $(FRSRC) $(FRSRC)/portable/memmang $(FRSRC)/portable/common $(FRPORT)
vpath %.s $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc

.PHONY: all clean
all: $(BUILD)/firmware.elf
	$(SIZE) $(BUILD)/firmware.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/module_mdl.o: $(M1MOD)/build/module.mdl | $(BUILD)
	cd $(M1MOD)/build && $(OBJCOPY) -I binary -O elf32-littlearm --binary-architecture arm \
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
