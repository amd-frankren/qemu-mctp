#ifndef QEMU_I2C_PLDM_BASE_H
#define QEMU_I2C_PLDM_BASE_H

#include "qemu/osdep.h"
#include "qemu/crc32c.h"
#include "hw/i2c/i2c.h"
#include "hw/registerfields.h"
#include "hw/i2c/mctp.h"
#include "pldm.h"

void pldm_base_get_types(PLDMDevice *pldmDev);

void handle_message_type_base(PLDMDevice *pldmDev, PLDMMessage *msg);

#endif