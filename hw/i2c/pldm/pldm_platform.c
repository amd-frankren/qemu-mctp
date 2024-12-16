#include "qemu/osdep.h"
#include "qemu/crc32c.h"
#include "hw/i2c/i2c.h"
#include "hw/registerfields.h"
#include "hw/i2c/mctp.h"
#include "pldm_platform.h"

enum {
    PLDM_COMMAND_PLATFORM_GET_TID                     = 0x02,
    PLDM_COMMAND_PLATFORM_GET_SENSOR_READING          = 0x11,
    PLDM_COMMAND_PLATFORM_GET_SENSOR_THRESHOLDS       = 0x12,
    PLDM_COMMAND_PLATFORM_GET_STATE_SENSOR_READINGS   = 0x21,
    PLDM_COMMAND_PLATFORM_SET_STATE_EFFECTER_ENABLES  = 0x38,
    PLDM_COMMAND_PLATFORM_SET_STATE_EFFECTER_STATES   = 0x39,
};


enum {
    SENSOR_ID_READ_PACKAGE_POWER_CONSUMPTION  = 0x00,
    SENSOR_ID_READ_TDP                        = 0x01,
};


typedef struct GetSensorReadingRequestData {
    uint16_t sensor_id;
    uint8_t rearm_event_state;
} GetSensorReadingRequestData;


void pldm_platform_get_sensor_reading(PLDMDevice *pldmDev, PLDMMessage *msg)
{
    uint8_t pldm_types[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    size_t len = sizeof(pldm_types) / sizeof(pldm_types[0]);

    GetSensorReadingRequestData *requestData = (GetSensorReadingRequestData *)msg->payload;

    switch (requestData->sensor_id) {
    case SENSOR_ID_READ_PACKAGE_POWER_CONSUMPTION: 
        pldm_types[0] = 0x00;  // completion code, success
        pldm_types[1] = 0x03;  // sensor data size
        pldm_types[2] = 0x00;  // sensor operational state
        pldm_types[3] = 0x02;  // sensor event message enable
        pldm_types[4] = 0x01;  // present state
        pldm_types[5] = 0x00;  // previous state
        pldm_types[6] = 0x01;  // event state
        pldm_types[7] = 0x23;  // present reading
        pldm_types[8] = 0x00;  // present reading
        break;
    case SENSOR_ID_READ_TDP:
        pldm_types[0] = 0x00;  // completion code, success
        pldm_types[1] = 0x03;  // sensor data size
        pldm_types[2] = 0x00;  // sensor operational state
        pldm_types[3] = 0x02;  // sensor event message enable
        pldm_types[4] = 0x01;  // present state
        pldm_types[5] = 0x00;  // previous state
        pldm_types[6] = 0x01;  // event state
        pldm_types[7] = 0x45;  // present reading
        pldm_types[8] = 0x00;  // present reading
        break;
    default:
        fprintf(stderr, "%s[%d]: Unsupported sensor id 0x%x\n", __FUNCTION__, __LINE__, requestData->sensor_id);
        break;
    }

 
    memcpy(pldmDev->scratch + pldmDev->pos, pldm_types, len);
    pldmDev->pos += len;
    pldmDev->len += len;
}

void handle_message_type_platform(PLDMDevice *pldmDev, PLDMMessage *msg)
{
    switch (msg->pldmCommandCode) {

    case PLDM_COMMAND_PLATFORM_GET_SENSOR_READING:
        pldm_platform_get_sensor_reading(pldmDev, msg);
        break;

    default:
        fprintf(stderr, "%s[%d]: Unsupported PLDM platform command 0x%x\n", __FUNCTION__, __LINE__, msg->pldmCommandCode);
        break;
    }
}

