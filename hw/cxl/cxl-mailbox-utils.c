/*
 * CXL Utility library for mailbox interface
 *
 * Copyright(C) 2020 Intel Corporation.
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/cxl/cxl.h"
#include "hw/cxl/cxl_events.h"
#include "hw/pci/pci.h"
#include "hw/pci-bridge/cxl_upstream_port.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/uuid.h"
#include "sysemu/hostmem.h"

#define CXL_CAPACITY_MULTIPLIER   (256 * MiB)
/* Experimental value: dynamic capacity event log size */
#define CXL_DC_EVENT_LOG_SIZE 8

/*
 * How to add a new command, example. The command set FOO, with cmd BAR.
 *  1. Add the command set and cmd to the enum.
 *     FOO    = 0x7f,
 *          #define BAR 0
 *  2. Implement the handler
 *    static CXLRetCode cmd_foo_bar(struct cxl_cmd *cmd,
 *                                  CXLDeviceState *cxl_dstate, uint16_t *len)
 *  3. Add the command to the cxl_cmd_set[][]
 *    [FOO][BAR] = { "FOO_BAR", cmd_foo_bar, x, y },
 *  4. Implement your handler
 *     define_mailbox_handler(FOO_BAR) { ... return CXL_MBOX_SUCCESS; }
 *
 *
 *  Writing the handler:
 *    The handler will provide the &struct cxl_cmd, the &CXLDeviceState, and the
 *    in/out length of the payload. The handler is responsible for consuming the
 *    payload from cmd->payload and operating upon it as necessary. It must then
 *    fill the output data into cmd->payload (overwriting what was there),
 *    setting the length, and returning a valid return code.
 *
 *  XXX: The handler need not worry about endianess. The payload is read out of
 *  a register interface that already deals with it.
 */

enum {
    INFOSTAT    = 0x00,
        #define IS_IDENTIFY   0x1
        #define BACKGROUND_OPERATION_STATUS    0x2
        #define GET_RESPONSE_MESSAGE_LIMIT     0x3
        #define SET_RESPONSE_MESSAGE_LIMIT     0x4
    EVENTS      = 0x01,
        #define GET_RECORDS   0x0
        #define CLEAR_RECORDS   0x1
        #define GET_INTERRUPT_POLICY   0x2
        #define SET_INTERRUPT_POLICY   0x3
    FIRMWARE_UPDATE = 0x02,
        #define GET_INFO      0x0
    TIMESTAMP   = 0x03,
        #define GET           0x0
        #define SET           0x1
    LOGS        = 0x04,
        #define GET_SUPPORTED 0x0
        #define GET_LOG       0x1
    IDENTIFY    = 0x40,
        #define MEMORY_DEVICE 0x0
    CCLS        = 0x41,
        #define GET_PARTITION_INFO     0x0
        #define GET_LSA       0x2
        #define SET_LSA       0x3
    SANITIZE    = 0x44,
        #define OVERWRITE     0x0
        #define SECURE_ERASE  0x1
    MEDIA_AND_POISON = 0x43,
        #define GET_POISON_LIST        0x0
        #define INJECT_POISON          0x1
        #define CLEAR_POISON           0x2
    DCD_CONFIG  = 0x48, /*r3.0: 8.2.9.8.9*/
        #define GET_DC_CONFIG          0x0
        #define GET_DYN_CAP_EXT_LIST   0x1
        #define ADD_DYN_CAP_RSP        0x2
        #define RELEASE_DYN_CAP        0x3
    PHYSICAL_SWITCH = 0x51
        #define IDENTIFY_SWITCH_DEVICE      0x0
};


static CXLRetCode cmd_events_get_records(struct cxl_cmd *cmd,
                                         CXLDeviceState *cxlds,
                                         uint16_t *len)
{
    CXLGetEventPayload *pl;
    uint8_t log_type;
    int max_recs;

    if (cmd->in < sizeof(log_type)) {
        return CXL_MBOX_INVALID_INPUT;
    }

    log_type = *((uint8_t *)cmd->payload);

    pl = (CXLGetEventPayload *)cmd->payload;
    memset(pl, 0, sizeof(*pl));

    max_recs = (cxlds->payload_size - CXL_EVENT_PAYLOAD_HDR_SIZE) /
                CXL_EVENT_RECORD_SIZE;
    if (max_recs > 0xFFFF) {
        max_recs = 0xFFFF;
    }

    return cxl_event_get_records(cxlds, pl, log_type, max_recs, len);
}

static CXLRetCode cmd_events_clear_records(struct cxl_cmd *cmd,
                                           CXLDeviceState *cxlds,
                                           uint16_t *len)
{
    CXLClearEventPayload *pl;

    pl = (CXLClearEventPayload *)cmd->payload;
    *len = 0;
    return cxl_event_clear_records(cxlds, pl);
}

