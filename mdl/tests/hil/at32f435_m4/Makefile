# M4 build: USB CDC transport. No embedded module -- modules arrive at
# runtime over the wire (tools/packer.py + tools/watch.py). Reuses M2's
# linker script/FreeRTOSConfig.h and M0's device conf header.

TOOLCHAIN := arm-none-eabi-
CC      := $(TOOLCHAIN)gcc
SIZE    := $(TOOLCHAIN)size

VENDOR  := ../../../../at32f435_lib/AT32F435_437_Firmware_Library_V2.2.6
MDL     := ../../..
M0      := ../at32f435_m0
M2      := ../at32f435_m2
COMMON  := ../common
FRSRC   := $(VENDOR)/middlewares/freertos/source
FRPORT  := $(MDL)/third_party/freertos_mpu_port
USBDRV  := $(VENDOR)/middlewares/usb_drivers
USBCDC  := $(VENDOR)/middlewares/usbd_class/cdc

BUILD   := build

CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

INCLUDES := \
  -I$(VENDOR)/libraries/cmsis/cm4/core_support \
  -I$(VENDOR)/libraries/cmsis/cm4/device_support \
  -I$(VENDOR)/libraries/drivers/inc \
  -I$(M0)/inc \
  -I$(M2)/inc \
  -Iinc \
  -I$(MDL)/core \
  -I$(MDL)/arch/arm_cm4 \
  -I$(MDL)/host \
  -I$(MDL)/transport \
  -I$(VENDOR)/middlewares/i2c_application_library \
  -I$(COMMON) \
  -I$(FRSRC)/include \
  -I$(FRPORT) \
  -I$(USBDRV)/inc \
  -I$(USBCDC)

DEFINES := -DAT32F435VCT7

# Bus faults are imprecise by default now (see board_debug.h). Turn the
# write buffer off for a session spent chasing one:  make PRECISE_FAULTS=1
# The console's `ver` reports which way the running image was built.
PRECISE_FAULTS ?= 0

CFLAGS := $(CPU_FLAGS) $(DEFINES) $(INCLUDES) \
  -DBOARD_DEBUG_PRECISE_BUS_FAULTS=$(PRECISE_FAULTS) \
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
  $(MDL)/arch/arm_cm4/mpu_armv7m.c \
  $(MDL)/arch/arm_cm4/fault_arm.c \
  $(MDL)/arch/arm_cm4/reloc_arm.c \
  $(MDL)/arch/arm_cm4/ctxsw_arm.c \
  $(MDL)/host/host_api.c \
  $(MDL)/host/host_events.c \
  $(MDL)/host/host_resources.c \
  $(MDL)/host/host_i2c.c \
  $(MDL)/host/host_uart.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_usart.c \
  $(VENDOR)/middlewares/i2c_application_library/i2c_application.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_i2c.c \
  $(MDL)/transport/protocol.c \
  $(MDL)/transport/console.c \
  $(MDL)/transport/usb_cdc.c \
  $(COMMON)/board_clock.c \
  $(COMMON)/board_debug.c \
  $(COMMON)/board_buildid.c \
  $(COMMON)/board_persist.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_gpio.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_crm.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_pwc.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_flash.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_acc.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_misc.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_exint.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_scfg.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_usb.c \
  $(VENDOR)/libraries/cmsis/cm4/device_support/system_at32f435_437.c \
  $(FRSRC)/tasks.c \
  $(FRSRC)/queue.c \
  $(FRSRC)/list.c \
  $(FRSRC)/timers.c \
  $(FRSRC)/event_groups.c \
  $(FRSRC)/stream_buffer.c \
  $(FRSRC)/portable/memmang/heap_4.c \
  $(FRSRC)/portable/common/mpu_wrappers.c \
  $(FRPORT)/port.c \
  $(USBDRV)/src/usb_core.c \
  $(USBDRV)/src/usbd_core.c \
  $(USBDRV)/src/usbd_int.c \
  $(USBDRV)/src/usbd_sdr.c \
  $(USBCDC)/cdc_class.c \
  $(USBCDC)/cdc_desc.c

ASM_SRCS := \
  $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc/startup_at32f435_437.s

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS))) \
        $(patsubst %.s,$(BUILD)/%.o,$(notdir $(ASM_SRCS)))

vpath %.c . $(M2) $(COMMON) $(MDL)/core $(MDL)/arch/arm_cm4 $(MDL)/host $(MDL)/transport \
  $(VENDOR)/libraries/drivers/src $(VENDOR)/libraries/cmsis/cm4/device_support \
  $(FRSRC) $(FRSRC)/portable/memmang $(FRSRC)/portable/common $(FRPORT) \
  $(USBDRV)/src $(USBCDC) $(VENDOR)/middlewares/i2c_application_library
vpath %.s $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc

.PHONY: all clean FORCE
all: $(BUILD)/firmware.elf
	$(SIZE) $(BUILD)/firmware.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Rebuilt every single time, so its __DATE__/__TIME__ is the moment THIS
# image was built rather than the last time the file changed. The device
# reports it via the console's `ver`, which is how you tell a download
# that actually took from one that silently flashed a cached ELF.
$(BUILD)/board_buildid.o: $(COMMON)/board_buildid.c FORCE | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

FORCE:

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/firmware.elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
