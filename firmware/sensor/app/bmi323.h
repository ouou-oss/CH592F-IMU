#ifndef SENSOR_BMI323_H
#define SENSOR_BMI323_H
#include <stdint.h>
uint16_t bmi_read(uint8_t address);
void bmi_write(uint8_t address, uint16_t value);
void bmi_fifo_read(uint8_t *data, uint16_t bytes);
uint8_t bmi_initialize(uint8_t odr, uint8_t range, uint8_t bandwidth);
uint8_t bmi_suspend(void);
uint8_t bmi_arm_motion(void);
#endif