static CXLRetCode cmd_events_get_interrupt_policy(struct cxl_cmd *cmd,
                                                  CXLDeviceState *cxlds,
                                                  uint16_t *len)
{
    CXLEventInterruptPolicy *policy;
    CXLEventLog *log;

    policy = (CXLEventInterruptPolicy *)cmd->payload;
    memset(policy, 0, sizeof(*policy));

    log = &cxlds->event_logs[CXL_EVENT_TYPE_INFO];
    if (log->irq_enabled) {
        policy->info_settings = CXL_EVENT_INT_SETTING(log->irq_vec);
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_WARN];
    if (log->irq_enabled) {
        policy->warn_settings = CXL_EVENT_INT_SETTING(log->irq_vec);
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_FAIL];
    if (log->irq_enabled) {
        policy->failure_settings = CXL_EVENT_INT_SETTING(log->irq_vec);
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_FATAL];
    if (log->irq_enabled) {
        policy->fatal_settings = CXL_EVENT_INT_SETTING(log->irq_vec);
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_DYNAMIC_CAP];
    if (log->irq_enabled) {
        /* Dynamic Capacity borrows the same vector as info */
        policy->dyn_cap_settings = CXL_INT_MSI_MSIX;
    }

    *len = sizeof(*policy);
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_events_set_interrupt_policy(struct cxl_cmd *cmd,
                                                  CXLDeviceState *cxlds,
                                                  uint16_t *len)
{
    CXLEventInterruptPolicy *policy;
    CXLEventLog *log;

    if (*len < CXL_EVENT_INT_SETTING_MIN_LEN) {
        return CXL_MBOX_INVALID_PAYLOAD_LENGTH;
    }

    policy = (CXLEventInterruptPolicy *)cmd->payload;

    log = &cxlds->event_logs[CXL_EVENT_TYPE_INFO];
    log->irq_enabled = (policy->info_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    log = &cxlds->event_logs[CXL_EVENT_TYPE_WARN];
    log->irq_enabled = (policy->warn_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    log = &cxlds->event_logs[CXL_EVENT_TYPE_FAIL];
    log->irq_enabled = (policy->failure_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    log = &cxlds->event_logs[CXL_EVENT_TYPE_FATAL];
    log->irq_enabled = (policy->fatal_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    /* DCD is optional */
    if (*len < sizeof(*policy)) {
        return CXL_MBOX_SUCCESS;
    }

    log = &cxlds->event_logs[CXL_EVENT_TYPE_DYNAMIC_CAP];
    log->irq_enabled = (policy->dyn_cap_settings & CXL_EVENT_INT_MODE_MASK) ==
                        CXL_INT_MSI_MSIX;

    *len = sizeof(*policy);
    return CXL_MBOX_SUCCESS;
}

static void find_cxl_usp(PCIBus *b, PCIDevice *d, void *opaque)
{
    PCIDevice **found_dev = opaque;

    if (object_dynamic_cast(OBJECT(d), TYPE_CXL_USP)) {
        *found_dev = d;
    }
}

static PCIDevice *switch_cci_to_usp(PCIDevice *cci_pci_dev)
{
    /*
     * Assumptions in here that this port is on same bus as
     * a switch upstream port.  Otherwise we need to more clever
     * about CCI to switch connectivity.
     */
    PCIBus *pci_bus = pci_get_bus(cci_pci_dev);
    PCIDevice *pci_dev = NULL;

    pci_for_each_device_under_bus(pci_bus, find_cxl_usp, &pci_dev);

    return pci_dev;
}

/* CXL r3 8.2.9.1.1 */
static CXLRetCode cmd_infostat_identify(struct cxl_cmd *cmd,
                                        CXLDeviceState *cxl_dstate,
                                        uint16_t *len)
{
    /*
     * Assumptions in here that this port is on same bus as
     * a switch upstream port.  Otherwise we need to more clever
     * about CCI to switch connectivity.
     */

    /* Find a Peer Upstream Port */
    PCIDevice *cci_pci_dev = PCI_DEVICE(container_of(cxl_dstate,
                                                     struct CSWMBCCIDev,
                                                     cxl_dstate));
    PCIDevice *pci_dev = switch_cci_to_usp(cci_pci_dev);
    struct {
        uint16_t pcie_vid;
        uint16_t pcie_did;
        uint16_t pcie_subsys_vid;
        uint16_t pcie_subsys_id;
        uint64_t sn;
        uint8_t max_message_size;
        uint8_t component_type;
    } QEMU_PACKED *is_identify;
    QEMU_BUILD_BUG_ON(sizeof(*is_identify) != 18);
    is_identify = (void *)cmd->payload;
    memset(is_identify, 0, sizeof(*is_identify));
    if (pci_dev) {
        CXLUpstreamPort *port = CXL_USP(pci_dev);
        /*
         * Messy question - which IDs?  Those of the CCI Function, or those of
         * the USP?
         */
        is_identify->pcie_vid = pci_get_word(&pci_dev->config[PCI_VENDOR_ID]);
        is_identify->pcie_did = pci_get_word(&pci_dev->config[PCI_DEVICE_ID]);
        is_identify->pcie_subsys_vid = 0; /* Not defined for a USP */
        is_identify->pcie_subsys_id = 0; /* Not defined for a USP */

        is_identify->sn = port->sn;
        is_identify->max_message_size = CXL_MAILBOX_PAYLOAD_SHIFT;
        is_identify->component_type = 0; /* Switch */
    }
    *len = sizeof(*is_identify);
    return CXL_MBOX_SUCCESS;
}

static void cxl_count_dsp(PCIBus *b, PCIDevice *d, void *private)
{
    uint16_t *count = private;
    if (object_dynamic_cast(OBJECT(d), TYPE_CXL_DSP)) {
        *count = *count + 1;
    }
}

static void cxl_set_dsp_active_bm(PCIBus *b, PCIDevice *d,
                                  void *private)
{
    uint8_t *bm = private;
    if (object_dynamic_cast(OBJECT(d), TYPE_CXL_DSP)) {
        uint8_t port = PCIE_PORT(d)->port;
        bm[port / 8] |= 1 << (port % 8);
    }
}

/* CXL r3 8.2.9.1.1 */
static CXLRetCode cmd_identify_switch_device(struct cxl_cmd *cmd,
                                             CXLDeviceState *cxl_dstate,
                                             uint16_t *len)
{
    /* Find a Peer Upstream Port */
    PCIDevice *cci_pci_dev = PCI_DEVICE(container_of(cxl_dstate,
                                                     struct CSWMBCCIDev,
                                                     cxl_dstate));

    struct {
        uint8_t ingress_port_id;
        uint8_t rsvd;
        uint8_t num_physical_ports;
        uint8_t num_vcs;
        uint8_t active_port_bitmap[0x20];
        uint8_t active_vcs_bitmap[0x20];
        uint16_t total_vppbs;
        uint16_t bound_vppbs;
        uint8_t num_hdm_decoders_per_usp;
    } QEMU_PACKED *identify_sw_dev_rsp;
    QEMU_BUILD_BUG_ON(sizeof(*identify_sw_dev_rsp) != 0x49);
    PCIDevice *pci_dev = switch_cci_to_usp(cci_pci_dev);

    identify_sw_dev_rsp = (void *)cmd->payload;
    memset(identify_sw_dev_rsp, 0, sizeof(*identify_sw_dev_rsp));
    if (pci_dev) {
        PCIBus *pci_bus = pci_bridge_get_sec_bus(PCI_BRIDGE(pci_dev));
        uint16_t num_dsp = 0;

        pci_for_each_device_under_bus(pci_bus, cxl_count_dsp, &num_dsp);
        identify_sw_dev_rsp->ingress_port_id = PCIE_PORT(pci_dev)->port;
        identify_sw_dev_rsp->num_physical_ports = 1 + num_dsp;
         /* Will be a while before we get more complex and need more VCS */
        identify_sw_dev_rsp->num_vcs = 1;
        pci_for_each_device_under_bus(pci_bus, cxl_set_dsp_active_bm,
                                      identify_sw_dev_rsp->active_port_bitmap);

        identify_sw_dev_rsp->active_vcs_bitmap[0] = 0x1;
        identify_sw_dev_rsp->total_vppbs = num_dsp; /* Fixed binding */
        identify_sw_dev_rsp->bound_vppbs = num_dsp;
         /* Only one HDM decoder implemented so far */
        identify_sw_dev_rsp->num_hdm_decoders_per_usp = 1;
    }
    *len = sizeof(*identify_sw_dev_rsp);
    return CXL_MBOX_SUCCESS;
}

/* CXL r3.0 8.2.9.1.2 */
static CXLRetCode cmd_infostat_bg_op_sts(struct cxl_cmd *cmd,
                                         CXLDeviceState *cxl_dstate,
                                         uint16_t *len)
{
    struct {
        uint8_t status;
        uint8_t rsvd;
        uint16_t opcode;
        uint16_t returncode;
        uint16_t vendor_ext_status;
    } QEMU_PACKED *bg_op_status;
    QEMU_BUILD_BUG_ON(sizeof(*bg_op_status) != 8);

    bg_op_status = (void *)cmd->payload;
    memset(bg_op_status, 0, sizeof(*bg_op_status));
    bg_op_status->status = ARRAY_FIELD_EX64(cxl_dstate->mbox_reg_state64,
                                            CXL_DEV_BG_CMD_STS, PERCENTAGE_COMP) << 1;
    if (cxl_dstate->bg.runtime > 0) {
        bg_op_status->status |= 1U << 0;
    }
    bg_op_status->opcode = cxl_dstate->bg.opcode;
    bg_op_status->returncode = ARRAY_FIELD_EX64(cxl_dstate->mbox_reg_state64,
                                               CXL_DEV_BG_CMD_STS, RET_CODE);
    *len = sizeof(*bg_op_status);
    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.2.1 */
static CXLRetCode cmd_firmware_update_get_info(struct cxl_cmd *cmd,
                                               CXLDeviceState *cxl_dstate,
                                               uint16_t *len)
{
    struct {
        uint8_t slots_supported;
        uint8_t slot_info;
        uint8_t caps;
        uint8_t rsvd[0xd];
        char fw_rev1[0x10];
        char fw_rev2[0x10];
        char fw_rev3[0x10];
        char fw_rev4[0x10];
    } QEMU_PACKED *fw_info;
    QEMU_BUILD_BUG_ON(sizeof(*fw_info) != 0x50);
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);

    if ((cxl_dstate->vmem_size < CXL_CAPACITY_MULTIPLIER) ||
        (cxl_dstate->pmem_size < CXL_CAPACITY_MULTIPLIER) ||
        (ct3d->dc.total_capacity < CXL_CAPACITY_MULTIPLIER)) {
        return CXL_MBOX_INTERNAL_ERROR;
    }

    fw_info = (void *)cmd->payload;
    memset(fw_info, 0, sizeof(*fw_info));

    fw_info->slots_supported = 2;
    fw_info->slot_info = BIT(0) | BIT(3);
    fw_info->caps = 0;
    pstrcpy(fw_info->fw_rev1, sizeof(fw_info->fw_rev1), "BWFW VERSION 0");

    *len = sizeof(*fw_info);
    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.3.1 */
static CXLRetCode cmd_timestamp_get(struct cxl_cmd *cmd,
                                    CXLDeviceState *cxl_dstate,
                                    uint16_t *len)
{
    uint64_t final_time = cxl_device_get_timestamp(cxl_dstate);

    stq_le_p(cmd->payload, final_time);
    *len = 8;

    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.3.2 */
static CXLRetCode cmd_timestamp_set(struct cxl_cmd *cmd,
                                  CXLDeviceState *cxl_dstate,
                                  uint16_t *len)
{
    cxl_dstate->timestamp.set = true;
    cxl_dstate->timestamp.last_set = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    cxl_dstate->timestamp.host_set = le64_to_cpu(*(uint64_t *)cmd->payload);

    *len = 0;
    return CXL_MBOX_SUCCESS;
}

/* CXL 3.0 8.2.9.5.2.1 Command Effects Log (CEL) */
static const QemuUUID cel_uuid = {
    .data = UUID(0x0da9c0b5, 0xbf41, 0x4b78, 0x8f, 0x79,
                 0x96, 0xb1, 0x62, 0x3b, 0x3f, 0x17)
};

/* 8.2.9.4.1 */
static CXLRetCode cmd_logs_get_supported(struct cxl_cmd *cmd,
                                         CXLDeviceState *cxl_dstate,
                                         uint16_t *len)
{
    struct {
        uint16_t entries;
        uint8_t rsvd[6];
        struct {
            QemuUUID uuid;
            uint32_t size;
        } log_entries[1];
    } QEMU_PACKED *supported_logs = (void *)cmd->payload;
    QEMU_BUILD_BUG_ON(sizeof(*supported_logs) != 0x1c);

    supported_logs->entries = 1;
    supported_logs->log_entries[0].uuid = cel_uuid;
    supported_logs->log_entries[0].size = 4 * cxl_dstate->cel_size;

    *len = sizeof(*supported_logs);
    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.4.2 */
static CXLRetCode cmd_logs_get_log(struct cxl_cmd *cmd,
                                   CXLDeviceState *cxl_dstate,
                                   uint16_t *len)
{
    struct {
        QemuUUID uuid;
        uint32_t offset;
        uint32_t length;
    } QEMU_PACKED QEMU_ALIGNED(16) *get_log = (void *)cmd->payload;

    /*
     * 8.2.9.4.2
     *   The device shall return Invalid Parameter if the Offset or Length
     *   fields attempt to access beyond the size of the log as reported by Get
     *   Supported Logs.
     *
     * XXX: Spec is wrong, "Invalid Parameter" isn't a thing.
     * XXX: Spec doesn't address incorrect UUID incorrectness.
     *
     * The CEL buffer is large enough to fit all commands in the emulation, so
     * the only possible failure would be if the mailbox itself isn't big
     * enough.
     */
    if (get_log->offset + get_log->length > cxl_dstate->payload_size) {
        return CXL_MBOX_INVALID_INPUT;
    }

    if (!qemu_uuid_is_equal(&get_log->uuid, &cel_uuid)) {
        return CXL_MBOX_UNSUPPORTED;
    }

    /* Store off everything to local variables so we can wipe out the payload */
    *len = get_log->length;

    memmove(cmd->payload, cxl_dstate->cel_log + get_log->offset,
           get_log->length);

    return CXL_MBOX_SUCCESS;
}

/* 8.2.9.5.1.1 */
static CXLRetCode cmd_identify_memory_device(struct cxl_cmd *cmd,
                                             CXLDeviceState *cxl_dstate,
                                             uint16_t *len)
{
    struct {
        char fw_revision[0x10];
        uint64_t total_capacity;
        uint64_t volatile_capacity;
        uint64_t persistent_capacity;
        uint64_t partition_align;
        uint16_t info_event_log_size;
        uint16_t warning_event_log_size;
        uint16_t failure_event_log_size;
        uint16_t fatal_event_log_size;
        uint32_t lsa_size;
        uint8_t poison_list_max_mer[3];
        uint16_t inject_poison_limit;
        uint8_t poison_caps;
        uint8_t qos_telemetry_caps;
        uint16_t dc_event_log_size;
    } QEMU_PACKED *id;
    QEMU_BUILD_BUG_ON(sizeof(*id) != 0x45);

    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);

    if ((!QEMU_IS_ALIGNED(cxl_dstate->vmem_size, CXL_CAPACITY_MULTIPLIER)) ||
        (!QEMU_IS_ALIGNED(cxl_dstate->pmem_size, CXL_CAPACITY_MULTIPLIER)) ||
        (!QEMU_IS_ALIGNED(ct3d->dc.total_capacity, CXL_CAPACITY_MULTIPLIER))) {
        return CXL_MBOX_INTERNAL_ERROR;
    }

    id = (void *)cmd->payload;
    memset(id, 0, sizeof(*id));

    snprintf(id->fw_revision, 0x10, "BWFW VERSION %02d", 0);

    stq_le_p(&id->total_capacity,
            cxl_dstate->static_mem_size / CXL_CAPACITY_MULTIPLIER);
    stq_le_p(&id->persistent_capacity, cxl_dstate->pmem_size / CXL_CAPACITY_MULTIPLIER);
    stq_le_p(&id->volatile_capacity, cxl_dstate->vmem_size / CXL_CAPACITY_MULTIPLIER);
    stl_le_p(&id->lsa_size, cvc->get_lsa_size(ct3d));
    /* 256 poison records */
    st24_le_p(id->poison_list_max_mer, 256);
    /* No limit - so limited by main poison record limit */
    stw_le_p(&id->inject_poison_limit, 0);
    stw_le_p(&id->dc_event_log_size, CXL_DC_EVENT_LOG_SIZE);

    *len = sizeof(*id);
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_ccls_get_partition_info(struct cxl_cmd *cmd,
                                              CXLDeviceState *cxl_dstate,
                                              uint16_t *len)
{
    struct {
        uint64_t active_vmem;
        uint64_t active_pmem;
        uint64_t next_vmem;
        uint64_t next_pmem;
    } QEMU_PACKED *part_info = (void *)cmd->payload;
    QEMU_BUILD_BUG_ON(sizeof(*part_info) != 0x20);
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);

    if ((!QEMU_IS_ALIGNED(cxl_dstate->vmem_size, CXL_CAPACITY_MULTIPLIER)) ||
        (!QEMU_IS_ALIGNED(cxl_dstate->pmem_size, CXL_CAPACITY_MULTIPLIER)) ||
        (!QEMU_IS_ALIGNED(ct3d->dc.total_capacity, CXL_CAPACITY_MULTIPLIER))) {
        return CXL_MBOX_INTERNAL_ERROR;
    }

    stq_le_p(&part_info->active_vmem, cxl_dstate->vmem_size / CXL_CAPACITY_MULTIPLIER);
    /*
     * When both next_vmem and next_pmem are 0, there is no pending change to
     * partitioning.
     */
    stq_le_p(&part_info->next_vmem, 0);
    stq_le_p(&part_info->active_pmem, cxl_dstate->pmem_size / CXL_CAPACITY_MULTIPLIER);
    stq_le_p(&part_info->next_pmem, 0);

    *len = sizeof(*part_info);
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_ccls_get_lsa(struct cxl_cmd *cmd,
                                   CXLDeviceState *cxl_dstate,
                                   uint16_t *len)
{
    struct {
        uint32_t offset;
        uint32_t length;
    } QEMU_PACKED *get_lsa;
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);
    uint32_t offset, length;

    get_lsa = (void *)cmd->payload;
    offset = get_lsa->offset;
    length = get_lsa->length;

    if (offset + length > cvc->get_lsa_size(ct3d)) {
        *len = 0;
        return CXL_MBOX_INVALID_INPUT;
    }

    *len = cvc->get_lsa(ct3d, get_lsa, length, offset);
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_ccls_set_lsa(struct cxl_cmd *cmd,
                                   CXLDeviceState *cxl_dstate,
                                   uint16_t *len)
{
    struct set_lsa_pl {
        uint32_t offset;
        uint32_t rsvd;
        uint8_t data[];
    } QEMU_PACKED;
    struct set_lsa_pl *set_lsa_payload = (void *)cmd->payload;
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);
    const size_t hdr_len = offsetof(struct set_lsa_pl, data);
    uint16_t plen = *len;

    *len = 0;
    if (!plen) {
        return CXL_MBOX_SUCCESS;
    }

    if (set_lsa_payload->offset + plen > cvc->get_lsa_size(ct3d) + hdr_len) {
        return CXL_MBOX_INVALID_INPUT;
    }
    plen -= hdr_len;

    cvc->set_lsa(ct3d, set_lsa_payload->data, plen, set_lsa_payload->offset);
    return CXL_MBOX_SUCCESS;
}

/* Perform the actual device zeroing */
static void __do_sanitization(CXLDeviceState *cxl_dstate)
{
    MemoryRegion *mr;
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);

    if (ct3d->hostvmem) {
        mr = host_memory_backend_get_memory(ct3d->hostvmem);
        if (mr) {
            void *hostmem = memory_region_get_ram_ptr(mr);
            memset(hostmem, 0, memory_region_size(mr));
        }
    }

    if (ct3d->hostpmem) {
        mr = host_memory_backend_get_memory(ct3d->hostpmem);
        if (mr) {
            void *hostmem = memory_region_get_ram_ptr(mr);
            memset(hostmem, 0, memory_region_size(mr));
        }
    }
    if (ct3d->lsa) {
        mr = host_memory_backend_get_memory(ct3d->lsa);
        if (mr) {
            void *lsa = memory_region_get_ram_ptr(mr);
            memset(lsa, 0, memory_region_size(mr));
        }
    }
}

/*
 * CXL 3.0 spec section 8.2.9.8.5.1 - Sanitize.
 *
 * Once the Sanitize command has started successfully, the device shall be
 * placed in the media disabled state. If the command fails or is interrupted
 * by a reset or power failure, it shall remain in the media disabled state
 * until a successful Sanitize command has been completed. During this state:
 *
 * 1. Memory writes to the device will have no effect, and all memory reads
 * will return random values (no user data returned, even for locations that
 * the failed Sanitize operation didn’t sanitize yet).
 *
 * 2. Mailbox commands shall still be processed in the disabled state, except
 * that commands that access Sanitized areas shall fail with the Media Disabled
 * error code.
 */
static CXLRetCode cmd_sanitize_overwrite(struct cxl_cmd *cmd,
                                         CXLDeviceState *cxl_dstate,
                                         uint16_t *len)
{
    uint64_t total_mem; /* in Mb */
    int secs;

    total_mem = (cxl_dstate->vmem_size + cxl_dstate->pmem_size) >> 20;
    if (total_mem <= 512) {
        secs = 4;
    } else if (total_mem <= 1024) {
        secs = 8;
    } else if (total_mem <= 2 * 1024) {
        secs = 15;
    } else if (total_mem <= 4 * 1024) {
        secs = 30;
    } else if (total_mem <= 8 * 1024) {
        secs = 60;
    } else if (total_mem <= 16 * 1024) {
        secs = 2 * 60;
    } else if (total_mem <= 32 * 1024) {
        secs = 4 * 60;
    } else if (total_mem <= 64 * 1024) {
        secs = 8 * 60;
    } else if (total_mem <= 128 * 1024) {
        secs = 15 * 60;
    } else if (total_mem <= 256 * 1024) {
        secs = 30 * 60;
    } else if (total_mem <= 512 * 1024) {
        secs = 60 * 60;
    } else if (total_mem <= 1024 * 1024) {
        secs = 120 * 60;
    } else {
        secs = 240 * 60; /* max 4 hrs */
    }

    /* EBUSY other bg cmds as of now */
    cxl_dstate->bg.runtime = secs * 1000UL;
    *len = 0;

    qemu_log_mask(LOG_UNIMP,
                  "Sanitize/overwrite command runtime for %ldMb media: %d seconds\n",
                  total_mem, secs);

    cxl_dev_disable_media(cxl_dstate);

    if (secs > 2) {
        /* sanitize when done */
        return CXL_MBOX_BG_STARTED;
    } else {
        __do_sanitization(cxl_dstate);
        cxl_dev_enable_media(cxl_dstate);

        return CXL_MBOX_SUCCESS;
    }
}

/*
 * This is very inefficient, but good enough for now!
 * Also the payload will always fit, so no need to handle the MORE flag and
 * make this stateful. We may want to allow longer poison lists to aid
 * testing that kernel functionality.
 */
static CXLRetCode cmd_media_get_poison_list(struct cxl_cmd *cmd,
                                            CXLDeviceState *cxl_dstate,
                                            uint16_t *len)
{
    struct get_poison_list_pl {
        uint64_t pa;
        uint64_t length;
    } QEMU_PACKED;

    struct get_poison_list_out_pl {
        uint8_t flags;
        uint8_t rsvd1;
        uint64_t overflow_timestamp;
        uint16_t count;
        uint8_t rsvd2[0x14];
        struct {
            uint64_t addr;
            uint32_t length;
            uint32_t resv;
        } QEMU_PACKED records[];
    } QEMU_PACKED;

    struct get_poison_list_pl *in = (void *)cmd->payload;
    struct get_poison_list_out_pl *out = (void *)cmd->payload;
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);
    uint16_t record_count = 0, i = 0;
    uint64_t query_start, query_length;
    CXLPoisonList *poison_list = &ct3d->poison_list;
    CXLPoison *ent;
    uint16_t out_pl_len;

    query_start = ldq_le_p(&in->pa);
    /* 64 byte alignemnt required */
    if (query_start & 0x3f) {
        return CXL_MBOX_INVALID_INPUT;
    }
    query_length = ldq_le_p(&in->length) * CXL_CACHE_LINE_SIZE;

    QLIST_FOREACH(ent, poison_list, node) {
        /* Check for no overlap */
        if (ent->start >= query_start + query_length ||
            ent->start + ent->length <= query_start) {
            continue;
        }
        record_count++;
    }
    out_pl_len = sizeof(*out) + record_count * sizeof(out->records[0]);
    assert(out_pl_len <= CXL_MAILBOX_MAX_PAYLOAD_SIZE);

    memset(out, 0, out_pl_len);
    QLIST_FOREACH(ent, poison_list, node) {
        uint64_t start, stop;

        /* Check for no overlap */
        if (ent->start >= query_start + query_length ||
            ent->start + ent->length <= query_start) {
            continue;
        }

        /* Deal with overlap */
        start = MAX(ROUND_DOWN(ent->start, 64ull), query_start);
        stop = MIN(ROUND_DOWN(ent->start, 64ull) + ent->length,
                   query_start + query_length);
        stq_le_p(&out->records[i].addr, start | (ent->type & 0x7));
        stl_le_p(&out->records[i].length, (stop - start) / CXL_CACHE_LINE_SIZE);
        i++;
    }
    if (ct3d->poison_list_overflowed) {
        out->flags = (1 << 1);
        stq_le_p(&out->overflow_timestamp, ct3d->poison_list_overflow_ts);
    }
    stw_le_p(&out->count, record_count);
    *len = out_pl_len;
    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_media_inject_poison(struct cxl_cmd *cmd,
                                          CXLDeviceState *cxl_dstate,
                                          uint16_t *len_unused)
{
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);
    CXLPoisonList *poison_list = &ct3d->poison_list;
    CXLPoison *ent;
    struct inject_poison_pl {
        uint64_t dpa;
    };
    struct inject_poison_pl *in = (void *)cmd->payload;
    uint64_t dpa = ldq_le_p(&in->dpa);
    CXLPoison *p;

    QLIST_FOREACH(ent, poison_list, node) {
        if (dpa >= ent->start &&
            dpa + CXL_CACHE_LINE_SIZE <= ent->start + ent->length) {
            return CXL_MBOX_SUCCESS;
        }
    }

    if (ct3d->poison_list_cnt == CXL_POISON_LIST_LIMIT) {
        return CXL_MBOX_INJECT_POISON_LIMIT;
    }
    p = g_new0(CXLPoison, 1);

    p->length = CXL_CACHE_LINE_SIZE;
    p->start = dpa;
    p->type = CXL_POISON_TYPE_INJECTED;

    /*
     * Possible todo: Merge with existing entry if next to it and if same type
     */
    QLIST_INSERT_HEAD(poison_list, p, node);
    ct3d->poison_list_cnt++;

    return CXL_MBOX_SUCCESS;
}

static CXLRetCode cmd_media_clear_poison(struct cxl_cmd *cmd,
                                         CXLDeviceState *cxl_dstate,
                                         uint16_t *len_unused)
{
    CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);
    CXLPoisonList *poison_list = &ct3d->poison_list;
    CXLType3Class *cvc = CXL_TYPE3_GET_CLASS(ct3d);
    struct clear_poison_pl {
        uint64_t dpa;
        uint8_t data[64];
    };
    CXLPoison *ent;
    uint64_t dpa;

    struct clear_poison_pl *in = (void *)cmd->payload;

    dpa = ldq_le_p(&in->dpa);
    if (dpa + CXL_CACHE_LINE_SIZE >= cxl_dstate->static_mem_size
            && ct3d->dc.num_regions == 0) {
        return CXL_MBOX_INVALID_PA;
    }

    if (ct3d->dc.num_regions && dpa + CXL_CACHE_LINE_SIZE >=
            cxl_dstate->static_mem_size + ct3d->dc.total_capacity) {
        return CXL_MBOX_INVALID_PA;
    }

    /* Clearing a region with no poison is not an error so always do so */
    if (cvc->set_cacheline) {
        if (!cvc->set_cacheline(ct3d, dpa, in->data)) {
            return CXL_MBOX_INTERNAL_ERROR;
        }
    }

    QLIST_FOREACH(ent, poison_list, node) {
        /*
         * Test for contained in entry. Simpler than general case
         * as clearing 64 bytes and entries 64 byte aligned
         */
        if ((dpa >= ent->start) && (dpa < ent->start + ent->length)) {
            break;
        }
    }
    if (!ent) {
        return CXL_MBOX_SUCCESS;
    }

    QLIST_REMOVE(ent, node);
    ct3d->poison_list_cnt--;

    if (dpa > ent->start) {
        CXLPoison *frag;
        /* Cannot overflow as replacing existing entry */

        frag = g_new0(CXLPoison, 1);

        frag->start = ent->start;
        frag->length = dpa - ent->start;
        frag->type = ent->type;

        QLIST_INSERT_HEAD(poison_list, frag, node);
        ct3d->poison_list_cnt++;
    }

    if (dpa + CXL_CACHE_LINE_SIZE < ent->start + ent->length) {
        CXLPoison *frag;

        if (ct3d->poison_list_cnt == CXL_POISON_LIST_LIMIT) {
            cxl_set_poison_list_overflowed(ct3d);
        } else {
            frag = g_new0(CXLPoison, 1);

            frag->start = dpa + CXL_CACHE_LINE_SIZE;
            frag->length = ent->start + ent->length - frag->start;
            frag->type = ent->type;
            QLIST_INSERT_HEAD(poison_list, frag, node);
            ct3d->poison_list_cnt++;
        }
    }
    /* Any fragments have been added, free original entry */
    g_free(ent);

    return CXL_MBOX_SUCCESS;
}

/*
 * cxl spec 3.0: 8.2.9.8.9.1
 * Get Dynamic Capacity Configuration
 **/
static CXLRetCode cmd_dcd_get_dyn_cap_config(struct cxl_cmd *cmd,
        CXLDeviceState *cxl_dstate,
        uint16_t *len)
{
    struct get_dyn_cap_config_in_pl {
        uint8_t region_cnt;
        uint8_t start_region_id;
    } QEMU_PACKED;

    struct get_dyn_cap_config_out_pl {
        uint8_t num_regions;
        uint8_t rsvd1[7];
        struct {
            uint64_t base;
            uint64_t decode_len;
            uint64_t region_len;
            uint64_t block_size;
            uint32_t dsmadhandle;
            uint8_t flags;
            uint8_t rsvd2[3];
        } QEMU_PACKED records[];
    } QEMU_PACKED;

    struct get_dyn_cap_config_in_pl *in = (void *)cmd->payload;
    struct get_dyn_cap_config_out_pl *out = (void *)cmd->payload;
    struct CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev,
            cxl_dstate);
    uint16_t record_count = 0, i;
    uint16_t out_pl_len;
    uint8_t start_region_id = in->start_region_id;

    if (start_region_id >= ct3d->dc.num_regions) {
        return CXL_MBOX_INVALID_INPUT;
    }

    record_count = MIN(ct3d->dc.num_regions - in->start_region_id,
            in->region_cnt);

    out_pl_len = sizeof(*out) + record_count * sizeof(out->records[0]);
    assert(out_pl_len <= CXL_MAILBOX_MAX_PAYLOAD_SIZE);

    memset(out, 0, out_pl_len);
    out->num_regions = record_count;
    for (i = 0; i < record_count; i++) {
        stq_le_p(&out->records[i].base,
                ct3d->dc.regions[start_region_id + i].base);
        stq_le_p(&out->records[i].decode_len,
                ct3d->dc.regions[start_region_id + i].decode_len);
        stq_le_p(&out->records[i].region_len,
                ct3d->dc.regions[start_region_id + i].len);
        stq_le_p(&out->records[i].block_size,
                ct3d->dc.regions[start_region_id + i].block_size);
        stl_le_p(&out->records[i].dsmadhandle,
                ct3d->dc.regions[start_region_id + i].dsmadhandle);
        out->records[i].flags = ct3d->dc.regions[start_region_id + i].flags;
    }

    *len = out_pl_len;
    return CXL_MBOX_SUCCESS;
}

/*
 * cxl spec 3.0: 8.2.9.8.9.2
 * Get Dynamic Capacity Extent List (Opcode 4810h)
 */
static CXLRetCode cmd_dcd_get_dyn_cap_ext_list(struct cxl_cmd *cmd,
        CXLDeviceState *cxl_dstate,
        uint16_t *len)
{
    struct get_dyn_cap_ext_list_in_pl {
        uint32_t extent_cnt;
        uint32_t start_extent_id;
    } QEMU_PACKED;

    struct get_dyn_cap_ext_list_out_pl {
        uint32_t count;
        uint32_t total_extents;
        uint32_t generation_num;
        uint8_t rsvd[4];
        CXLDCExtent_raw records[];
    } QEMU_PACKED;

    struct get_dyn_cap_ext_list_in_pl *in = (void *)cmd->payload;
    struct get_dyn_cap_ext_list_out_pl *out = (void *)cmd->payload;
    struct CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev,
            cxl_dstate);
    uint16_t record_count = 0, i = 0, record_done = 0;
    CXLDCDExtentList *extent_list = &ct3d->dc.extents;
    CXLDCD_Extent *ent;
    uint16_t out_pl_len;
    uint32_t start_extent_id = in->start_extent_id;

    if (start_extent_id > ct3d->dc.total_extent_count) {
        return CXL_MBOX_INVALID_INPUT;
    }

    record_count = MIN(in->extent_cnt,
            ct3d->dc.total_extent_count - start_extent_id);

    out_pl_len = sizeof(*out) + record_count * sizeof(out->records[0]);
    /* May need more processing here in the future */
    assert(out_pl_len <= CXL_MAILBOX_MAX_PAYLOAD_SIZE);

    memset(out, 0, out_pl_len);
    stl_le_p(&out->count, record_count);
    stl_le_p(&out->total_extents, ct3d->dc.total_extent_count);
    stl_le_p(&out->generation_num, ct3d->dc.ext_list_gen_seq);

    if (record_count > 0) {
        QTAILQ_FOREACH(ent, extent_list, node) {
            if (i++ < start_extent_id) {
                continue;
            }
            stq_le_p(&out->records[record_done].start_dpa, ent->start_dpa);
            stq_le_p(&out->records[record_done].len, ent->len);
            memcpy(&out->records[record_done].tag, ent->tag, 0x10);
            stw_le_p(&out->records[record_done].shared_seq, ent->shared_seq);
            record_done++;
            if (record_done == record_count) {
                break;
            }
        }
    }

    *len = out_pl_len;
    return CXL_MBOX_SUCCESS;
}

/*
 * Check whether the bits at addr between [nr, nr+size) are all set,
 * return 1 if all 1s, else return 0
 */
static inline int test_bits(const unsigned long *addr, int nr, int size)
{
    unsigned long res = find_next_zero_bit(addr, size + nr, nr);

    return (res >= nr + size) ? 1 : 0;
}

/*
 * Find dynamic capacity region id based on dpa range [dpa, dpa+len)
 */
static uint8_t find_region_id(struct CXLType3Dev *dev, uint64_t dpa,
        uint64_t len)
{
    int8_t i = dev->dc.num_regions - 1;

    while (i > 0 && dpa < dev->dc.regions[i].base) {
        i--;
    }

    if (dpa < dev->dc.regions[i].base
            || dpa + len > dev->dc.regions[i].base + dev->dc.regions[i].len) {
        return dev->dc.num_regions;
    }

    return i;
}

static void insert_extent_to_extent_list(CXLDCDExtentList *list, uint64_t dpa,
        uint64_t len, uint8_t *tag, uint16_t shared_seq)
{
    CXLDCD_Extent *extent;
    extent = g_new0(CXLDCD_Extent, 1);
    extent->start_dpa = dpa;
    extent->len = len;
    if (tag) {
        memcpy(extent->tag, tag, 0x10);
    } else {
        memset(extent->tag, 0, 0x10);
    }
    extent->shared_seq = shared_seq;

    QTAILQ_INSERT_TAIL(list, extent, node);
}

typedef struct updated_dc_extent_list_in_pl {
    uint32_t num_entries_updated;
    uint8_t rsvd[4];
    struct { /* r3.0: Table 8-130 */
        uint64_t start_dpa;
        uint64_t len;
        uint8_t rsvd[8];
    } QEMU_PACKED updated_entries[];
} QEMU_PACKED updated_dc_extent_list_in_pl;

/*
 * The function only check the input extent list against itself.
 */
static CXLRetCode detect_malformed_extent_list(CXLType3Dev *dev,
        const updated_dc_extent_list_in_pl *in)
{
    unsigned long *blk_bitmap;
    uint64_t min_block_size = dev->dc.regions[0].block_size;
    struct CXLDCD_Region *region = &dev->dc.regions[0];
    uint32_t i;
    uint64_t dpa, len;
    uint8_t rid;
    CXLRetCode ret;

    for (i = 1; i < dev->dc.num_regions; i++) {
        region = &dev->dc.regions[i];
        if (min_block_size > region->block_size) {
            min_block_size = region->block_size;
        }
    }

    blk_bitmap = bitmap_new((region->len + region->base
                - dev->dc.regions[0].base) / min_block_size);

    for (i = 0; i < in->num_entries_updated; i++) {
        dpa = in->updated_entries[i].start_dpa;
        len = in->updated_entries[i].len;

        rid = find_region_id(dev, dpa, len);
        if (rid == dev->dc.num_regions) {
            ret = CXL_MBOX_INVALID_PA;
            goto out;
        }

        region = &dev->dc.regions[rid];
        if (dpa % region->block_size || len % region->block_size) {
            ret = CXL_MBOX_INVALID_EXTENT_LIST;
            goto out;
        }
        /* the dpa range already covered by some other extents in the list */
        if (test_bits(blk_bitmap, dpa / min_block_size, len / min_block_size)) {
            ret = CXL_MBOX_INVALID_EXTENT_LIST;
            goto out;
        }
        bitmap_set(blk_bitmap, dpa / min_block_size, len / min_block_size);
   }

    ret = CXL_MBOX_SUCCESS;

out:
    g_free(blk_bitmap);
    return ret;
}

/*
 * cxl spec 3.0: 8.2.9.8.9.3
 * Add Dynamic Capacity Response (opcode 4802h)
 * Assume an extent is added only after the response is processed successfully
 * TODO: for better extent list validation, a better solution would be
 * maintaining a pending extent list and use it to verify the extent list in
 * the response.
 */
static CXLRetCode cmd_dcd_add_dyn_cap_rsp(struct cxl_cmd *cmd,
        CXLDeviceState *cxl_dstate, uint16_t *len_unused)
{
    updated_dc_extent_list_in_pl *in = (void *)cmd->payload;
    struct CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev,
            cxl_dstate);
    CXLDCDExtentList *extent_list = &ct3d->dc.extents;
    CXLDCD_Extent *ent;
    uint32_t i;
    uint64_t dpa, len;
    CXLRetCode ret;
 
    if (in->num_entries_updated == 0) {
        ret = CXL_MBOX_SUCCESS;
        goto out;
    }

    ret = detect_malformed_extent_list(ct3d, in);
    if (ret != CXL_MBOX_SUCCESS) {
        goto out;
    }

    for (i = 0; i < in->num_entries_updated; i++) {
        dpa = in->updated_entries[i].start_dpa;
        len = in->updated_entries[i].len;

        /*
         * Check if the DPA range of the to-be-added extent overlaps with
         * existing extent list maintained by the device.
         */
        QTAILQ_FOREACH(ent, extent_list, node) {
            if (ent->start_dpa == dpa && ent->len == len) {
                ret = CXL_MBOX_INVALID_PA;
                goto out;
            } else if (ent->start_dpa <= dpa
                    && dpa + len <= ent->start_dpa + ent->len) {
                ret = CXL_MBOX_INVALID_PA;
                goto out;
            } else if ((dpa < ent->start_dpa + ent->len
                        && dpa + len > ent->start_dpa + ent->len)
                    || (dpa < ent->start_dpa && dpa + len > ent->start_dpa)) {
                ret = CXL_MBOX_INVALID_PA;
                goto out;
            }
        }

        /*
         * TODO: add a pending extent list based on event log record and verify
         * the input response
         */

        insert_extent_to_extent_list(extent_list, dpa, len, NULL, 0);
    }
    ret = CXL_MBOX_SUCCESS;

out:
    return ret;
}

/*
 * Spec 3.0: 8.2.9.8.9.4
 * Release Dynamic Capacity (opcode 4803h)
 **/
static CXLRetCode cmd_dcd_release_dyn_cap(struct cxl_cmd *cmd,
        CXLDeviceState *cxl_dstate,
        uint16_t *len_unused)
{
    updated_dc_extent_list_in_pl *in = (void *)cmd->payload;
    struct CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev,
            cxl_dstate);
    CXLDCDExtentList *extent_list = &ct3d->dc.extents;
    CXLDCD_Extent *ent;
    uint32_t i;
    uint64_t dpa, len;
    CXLRetCode ret;

    if (in->num_entries_updated == 0) {
        return CXL_MBOX_INVALID_INPUT;
    }

    ret = detect_malformed_extent_list(ct3d, in);
    if (ret != CXL_MBOX_SUCCESS) {
        return ret;
    }

    for (i = 0; i < in->num_entries_updated; i++) {
        dpa = in->updated_entries[i].start_dpa;
        len = in->updated_entries[i].len;

        QTAILQ_FOREACH(ent, extent_list, node) {
            if (ent->start_dpa == dpa && ent->len == len) {
                break;
            } else if (ent->start_dpa < dpa
                    && dpa + len <= ent->start_dpa + ent->len) {
                /* remove partial extent */
                uint64_t len1 = dpa - ent->start_dpa;
                uint64_t len2 = ent->start_dpa + ent->len - dpa - len;

                if (len1) {
                    insert_extent_to_extent_list(extent_list, ent->start_dpa,
                            len1, NULL, 0);
                }
                if (len2) {
                    insert_extent_to_extent_list(extent_list, dpa + len, len2,
                            NULL, 0);
                }
                break;
            } else if ((dpa < ent->start_dpa + ent->len
                        && dpa + len > ent->start_dpa + ent->len)
                    || (dpa < ent->start_dpa && dpa + len > ent->start_dpa))
                return CXL_MBOX_INVALID_EXTENT_LIST;
        }

        if (ent) {
            QTAILQ_REMOVE(extent_list, ent, node);
            g_free(ent);
        } else {
            /* Try to remove a non-existing extent */
            return CXL_MBOX_INVALID_PA;
        }
    }

    return CXL_MBOX_SUCCESS;
}
 
