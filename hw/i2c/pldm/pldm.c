#include "qemu/osdep.h"
#include "qemu/crc32c.h"
#include "hw/i2c/i2c.h"
#include "hw/registerfields.h"
#include "hw/i2c/mctp.h"
#include "pldm.h"
#include "pldm_base.h"
#include "pldm_platform.h"

#define TYPE_PLDM_I2C_DEVICE "pldm-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(PLDMDevice, PLDM_I2C_DEVICE)

enum {
    PLDM_TYPE_CODE_BASE             = 0x0,
    PLDM_TYPE_CODE_PLATFORM         = 0x2,
    PLDM_TYPE_CODE_REDFISH_DEVICE   = 0x6,
};

static void pldm_handle_message(MCTPI2CEndpoint *mctp)
{
    PLDMDevice *pldmDev = PLDM_I2C_DEVICE(mctp);
    PLDMMessage *msg = (PLDMMessage *)pldmDev->buffer;

    uint8_t buf[] = {
        MCTP_MESSAGE_TYPE_PLDM,
        0,
        msg->hdrVer_pldmType, 
        msg->pldmCommandCode,
    };
    
    memcpy(pldmDev->scratch, buf, sizeof(buf));
    pldmDev->pos = sizeof(buf);

    switch (msg->hdrVer_pldmType) {
    case PLDM_TYPE_CODE_BASE: // Control and Discovery
        handle_message_type_base(pldmDev, msg);
        break;

    case PLDM_TYPE_CODE_PLATFORM: 
        handle_message_type_platform(pldmDev, msg);
        break;

    default:
        // fprintf(stderr, "%s[%d]: Unsupported PLDM type 0x%x\n", __FUNCTION__, __LINE__, msg->hdrVer_pldmType);
        break;
    }

    pldmDev->pos = 0;

    i2c_mctp_schedule_send(mctp);
}

static size_t pldm_get_message_bytes(MCTPI2CEndpoint *mctp, uint8_t *buf,
                                    size_t maxlen, uint8_t *mctp_flags)
{
    PLDMDevice *pldmDev = PLDM_I2C_DEVICE(mctp);
    size_t len;

    len = MIN(maxlen, pldmDev->len - pldmDev->pos);

    if (len == 0) {
        return 0;
    }

    if (pldmDev->pos == 0) {
        *mctp_flags |= MCTP_H_FLAGS_SOM;
    }

    memcpy(buf, pldmDev->scratch + pldmDev->pos, len);
    pldmDev->pos += len;

    if (pldmDev->pos == pldmDev->len) {
        *mctp_flags |= MCTP_H_FLAGS_EOM;

        pldmDev->pos = pldmDev->len = 0;
    }

    return len;
}

static int pldm_put_message_bytes(MCTPI2CEndpoint *mctp, uint8_t *buf,
                                 size_t len)
{
    PLDMDevice *pldmDev = PLDM_I2C_DEVICE(mctp);

    if (pldmDev->len + len > PLDM_MAX_MESSAGE_LENGTH) {
        return -1;
    }

    memcpy(pldmDev->buffer + pldmDev->len, buf, len);
    pldmDev->len += len;

    return 0;
}

static void pldm_reset_message(MCTPI2CEndpoint *mctp)
{
    PLDMDevice *pldmDev = PLDM_I2C_DEVICE(mctp);
    pldmDev->len = 0;
}

static size_t pldm_get_message_types(MCTPI2CEndpoint *mctp, uint8_t *data,
                                    size_t maxlen)
{
    uint8_t buf[] = {
        0x0, 0x1, 0x1,
    };

    memcpy(data, buf, sizeof(buf));

    return sizeof(buf);
}

static void pldm_vnc_class_init(ObjectClass *oc, void *data)
{
    MCTPI2CEndpointClass *mc = MCTP_I2C_ENDPOINT_CLASS(oc);

    mc->get_message_types = pldm_get_message_types;
    mc->get_message_bytes = pldm_get_message_bytes;
    mc->put_message_bytes = pldm_put_message_bytes;

    mc->handle_message = pldm_handle_message;
    mc->reset_message = pldm_reset_message;
}

static const TypeInfo pldm_vnc = {
    .name = TYPE_PLDM_I2C_DEVICE,
    .parent = TYPE_MCTP_I2C_ENDPOINT,
    .instance_size = sizeof(PLDMDevice),
    .class_init = pldm_vnc_class_init,
};

static void register_types(void)
{
    type_register_static(&pldm_vnc);
}

type_init(register_types);
