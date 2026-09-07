# M0 build: mdl core+arch layer linked against the real AT32F435xG BSP
# (Artery AT32F435_437_Firmware_Library_V2.2.6, extracted next to this repo).
# Produces firmware.elf -- not flashed or run anywhere by this Makefile,
# see mdl/tests/hil/at32f435_m0/README for how it's meant to be used.

TOOLCHAIN := arm-none-eabi-
CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE    := $(TOOLCHAIN)size

VENDOR  := ../../../../at32f435_lib/AT32F435_437_Firmware_Library_V2.2.6
MDL     := ../../..

BUILD   := build

CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

INCLUDES := \
  -I$(VENDOR)/libraries/cmsis/cm4/core_support \
  -I$(VENDOR)/libraries/cmsis/cm4/device_support \
  -I$(VENDOR)/libraries/drivers/inc \
  -Iinc \
  -I$(MDL)/core \
  -I$(MDL)/host \
  -I$(MDL)/arch/arm_cm4

DEFINES := -DAT32F435VCT7

CFLAGS := $(CPU_FLAGS) $(DEFINES) $(INCLUDES) \
  -std=c11 -O1 -g3 -Wall -Wextra \
  -ffunction-sections -fdata-sections \
  -MMD -MP

ASFLAGS := $(CPU_FLAGS) -x assembler-with-cpp

LDSCRIPT := linker/AT32F435xC_MDL.ld
LDFLAGS := $(CPU_FLAGS) -T$(LDSCRIPT) -Wl,-L$(MDL)/linker \
  -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map
# Deliberately NOT -nostartfiles: ENTRY(Reset_Handler) in the linker
# script means crt0.o's _start is unreferenced and gets garbage-collected,
# but crti.o/crtn.o are still needed to provide _init/_fini for
# __libc_init_array() (called from the vendor's Reset_Handler in
# startup_at32f435_437.s). -nostartfiles excludes those too and produces
# an undefined reference to _init.

C_SRCS := \
  main.c \
  $(MDL)/core/registry.c \
  $(MDL)/core/sandbox.c \
  $(MDL)/arch/arm_cm4/mpu_armv7m.c \
  $(MDL)/arch/arm_cm4/fault_arm.c \
  $(MDL)/arch/arm_cm4/reloc_arm.c \
  $(MDL)/arch/arm_cm4/ctxsw_arm.c \
  $(VENDOR)/libraries/cmsis/cm4/device_support/system_at32f435_437.c

ASM_SRCS := \
  $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc/startup_at32f435_437.s

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS))) \
        $(patsubst %.s,$(BUILD)/%.o,$(notdir $(ASM_SRCS)))

vpath %.c . $(MDL)/core $(MDL)/arch/arm_cm4 $(VENDOR)/libraries/cmsis/cm4/device_support
vpath %.s $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc

.PHONY: all clean
all: $(BUILD)/firmware.elf
	$(SIZE) $(BUILD)/firmware.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/firmware.elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