#define IMMEDIATE_CONFIG_CHANGE (1 << 1)
#define IMMEDIATE_DATA_CHANGE (1 << 2)
#define IMMEDIATE_POLICY_CHANGE (1 << 3)
#define IMMEDIATE_LOG_CHANGE (1 << 4)
#define SECURITY_STATE_CHANGE (1 << 5)
#define BACKGROUND_OPERATION (1 << 6)

static struct cxl_cmd cxl_cmd_set[256][256] = {
    [EVENTS][GET_RECORDS] = { "EVENTS_GET_RECORDS",
        cmd_events_get_records, 1, 0 },
    [EVENTS][CLEAR_RECORDS] = { "EVENTS_CLEAR_RECORDS",
        cmd_events_clear_records, ~0, IMMEDIATE_LOG_CHANGE },
    [EVENTS][GET_INTERRUPT_POLICY] = { "EVENTS_GET_INTERRUPT_POLICY",
                                      cmd_events_get_interrupt_policy, 0, 0 },
    [EVENTS][SET_INTERRUPT_POLICY] = { "EVENTS_SET_INTERRUPT_POLICY",
                                      cmd_events_set_interrupt_policy,
                                      ~0, IMMEDIATE_CONFIG_CHANGE },
    [FIRMWARE_UPDATE][GET_INFO] = { "FIRMWARE_UPDATE_GET_INFO",
        cmd_firmware_update_get_info, 0, 0 },
    [TIMESTAMP][GET] = { "TIMESTAMP_GET", cmd_timestamp_get, 0, 0 },
    [TIMESTAMP][SET] = { "TIMESTAMP_SET", cmd_timestamp_set, 8, IMMEDIATE_POLICY_CHANGE },
    [LOGS][GET_SUPPORTED] = { "LOGS_GET_SUPPORTED", cmd_logs_get_supported, 0, 0 },
    [LOGS][GET_LOG] = { "LOGS_GET_LOG", cmd_logs_get_log, 0x18, 0 },
    [IDENTIFY][MEMORY_DEVICE] = { "IDENTIFY_MEMORY_DEVICE",
        cmd_identify_memory_device, 0, 0 },
    [CCLS][GET_PARTITION_INFO] = { "CCLS_GET_PARTITION_INFO",
        cmd_ccls_get_partition_info, 0, 0 },
    [CCLS][GET_LSA] = { "CCLS_GET_LSA", cmd_ccls_get_lsa, 8, 0 },
    [CCLS][SET_LSA] = { "CCLS_SET_LSA", cmd_ccls_set_lsa,
        ~0, IMMEDIATE_CONFIG_CHANGE | IMMEDIATE_DATA_CHANGE },
    [SANITIZE][OVERWRITE] = { "SANITIZE_OVERWRITE", cmd_sanitize_overwrite,
        0, IMMEDIATE_DATA_CHANGE | SECURITY_STATE_CHANGE | BACKGROUND_OPERATION },
    [MEDIA_AND_POISON][GET_POISON_LIST] = { "MEDIA_AND_POISON_GET_POISON_LIST",
        cmd_media_get_poison_list, 16, 0 },
    [MEDIA_AND_POISON][INJECT_POISON] = { "MEDIA_AND_POISON_INJECT_POISON",
        cmd_media_inject_poison, 8, 0 },
    [MEDIA_AND_POISON][CLEAR_POISON] = { "MEDIA_AND_POISON_CLEAR_POISON",
        cmd_media_clear_poison, 72, 0 },
    [DCD_CONFIG][GET_DC_CONFIG] = { "DCD_GET_DC_CONFIG",
        cmd_dcd_get_dyn_cap_config, 2, 0 },
    [DCD_CONFIG][GET_DYN_CAP_EXT_LIST] = {
        "DCD_GET_DYNAMIC_CAPACITY_EXTENT_LIST", cmd_dcd_get_dyn_cap_ext_list,
        8, 0 },
    [DCD_CONFIG][ADD_DYN_CAP_RSP] = {
        "ADD_DCD_DYNAMIC_CAPACITY_RESPONSE", cmd_dcd_add_dyn_cap_rsp,
        ~0, IMMEDIATE_DATA_CHANGE },
    [DCD_CONFIG][RELEASE_DYN_CAP] = {
        "RELEASE_DCD_DYNAMIC_CAPACITY", cmd_dcd_release_dyn_cap,
        ~0, IMMEDIATE_DATA_CHANGE },
};

