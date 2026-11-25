# GCC8 与厂家 BLE/ISP 二进制 ABI 保持一致；TOOLCHAIN 可覆盖为其他安装路径。
TOOLCHAIN ?= /Applications/MounRiver Studio.app/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC
PREFIX ?= riscv-none-embed-
CC := $(TOOLCHAIN)/bin/$(PREFIX)gcc
OBJCOPY := $(TOOLCHAIN)/bin/$(PREFIX)objcopy
SIZE := $(TOOLCHAIN)/bin/$(PREFIX)size
PYTHON ?= python3
ROLE ?= sensor
LOG ?= 0
SENSOR_RING_SLOTS ?= 24
SENSOR_IDLE ?= 1
SENSOR_AUTO_START ?= 0
SENSOR_STANDBY_SLEEP ?= 1
SENSOR_PA8_LED ?= 1
SENSOR_LED_ACTIVE_LOW ?= 1
DCDC_ENABLE ?= 1
# 板级变体：空 = 原板；pa = CH592F + AT2401C 前端板（仅网关角色）。
BOARD ?=
# AT2401C 的 LNA 增益从上报 rssi 中扣回，默认按数据手册 RX 增益 13 dB。
PA_RSSI_OFFSET ?= -13

ifeq ($(filter $(ROLE),sensor gateway),)
$(error ROLE must be sensor or gateway)
endif

ifeq ($(BOARD),)
BOARD_SUFFIX :=
else ifeq ($(BOARD),pa)
BOARD_SUFFIX := -pa
ifneq ($(ROLE),gateway)
$(error BOARD=pa is only defined for ROLE=gateway)
endif
else
$(error BOARD must be empty or pa)
endif

# PA 前端补偿了链路预算，芯片侧降到 +4 dBm；EIRP 仍在 20 dBm 附近，需实测确认。
BLE_TX_POWER ?= $(if $(BOARD_SUFFIX),LL_TX_PWR_4_DBM,LL_TX_PWR_0_DBM)

SDK := sdk
BUILD := build/$(ROLE)$(BOARD_SUFFIX)$(if $(filter 1,$(LOG)),-debug,)
TARGET := $(BUILD)/$(ROLE)$(BOARD_SUFFIX)
ARCH := -march=rv32imac -mabi=ilp32 -mcmodel=medany
CPPFLAGS := -Icommon -I$(ROLE)/app -I$(SDK)/SRC/StdPeriphDriver/inc \
            -I$(SDK)/SRC/RVMSIS -I$(SDK)/BLE/LIB -I$(SDK)/BLE/HAL/include \
            -DCLK_OSC32K=0 -DDCDC_ENABLE=$(DCDC_ENABLE) -DHAL_KEY=0 -DHAL_LED=0 \
            -DBLE_SNV=0 -DBLE_TX_POWER=$(BLE_TX_POWER) -DAPP_LOG_ENABLE=$(LOG)
ifeq ($(ROLE),gateway)
# 网关自行在 RF 停止的窗口校准，不把 Flash HAL 任务放进接收期间。
CPPFLAGS += -DHAL_SLEEP=0 -DBLE_CALIBRATION_ENABLE=0 -DTEM_SAMPLE=0 -DAPP_ROLE_GATEWAY=1
else
# HAL_SLEEP registers the node's Idle/Sleep hooks.
CPPFLAGS += -DHAL_SLEEP=$(SENSOR_IDLE) -DBLE_CALIBRATION_ENABLE=0 -DTEM_SAMPLE=0 \
            -DAPP_ROLE_SENSOR=1 -DSENSOR_RING_SLOTS=$(SENSOR_RING_SLOTS) \
            -DSENSOR_AUTO_START=$(SENSOR_AUTO_START) -DSENSOR_STANDBY_SLEEP=$(SENSOR_STANDBY_SLEEP) \
            -DSENSOR_PA8_LED=$(SENSOR_PA8_LED) -DSENSOR_LED_ACTIVE_LOW=$(SENSOR_LED_ACTIVE_LOW)
