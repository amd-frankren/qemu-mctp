#ifndef QEMU_I2C_PLDM_H
#define QEMU_I2C_PLDM_H

#include "qemu/osdep.h"
#include "qemu/crc32c.h"
#include "hw/i2c/i2c.h"
#include "hw/registerfields.h"
#include "hw/i2c/mctp.h"

#define PLDM_MAX_MESSAGE_LENGTH 4224

typedef struct PLDMDevice {
    MCTPI2CEndpoint mctp;

    uint8_t buffer[PLDM_MAX_MESSAGE_LENGTH];
    uint8_t scratch[PLDM_MAX_MESSAGE_LENGTH];

    size_t  len;
    int64_t pos;
} PLDMDevice;

typedef struct PLDMMessage {
    uint8_t ic_messageType;
    uint8_t rq_d_rsvd_instanceId;
    uint8_t hdrVer_pldmType;
    uint8_t pldmCommandCode;
    uint8_t payload[]; /* includes the Message Integrity Check */
} PLDMMessage;


#endif