static struct cxl_cmd cxl_cmd_set_sw[256][256] = {
    [INFOSTAT][IS_IDENTIFY] = { "IDENTIFY", cmd_infostat_identify, 0, 18 },
    [INFOSTAT][BACKGROUND_OPERATION_STATUS] = { "BACKGROUND_OPERATION_STATUS",
        cmd_infostat_bg_op_sts, 0, 8 },
    /*
     * TODO get / set response message limit - requires all messages over
     * 256 bytes to support chunking.
     */
    [TIMESTAMP][GET] = { "TIMESTAMP_GET", cmd_timestamp_get, 0, 0 },
    [TIMESTAMP][SET] = { "TIMESTAMP_SET", cmd_timestamp_set, 8, IMMEDIATE_POLICY_CHANGE },
    [LOGS][GET_SUPPORTED] = { "LOGS_GET_SUPPORTED", cmd_logs_get_supported, 0, 0 },
    [LOGS][GET_LOG] = { "LOGS_GET_LOG", cmd_logs_get_log, 0x18, 0 },
    [PHYSICAL_SWITCH][IDENTIFY_SWITCH_DEVICE] = {"IDENTIFY_SWITCH_DEVICE",
        cmd_identify_switch_device, 0, 0x49 },
};

/*
 * While the command is executing in the background, the device should
 * update the percentage complete in the Background Command Status Register
 * at least once per second.
 */