endif
ifeq ($(BOARD),pa)
# AT2401C：PA5 = TX_EN、PA4 = RX_EN，高有效，由协议栈 RF 使能处自动翻转。
CPPFLAGS += -DGW_BOARD_PA=1 -DGW_RSSI_OFFSET_DB=$(PA_RSSI_OFFSET) \
            -DGW_PA_TX_PIN=GPIO_Pin_5 -DGW_PA_RX_PIN=GPIO_Pin_4
endif
ifeq ($(LOG),1)
ifeq ($(ROLE),sensor)
CPPFLAGS += -DDEBUG=1
endif
endif
CPPFLAGS += $(EXTRA_CPPFLAGS)
CFLAGS := $(ARCH) -std=gnu99 -Os -g3 -ffunction-sections -fdata-sections \
          -fno-common -fsigned-char -fno-jump-tables -Wall -Wextra \
          -Wno-unused-parameter -MMD -MP
ASFLAGS := $(ARCH) -x assembler-with-cpp -g3 -MMD -MP
LDFLAGS := $(ARCH) -nostartfiles --specs=nano.specs --specs=nosys.specs \
           -L$(ROLE) -TLink.ld -Wl,--wrap=main,--gc-sections,--print-memory-usage,-Map,$(TARGET).map

DRIVERS := clk gpio sys pwr uart1 adc flash
ifeq ($(ROLE),sensor)
DRIVERS += spi0
endif
SOURCES := $(wildcard $(ROLE)/app/*.c) $(wildcard common/*.c) \
           $(SDK)/SRC/RVMSIS/core_riscv.c $(SDK)/BLE/HAL/MCU.c $(SDK)/BLE/HAL/RTC.c \
           $(addprefix $(SDK)/SRC/StdPeriphDriver/CH59x_,$(addsuffix .c,$(DRIVERS)))
ASM_SOURCES := $(wildcard common/*.S) $(SDK)/SRC/Startup/startup_CH592.S $(SDK)/BLE/LIB/ble_task_scheduler.S
OBJECTS := $(addprefix $(BUILD)/,$(SOURCES:.c=.o) $(ASM_SOURCES:.S=.o))
LIBRARIES := $(SDK)/BLE/LIB/libCH59xBLE.a $(SDK)/SRC/StdPeriphDriver/libISP592.a

.DELETE_ON_ERROR:
.PHONY: all sensor gateway gateway-pa size check-tools FORCE
all: $(TARGET).elf $(TARGET).hex $(TARGET).bin size

sensor gateway:
	$(MAKE) ROLE=$@ all

gateway-pa:
	$(MAKE) ROLE=gateway BOARD=pa all

check-tools:
	@"$(CC)" --version | head -n 1

# 改宏或工具链后自动重编；比较明文参数，无需清理构建目录。
$(BUILD)/.config: FORCE
	@mkdir -p "$(@D)"
	@printf '%s\n' '$(CC)' '$(CPPFLAGS)' '$(CFLAGS)' '$(ASFLAGS)' '$(LDFLAGS)' > "$@.new"
	@cmp -s "$@.new" "$@" || cp "$@.new" "$@"

$(OBJECTS): $(BUILD)/.config

$(TARGET).elf: $(OBJECTS) $(LIBRARIES) Link.ld $(ROLE)/rf-ram.ld ../tools/check_firmware.py
	"$(CC)" $(LDFLAGS) $(OBJECTS) -Wl,--start-group $(LIBRARIES) -lc -lm -Wl,--end-group -o "$@"
	"$(PYTHON)" "../tools/check_firmware.py" "$@" --toolchain "$(TOOLCHAIN)" --prefix "$(PREFIX)" --role "$(ROLE)"

$(TARGET).hex: $(TARGET).elf
	"$(OBJCOPY)" -O ihex "$<" "$@"

$(TARGET).bin: $(TARGET).elf
	"$(OBJCOPY)" -O binary "$<" "$@"

$(BUILD)/%.o: %.c
	@mkdir -p "$(@D)"
	"$(CC)" $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(BUILD)/%.o: %.S
	@mkdir -p "$(@D)"
	"$(CC)" $(CPPFLAGS) $(ASFLAGS) -c "$<" -o "$@"

size: $(TARGET).elf
	"$(SIZE)" "$<"

-include $(OBJECTS:.o=.d)
