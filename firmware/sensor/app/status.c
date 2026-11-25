#include "sensor.h"
#include "bmi323.h"
#include "stack_monitor.h"
#include <string.h>

void status_emit(void)
{
    app_stack_measure();
    uint8_t packet[63];
    memset(packet, 0, sizeof(packet)); packet_header(packet, 2);
    if (!sensor_stats.streaming) packet[2] |= PROTO_FLAG_PAUSED;
    proto_put_u32(packet + 14, sampler_index());
    proto_put_u16(packet + 20, sensor_stats.streaming ? bmi_read(0x09) : 0x8000u);
    proto_put_u32(packet + 22, sensor_stats.tx_ok);
    proto_put_u32(packet + 26, sensor_stats.tx_retry);
    proto_put_u32(packet + 30, sensor_stats.tx_backoff);
    proto_put_u32(packet + 34, sensor_ring.drop_packets);
    proto_put_u32(packet + 38, sensor_ring.drop_samples);
    proto_put_u32(packet + 42, sensor_stats.fifo_gap_samples);
    proto_put_u16(packet + 46, sensor_stats.fifo_full);
    packet[48] = sensor_ring.max_used; packet[49] = sensor_stats.cmd_ack;
    proto_put_u16(packet + 50, sensor_stats.bmi_error);
    packet[52] = (uint8_t)sensor_stats.ack_rssi;
    packet[53] = SENSOR_FW_MAJOR; packet[54] = SENSOR_FW_MINOR;
    packet[55] = sensor_stats.reinit;
    packet[56] = sensor_stats.gateway; packet[57] = 0;
    packet[58] = sensor_stats.motion_sleep ? PROTO_MODE_MOTION_SLEEP : PROTO_MODE_ACTIVE;
    packet_ring_push(&sensor_ring, packet, sizeof(packet));
    radio_request_tx();
#if SENSOR_DEBUG
    PRINT("n=%u i=%lu irq=%lu tx=%lu first=%lu/%lu retry=%lu drop=%lu gap=%lu unknown=%lu fin=%lu q=%u stack=%u guard=%u\n",
          sensor_config.node, (unsigned long)sensor_stats.samples,
          (unsigned long)sensor_stats.irq_count, (unsigned long)sensor_stats.tx_ok,
          (unsigned long)sensor_stats.tx_first_ok, (unsigned long)sensor_stats.tx_first_attempts,
          (unsigned long)sensor_stats.tx_retry,
          (unsigned long)sensor_ring.drop_samples, (unsigned long)sensor_stats.fifo_gap_samples,
          (unsigned long)sensor_stats.fifo_unknown,
          (unsigned long)sensor_stats.tx_finish, sensor_ring.used,
          app_stack.free_min_bytes, app_stack.guard_fault);
#endif
}