#define CXL_MBOX_BG_UPDATE_FREQ 1000UL

void cxl_process_mailbox(CXLDeviceState *cxl_dstate)
{
    uint16_t ret = CXL_MBOX_SUCCESS;
    struct cxl_cmd *cxl_cmd;
    uint64_t status_reg = 0;
    opcode_handler h;
    uint8_t bg_started = 0;
    uint64_t command_reg = cxl_dstate->mbox_reg_state64[R_CXL_DEV_MAILBOX_CMD];

    uint8_t set = FIELD_EX64(command_reg, CXL_DEV_MAILBOX_CMD, COMMAND_SET);
    uint8_t cmd = FIELD_EX64(command_reg, CXL_DEV_MAILBOX_CMD, COMMAND);
    uint16_t len = FIELD_EX64(command_reg, CXL_DEV_MAILBOX_CMD, LENGTH);
    cxl_cmd = &cxl_dstate->cxl_cmd_set[set][cmd];
    h = cxl_cmd->handler;
    if (h) {
        if (len == cxl_cmd->in || cxl_cmd->in == ~0) {
            cxl_cmd->payload = cxl_dstate->mbox_reg_state +
                A_CXL_DEV_CMD_PAYLOAD;
            /* Only one bg command at a time */
            if ((cxl_cmd->effect & BACKGROUND_OPERATION) &&
                cxl_dstate->bg.runtime > 0) {
                    ret = CXL_MBOX_BUSY;
                    goto done;
            }
            /* forbid any selected commands while overwriting */
            if (sanitize_running(cxl_dstate)) {
                if (h == cmd_events_get_records ||
                    h == cmd_ccls_get_partition_info ||
                    h == cmd_ccls_set_lsa ||
                    h == cmd_ccls_get_lsa ||
                    h == cmd_logs_get_log ||
                    h == cmd_media_get_poison_list ||
                    h == cmd_media_inject_poison ||
                    h == cmd_media_clear_poison ||
                    h == cmd_sanitize_overwrite) {
                        ret = CXL_MBOX_MEDIA_DISABLED;
                        goto done;
                }
            }
            ret = (*h)(cxl_cmd, cxl_dstate, &len);
            if ((cxl_cmd->effect & BACKGROUND_OPERATION) &&
                ret == CXL_MBOX_BG_STARTED) {
                bg_started = 1;
            }
            assert(len <= cxl_dstate->payload_size);
        } else {
            ret = CXL_MBOX_INVALID_PAYLOAD_LENGTH;
        }
    } else {
        qemu_log_mask(LOG_UNIMP, "Command %04xh not implemented\n",
                      set << 8 | cmd);
        ret = CXL_MBOX_UNSUPPORTED;
    }

done:
    /* Set bg and the return code */
    if (bg_started) {
        status_reg = FIELD_DP64(0, CXL_DEV_MAILBOX_STS, BG_OP, bg_started);
    }
    status_reg = FIELD_DP64(status_reg, CXL_DEV_MAILBOX_STS, ERRNO, ret);

    /* Set the return length */
    command_reg = FIELD_DP64(command_reg, CXL_DEV_MAILBOX_CMD, COMMAND_SET, 0);
    command_reg = FIELD_DP64(command_reg, CXL_DEV_MAILBOX_CMD, COMMAND, 0);
    command_reg = FIELD_DP64(command_reg, CXL_DEV_MAILBOX_CMD, LENGTH, len);

    cxl_dstate->mbox_reg_state64[R_CXL_DEV_MAILBOX_CMD] = command_reg;
    cxl_dstate->mbox_reg_state64[R_CXL_DEV_MAILBOX_STS] = status_reg;

    if (bg_started) {
        uint64_t bg_status_reg, now;

        cxl_dstate->bg.opcode = (set << 8) | cmd;

        bg_status_reg = FIELD_DP64(0, CXL_DEV_BG_CMD_STS, OP, cxl_dstate->bg.opcode);
        bg_status_reg = FIELD_DP64(bg_status_reg, CXL_DEV_BG_CMD_STS,
                                   PERCENTAGE_COMP, 0);
        cxl_dstate->mbox_reg_state64[R_CXL_DEV_BG_CMD_STS] = bg_status_reg;

        now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        cxl_dstate->bg.starttime = now;
        timer_mod(cxl_dstate->bg.timer, now + CXL_MBOX_BG_UPDATE_FREQ);
    }

    /* Tell the host we're done */
    ARRAY_FIELD_DP32(cxl_dstate->mbox_reg_state32, CXL_DEV_MAILBOX_CTRL,
                     DOORBELL, 0);
}

