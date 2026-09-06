# M1 build: packer + loader + GOT relocation, module runs privileged.
# Reuses the M0 project's linker script and at32f435_437_conf.h rather
# than duplicating them (same arena, same device config).

TOOLCHAIN := arm-none-eabi-
CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE    := $(TOOLCHAIN)size
PYTHON  := python

VENDOR  := ../../../../at32f435_lib/AT32F435_437_Firmware_Library_V2.2.6
MDL     := ../../..
M0      := ../at32f435_m0
COMMON  := ../common
MODULE  := ../../modules/hello

BUILD   := build

CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

INCLUDES := \
  -I$(VENDOR)/libraries/cmsis/cm4/core_support \
  -I$(VENDOR)/libraries/cmsis/cm4/device_support \
  -I$(VENDOR)/libraries/drivers/inc \
  -I$(M0)/inc \
  -I$(COMMON) \
  -I$(MDL)/core \
  -I$(MDL)/arch/arm_cm4 \
  -I$(MDL)/host

DEFINES := -DAT32F435VCT7

CFLAGS := $(CPU_FLAGS) $(DEFINES) $(INCLUDES) \
  -std=c11 -O1 -g3 -Wall -Wextra \
  -ffunction-sections -fdata-sections \
  -MMD -MP

ASFLAGS := $(CPU_FLAGS) -x assembler-with-cpp

LDSCRIPT := $(M0)/linker/AT32F435xC_MDL.ld
LDFLAGS := $(CPU_FLAGS) -T$(LDSCRIPT) -Wl,-L$(MDL)/linker \
  -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

C_SRCS := \
  main.c \
  $(MDL)/core/registry.c \
  $(MDL)/core/sandbox.c \
  $(MDL)/core/loader.c \
  $(MDL)/core/crc32.c \
  $(MDL)/arch/arm_cm4/mpu_armv7m.c \
  $(MDL)/arch/arm_cm4/fault_arm.c \
  $(MDL)/arch/arm_cm4/reloc_arm.c \
  $(MDL)/arch/arm_cm4/ctxsw_arm.c \
  host_api_m1.c \
  $(COMMON)/board_clock.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_gpio.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_crm.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_pwc.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_flash.c \
  $(VENDOR)/libraries/drivers/src/at32f435_437_misc.c \
  $(VENDOR)/libraries/cmsis/cm4/device_support/system_at32f435_437.c
# host_api_m1.c, NOT $(MDL)/host/host_api.c: that one is the M2+ SVC-gated
# FreeRTOS implementation and cannot link into a bare-metal build. See
# host_api_m1.c's file comment.
# pwc/flash: board_clock.c needs pwc_ldo_output_voltage_set() and
# flash_clock_divider_set() to reach 288MHz safely.
# misc: SysTick_Config() / NVIC helpers used by host_api_m1.c.

ASM_SRCS := \
  $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc/startup_at32f435_437.s

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS))) \
        $(patsubst %.s,$(BUILD)/%.o,$(notdir $(ASM_SRCS))) \
        $(BUILD)/module_mdl.o

vpath %.c . $(MDL)/core $(MDL)/arch/arm_cm4 $(MDL)/host $(COMMON) \
  $(VENDOR)/libraries/drivers/src $(VENDOR)/libraries/cmsis/cm4/device_support
vpath %.s $(VENDOR)/libraries/cmsis/cm4/device_support/startup/gcc

.PHONY: all clean module
all: $(BUILD)/firmware.elf
	$(SIZE) $(BUILD)/firmware.elf

$(BUILD):
	mkdir -p $(BUILD)

# ---- module: compile, link (per host_api.h/spec's exact flags), pack ----
MODULE_CFLAGS := $(CPU_FLAGS) \
  -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative \
  -ffunction-sections -fdata-sections -nostdlib -nostartfiles \
  -O1 -g -Wall -Wextra -I$(MDL)/host

MODULE_LDFLAGS := $(CPU_FLAGS) -nostdlib -shared -Wl,-Bsymbolic \
  -Wl,--no-undefined -Wl,--gc-sections -static-libgcc

module: $(MODULE)/build/module.mdl

$(MODULE)/build/module.o: $(MODULE)/module.c | $(BUILD)
	mkdir -p $(MODULE)/build
	$(CC) $(MODULE_CFLAGS) -c $< -o $@

$(MODULE)/build/module.so: $(MODULE)/build/module.o
	$(CC) $(MODULE_LDFLAGS) $< -o $@

$(MODULE)/build/module.mdl: $(MODULE)/build/module.so
	$(PYTHON) $(MDL)/tools/packer.py $< $@

$(BUILD)/module_mdl.o: $(MODULE)/build/module.mdl | $(BUILD)
	# objcopy bakes the INPUT PATH into the generated symbol names
	# (_binary_<sanitized path>_start/_end/_size) -- cd into the
	# module's build dir first so main.c's extern declarations
	# (_binary_module_mdl_start/_end) stay short and predictable
	# instead of encoding this whole repo's absolute path.
	# --rename-section: objcopy's default output section for a raw
	# binary blob is .data (writable, RAM-resident after the vendor
	# startup's flash->RAM copy loop) -- rename to .rodata so this
	# read-only blob costs flash only, not RAM, matching what it
	# actually is.
	cd $(MODULE)/build && $(OBJCOPY) -I binary -O elf32-littlearm --binary-architecture arm \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  module.mdl $(abspath $@)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/firmware.elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD) $(MODULE)/build

-include $(OBJS:.o=.d)
