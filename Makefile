HOST_CC ?= cc
HOST_CFLAGS := -std=c99 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all firmware sensor gateway web test test-common test-sensor test-gateway test-docs test-web
all: firmware
firmware: sensor gateway
sensor gateway:
	$(MAKE) -C "firmware" ROLE=$@ all

web:
	node "web/build.cjs"

test: test-common test-sensor test-gateway test-docs test-web

test-common:
	@mkdir -p "build/tests"
	$(HOST_CC) $(HOST_CFLAGS) -I"firmware/common" "tests/common/test_protocol.c" \
	    "firmware/common/proto.c" "firmware/common/crc16.c" "firmware/common/framing.c" \
	    -o "build/tests/test_protocol"
	"build/tests/test_protocol"
	$(HOST_CC) $(HOST_CFLAGS) -I"firmware/common" "tests/common/test_stack.c" \
	    "firmware/common/stack_monitor.c" -o "build/tests/test_stack"
	"build/tests/test_stack"

test-sensor:
	@mkdir -p "build/tests"
	$(HOST_CC) $(HOST_CFLAGS) -I"firmware/common" -I"firmware/sensor/app" \
	    "tests/sensor/test_sensor.c" "firmware/sensor/app/sample_clock.c" \
	    "firmware/sensor/app/pkt_ring.c" -o "build/tests/test_sensor"
	"build/tests/test_sensor"
	$(HOST_CC) $(HOST_CFLAGS) -I"tests/sensor/stubs" -I"firmware/common" \
	    -I"firmware/sensor/app" "tests/sensor/test_radio.c" \
	    "firmware/sensor/app/rf_tx.c" "firmware/sensor/app/pkt_ring.c" \
	    "firmware/sensor/app/packer.c" "firmware/sensor/app/status.c" \
	    -o "build/tests/test_sensor_radio"
	"build/tests/test_sensor_radio"
	$(HOST_CC) $(HOST_CFLAGS) -I"tests/sensor/stubs" -I"firmware/common" \
	    -I"firmware/sensor/app" "tests/sensor/test_boot.c" \
	    "firmware/common/crc16.c" -o "build/tests/test_sensor_boot"
	"build/tests/test_sensor_boot"
	$(HOST_CC) $(HOST_CFLAGS) -I"tests/sensor/stubs" -I"firmware/common" \
	    -I"firmware/sensor/app" "tests/sensor/test_command.c" \
	    "firmware/sensor/app/command.c" -o "build/tests/test_sensor_command"
	"build/tests/test_sensor_command"
	$(HOST_CC) $(HOST_CFLAGS) -DHAL_SLEEP=1 -I"tests/sensor/stubs" -I"firmware/common" \
	    -I"firmware/sensor/app" "tests/sensor/test_sampler.c" \
	    "firmware/sensor/app/sampler.c" "firmware/sensor/app/sample_clock.c" \
	    -o "build/tests/test_sensor_sampler"
	"build/tests/test_sensor_sampler"
	$(HOST_CC) $(HOST_CFLAGS) -DHAL_SLEEP=1 -DDCDC_ENABLE=1 \
	    -I"tests/sensor/stubs" -I"firmware/common" \
	    -I"firmware/sensor/app" "tests/sensor/test_startup.c" -o "build/tests/test_sensor_startup"
	"build/tests/test_sensor_startup"
	$(HOST_CC) $(HOST_CFLAGS) -DHAL_SLEEP=1 -DSENSOR_STANDBY_SLEEP=0 -DDCDC_ENABLE=1 \
	    -I"tests/sensor/stubs" -I"firmware/common" -I"firmware/sensor/app" \
	    "tests/sensor/test_startup.c" -o "build/tests/test_sensor_nosleep"
	"build/tests/test_sensor_nosleep"
	$(HOST_CC) $(HOST_CFLAGS) -I"tests/sensor/stubs" -I"firmware/common" -I"firmware/sensor/app" \
	    "tests/sensor/test_board.c" -o "build/tests/test_sensor_board"
	"build/tests/test_sensor_board"
	$(HOST_CC) $(HOST_CFLAGS) -DHAL_SLEEP=1 -I"tests/sensor/stubs" -I"firmware/common" \
	    -I"firmware/sensor/app" "tests/sensor/test_idle.c" "firmware/sensor/app/idle.c" \
	    -o "build/tests/test_sensor_idle"
	"build/tests/test_sensor_idle"

test-gateway:
	sh "tests/gateway/run.sh"

test-docs:
	node "docs/site/tests/codec.test.cjs"

test-web:
	node --test "web/tests/protocol.test.cjs" "web/tests/analysis.test.cjs" "web/tests/timeline.test.cjs"
