#ifndef QEMU_I2C_PLDM_PLATFORM_H
#define QEMU_I2C_PLDM_PLATFORM_H

#include "qemu/osdep.h"
#include "qemu/crc32c.h"
#include "hw/i2c/i2c.h"
#include "hw/registerfields.h"
#include "hw/i2c/mctp.h"
#include "pldm.h"

void pldm_platform_get_sensor_reading(PLDMDevice *pldmDev, PLDMMessage *msg);

void handle_message_type_platform(PLDMDevice *pldmDev, PLDMMessage *msg);


#endif