static void bg_timercb(void *opaque)
{
    CXLDeviceState *cxl_dstate = opaque;
    uint64_t bg_status_reg = 0;
    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint64_t total_time = cxl_dstate->bg.starttime + cxl_dstate->bg.runtime;

    assert(cxl_dstate->bg.runtime > 0);
    bg_status_reg = FIELD_DP64(bg_status_reg, CXL_DEV_BG_CMD_STS,
                               OP, cxl_dstate->bg.opcode);

    if (now >= total_time) { /* we are done */
        uint64_t status_reg;
        uint16_t ret = CXL_MBOX_SUCCESS;

        cxl_dstate->bg.complete_pct = 100;
        /* Clear bg */
        status_reg = FIELD_DP64(0, CXL_DEV_MAILBOX_STS, BG_OP, 0);
        cxl_dstate->mbox_reg_state64[R_CXL_DEV_MAILBOX_STS] = status_reg;

        bg_status_reg = FIELD_DP64(bg_status_reg, CXL_DEV_BG_CMD_STS, RET_CODE, ret);

        if (ret == CXL_MBOX_SUCCESS) {
            switch (cxl_dstate->bg.opcode) {
            case 0x4400: /* sanitize */
                __do_sanitization(cxl_dstate);
                cxl_dev_enable_media(cxl_dstate);
                break;
            case 0x4304: /* TODO: scan media */
                break;
            default:
                __builtin_unreachable();
                break;
            }
        }
        qemu_log("Background command %04xh finished: %s\n",
                 cxl_dstate->bg.opcode,
                 ret == CXL_MBOX_SUCCESS ? "success" : "aborted");
    } else {
        /* estimate only */
        cxl_dstate->bg.complete_pct = 100 * now / total_time;
        timer_mod(cxl_dstate->bg.timer, now + CXL_MBOX_BG_UPDATE_FREQ);
    }

    bg_status_reg = FIELD_DP64(bg_status_reg, CXL_DEV_BG_CMD_STS, PERCENTAGE_COMP,
                               cxl_dstate->bg.complete_pct);
    cxl_dstate->mbox_reg_state64[R_CXL_DEV_BG_CMD_STS] = bg_status_reg;

    if (cxl_dstate->bg.complete_pct == 100) {
        CXLType3Dev *ct3d = container_of(cxl_dstate, CXLType3Dev, cxl_dstate);
        PCIDevice *pdev = &ct3d->parent_obj;

        cxl_dstate->bg.starttime = 0;
        /* registers are updated, allow new bg-capable cmds */
        cxl_dstate->bg.runtime = 0;

        if (msix_enabled(pdev)) {
            msix_notify(pdev, cxl_dstate->mbox_msi_n);
        } else if (msi_enabled(pdev)) {
            msi_notify(pdev, cxl_dstate->mbox_msi_n);
        }
    }
}

void cxl_initialize_mailbox(CXLDeviceState *cxl_dstate, bool switch_cci)
{
    if (!switch_cci) {
        cxl_dstate->cxl_cmd_set = cxl_cmd_set;
    } else {
        cxl_dstate->cxl_cmd_set = cxl_cmd_set_sw;
    }
    for (int set = 0; set < 256; set++) {
        for (int cmd = 0; cmd < 256; cmd++) {
            if (cxl_dstate->cxl_cmd_set[set][cmd].handler) {
                struct cxl_cmd *c = &cxl_dstate->cxl_cmd_set[set][cmd];
                struct cel_log *log =
                    &cxl_dstate->cel_log[cxl_dstate->cel_size];

                log->opcode = (set << 8) | cmd;
                log->effect = c->effect;
                cxl_dstate->cel_size++;
            }
        }
    }
    cxl_dstate->bg.complete_pct = 0;
    cxl_dstate->bg.starttime = 0;
    cxl_dstate->bg.runtime = 0;
    cxl_dstate->bg.timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                        bg_timercb, cxl_dstate);
}
