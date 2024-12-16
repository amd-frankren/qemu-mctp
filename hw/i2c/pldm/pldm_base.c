#include "qemu/osdep.h"
#include "qemu/crc32c.h"
#include "hw/i2c/i2c.h"
#include "hw/registerfields.h"
#include "hw/i2c/mctp.h"
#include "pldm_base.h"

enum {
    PLDM_COMMAND_BASE_SET_TID       = 0x1,
    PLDM_COMMAND_BASE_GET_TID       = 0x2,
    PLDM_COMMAND_BASE_GET_VERSION   = 0x3,
    PLDM_COMMAND_BASE_GET_TYPES     = 0x4,
};

void pldm_base_get_types(PLDMDevice *pldmDev)
{
    size_t len = 9;
    static uint8_t pldm_types[9] = {
        0x00,       // success
        0x05,       // PLDM Type 0*8
        0x00,       // PLDM Type 1*8
        0x00,       // PLDM Type 2*8
        0x00,       // PLDM Type 3*8
        0x00,       // PLDM Type 4*8
        0x00,       // PLDM Type 5*8
        0x00,       // PLDM Type 6*8
        0x00,       // PLDM Type 7*8
    };
    memcpy(pldmDev->scratch + pldmDev->pos, pldm_types, len);
    pldmDev->pos += len;
    pldmDev->len += len;
}

void handle_message_type_base(PLDMDevice *pldmDev, PLDMMessage *msg)
{
    switch (msg->pldmCommandCode) {

    case PLDM_COMMAND_BASE_GET_TYPES:
        pldm_base_get_types(pldmDev);
        break;

    default:
        fprintf(stderr, "%s[%d]: Unsupported PLDM base command 0x%x\n", __FUNCTION__, __LINE__, msg->pldmCommandCode);
        break;
    }
}

