#include "qemu/osdep.h"
#include "hw/boards.h"
#include "hw/acpi/cpu.h"
#include "qapi/error.h"
#include "trace.h"

#define ACPI_CPU_HOTPLUG_REG_LEN 12
#define ACPI_CPU_SELECTOR_OFFSET_WR 0
#define ACPI_CPU_FLAGS_OFFSET_RW 4
#define ACPI_CPU_CMD_OFFSET_WR 5
#define ACPI_CPU_CMD_DATA_OFFSET_RW 8

enum {
    CPHP_CMD_MAX
};

static uint64_t cpu_hotplug_rd(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = ~0;
    CPUHotplugState *cpu_st = opaque;
    AcpiCpuStatus *cdev;

    if (cpu_st->selector >= cpu_st->dev_count) {
        return val;
    }

    cdev = &cpu_st->devs[cpu_st->selector];
    switch (addr) {
    case ACPI_CPU_FLAGS_OFFSET_RW: /* pack and return is_* fields */
        val = 0;
        val |= cdev->is_enabled   ? 1 : 0;
        val |= cdev->is_inserting ? 2 : 0;
        val |= cdev->is_removing  ? 4 : 0;
        trace_cpuhp_acpi_read_flags(cpu_st->selector, val);
        break;
    case ACPI_CPU_CMD_DATA_OFFSET_RW:
        switch (cpu_st->command) {
        default:
           break;
        }
        break;
    default:
        break;
    }
    return val;
}

static void cpu_hotplug_wr(void *opaque, hwaddr addr, uint64_t data,
                           unsigned int size)
{
    CPUHotplugState *cpu_st = opaque;
    AcpiCpuStatus *cdev;
    Error *local_err = NULL;

    assert(cpu_st->dev_count);

    if (addr) {
        if (cpu_st->selector >= cpu_st->dev_count) {
            trace_cpuhp_acpi_invalid_idx_selected(cpu_st->selector);
            return;
        }
    }

    switch (addr) {
    case ACPI_CPU_SELECTOR_OFFSET_WR: /* current CPU selector */
        cpu_st->selector = data;
        trace_cpuhp_acpi_write_idx(cpu_st->selector);
        break;
    case ACPI_CPU_FLAGS_OFFSET_RW: /* set is_* fields  */
        cdev = &cpu_st->devs[cpu_st->selector];
        if (data & 2) { /* clear insert event */
            cdev->is_inserting = false;
            trace_cpuhp_acpi_clear_inserting_evt(cpu_st->selector);
        } else if (data & 4) { /* clear remove event */
            cdev->is_removing = false;
            trace_cpuhp_acpi_clear_remove_evt(cpu_st->selector);
        } else if (data & 8) {
            DeviceState *dev = NULL;
            HotplugHandler *hotplug_ctrl = NULL;

            if (!cdev->is_enabled) {
                trace_cpuhp_acpi_ejecting_invalid_cpu(cpu_st->selector);
                break;
            }

            trace_cpuhp_acpi_ejecting_cpu(cpu_st->selector);
            dev = DEVICE(cdev->cpu);
            hotplug_ctrl = qdev_get_hotplug_handler(dev);
            hotplug_handler_unplug(hotplug_ctrl, dev, &local_err);
            if (local_err) {
                break;
            }
        }
        break;
    case ACPI_CPU_CMD_OFFSET_WR:
        trace_cpuhp_acpi_write_cmd(cpu_st->selector, data);
        if (data < CPHP_CMD_MAX) {
            cpu_st->command = data;
        }
        break;
    default:
        break;
    }
    error_free(local_err);
}

static const MemoryRegionOps cpu_hotplug_ops = {
    .read = cpu_hotplug_rd,
    .write = cpu_hotplug_wr,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

void cpu_hotplug_hw_init(MemoryRegion *as, Object *owner,
                         CPUHotplugState *state, hwaddr base_addr)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    CPUArchIdList *id_list;
    int i;

    id_list = mc->possible_cpu_arch_ids(machine);
    state->dev_count = id_list->len;
    state->devs = g_new0(typeof(*state->devs), state->dev_count);
    for (i = 0; i < id_list->len; i++) {
        state->devs[i].cpu =  id_list->cpus[i].cpu;
        state->devs[i].arch_id = id_list->cpus[i].arch_id;
        state->devs[i].is_enabled =  id_list->cpus[i].cpu ? true : false;
    }
    g_free(id_list);
    memory_region_init_io(&state->ctrl_reg, owner, &cpu_hotplug_ops, state,
                          "acpi-mem-hotplug", ACPI_CPU_HOTPLUG_REG_LEN);
    memory_region_add_subregion(as, base_addr, &state->ctrl_reg);
}

static AcpiCpuStatus *get_cpu_status(CPUHotplugState *cpu_st, DeviceState *dev)
{
    CPUClass *k = CPU_GET_CLASS(dev);
    uint64_t cpu_arch_id = k->get_arch_id(CPU(dev));
    int i;

    for (i = 0; i < cpu_st->dev_count; i++) {
        if (cpu_arch_id == cpu_st->devs[i].arch_id) {
            return &cpu_st->devs[i];
        }
    }
    return NULL;
}

void acpi_cpu_plug_cb(HotplugHandler *hotplug_dev,
                      CPUHotplugState *cpu_st, DeviceState *dev, Error **errp)
{
    AcpiCpuStatus *cdev;

    if (!cpu_st->is_enabled) {
        error_setg(errp, "acpi: CPU hotplug is not enabled on: %s",
                   object_get_typename(OBJECT(hotplug_dev)));
        return;
    }

    cdev = get_cpu_status(cpu_st, dev);
    if (!cdev) {
        return;
    }

    cdev->cpu = CPU(dev);
    cdev->is_enabled = true;
    if (dev->hotplugged) {
        cdev->is_inserting = true;
        ACPI_SEND_EVENT(hotplug_dev, ACPI_CPU_HOTPLUG_STATUS);
    }
}

void acpi_cpu_unplug_request_cb(HotplugHandler *hotplug_dev,
                                CPUHotplugState *cpu_st,
                                DeviceState *dev, Error **errp)
{
    AcpiCpuStatus *cdev;

    if (!cpu_st->is_enabled) {
        error_setg(errp, "acpi: CPU hotplug is not enabled on: %s",
                   object_get_typename(OBJECT(hotplug_dev)));
        return;
    }

    cdev = get_cpu_status(cpu_st, dev);
    if (!cdev) {
        return;
    }

    cdev->is_removing = true;
    ACPI_SEND_EVENT(hotplug_dev, ACPI_CPU_HOTPLUG_STATUS);
}

void acpi_cpu_unplug_cb(CPUHotplugState *cpu_st,
                        DeviceState *dev, Error **errp)
{
    AcpiCpuStatus *cdev;

    cdev = get_cpu_status(cpu_st, dev);
    if (!cdev) {
        return;
    }

    cdev->cpu = NULL;
    cdev->is_enabled = false;
}

static const VMStateDescription vmstate_cpuhp_sts = {
    .name = "CPU hotplug device state",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_BOOL(is_enabled, AcpiCpuStatus),
        VMSTATE_BOOL(is_inserting, AcpiCpuStatus),
        VMSTATE_BOOL(is_removing, AcpiCpuStatus),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_cpu_hotplug = {
    .name = "CPU hotplug state",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_UINT32(selector, CPUHotplugState),
        VMSTATE_UINT32(command, CPUHotplugState),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(devs, CPUHotplugState, dev_count,
                                             vmstate_cpuhp_sts, AcpiCpuStatus),
        VMSTATE_END_OF_LIST()
    }
};

#define CPU_NAME_FMT      "C%.03X"
#define CPUHP_RES_DEVICE  "PRES"
#define CPU_LOCK          "CPLK"
#define CPU_STS_METHOD    "CSTA"
#define CPU_SCAN_METHOD   "CSCN"
#define CPU_EJECT_METHOD  "CEJ0"
#define CPU_NOTIFY_METHOD "CTFY"

#define CPU_ENABLED       "CPEN"
#define CPU_SELECTOR      "CSEL"
#define CPU_EJECT_EVENT   "CEJ0"
#define CPU_INSERT_EVENT  "CINS"
#define CPU_REMOVE_EVENT  "CRMV"
#define CPU_COMMAND       "CCMD"
#define CPU_DATA          "CDAT"

void build_cpus_aml(Aml *table, MachineState *machine, bool acpi1_compat,
                    const char *res_root, const char *event_handler_method,
                    hwaddr io_base)
{
    Aml *ifctx;
    Aml *field;
    Aml *method;
    Aml *cpu_ctrl_dev;
    Aml *cpus_dev;
    Aml *zero = aml_int(0);
    Aml *sb_scope = aml_scope("_SB");
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    CPUArchIdList *arch_ids = mc->possible_cpu_arch_ids(machine);
    char *cphp_res_path = g_strdup_printf("%s." CPUHP_RES_DEVICE, res_root);
    Object *obj = object_resolve_path_type("", TYPE_ACPI_DEVICE_IF, NULL);
    AcpiDeviceIfClass *adevc = ACPI_DEVICE_IF_GET_CLASS(obj);
    AcpiDeviceIf *adev = ACPI_DEVICE_IF(obj);

    if (machine->cpu_hotplug) {
        cpu_ctrl_dev = aml_device("%s", cphp_res_path);
        {
            Aml *crs;

            aml_append(cpu_ctrl_dev,
                aml_name_decl("_HID", aml_eisaid("PNP0A06")));
            aml_append(cpu_ctrl_dev,
                aml_name_decl("_UID", aml_string("CPU Hotplug resources")));
            aml_append(cpu_ctrl_dev, aml_mutex(CPU_LOCK, 0));

            crs = aml_resource_template();
            aml_append(crs, aml_io(AML_DECODE16, io_base, io_base, 1,
                                   ACPI_CPU_HOTPLUG_REG_LEN));
            aml_append(cpu_ctrl_dev, aml_name_decl("_CRS", crs));

            /* declare CPU hotplug MMIO region with related access fields */
            aml_append(cpu_ctrl_dev,
                aml_operation_region("PRST", AML_SYSTEM_IO, aml_int(io_base),
                                     ACPI_CPU_HOTPLUG_REG_LEN));

            field = aml_field("PRST", AML_BYTE_ACC, AML_NOLOCK,
                              AML_WRITE_AS_ZEROS);
            aml_append(field, aml_reserved_field(ACPI_CPU_FLAGS_OFFSET_RW * 8));
            /* 1 if enabled, read only */
            aml_append(field, aml_named_field(CPU_ENABLED, 1));
            /* (read) 1 if has a insert event. (write) 1 to clear event */
            aml_append(field, aml_named_field(CPU_INSERT_EVENT, 1));
            /* (read) 1 if has a remove event. (write) 1 to clear event */
            aml_append(field, aml_named_field(CPU_REMOVE_EVENT, 1));
            /* initiates device eject, write only */
            aml_append(field, aml_named_field(CPU_EJECT_EVENT, 1));
            aml_append(field, aml_reserved_field(4));
            aml_append(field, aml_named_field(CPU_COMMAND, 8));
            aml_append(cpu_ctrl_dev, field);

            field = aml_field("PRST", AML_DWORD_ACC, AML_NOLOCK, AML_PRESERVE);
            /* CPU selector, write only */
            aml_append(field, aml_named_field(CPU_SELECTOR, 32));
            /* flags + cmd + 2byte align */
            aml_append(field, aml_reserved_field(4 * 8));
            aml_append(field, aml_named_field(CPU_DATA, 32));
            aml_append(cpu_ctrl_dev, field);

        }
        aml_append(sb_scope, cpu_ctrl_dev);
    }

    cpus_dev = aml_device("\\_SB.CPUS");
    {
        int i;
        Aml *one = aml_int(1);
        Aml *cpu_selector = aml_name("%s.%s", cphp_res_path, CPU_SELECTOR);
        Aml *ins_evt = aml_name("%s.%s", cphp_res_path, CPU_INSERT_EVENT);
        Aml *rm_evt = aml_name("%s.%s", cphp_res_path, CPU_REMOVE_EVENT);
        Aml *ej_evt = aml_name("%s.%s", cphp_res_path, CPU_EJECT_EVENT);
        Aml *is_enabled = aml_name("%s.%s", cphp_res_path, CPU_ENABLED);
        Aml *ctrl_lock = aml_name("%s.%s", cphp_res_path, CPU_LOCK);

        aml_append(cpus_dev, aml_name_decl("_HID", aml_string("ACPI0010")));
        aml_append(cpus_dev, aml_name_decl("_CID", aml_eisaid("PNP0A05")));

        if (machine->cpu_hotplug) {
            method = aml_method(CPU_NOTIFY_METHOD, 2, AML_NOTSERIALIZED);
            for (i = 0; i < arch_ids->len; i++) {
                Aml *cpu = aml_name(CPU_NAME_FMT, i);
                Aml *uid = aml_arg(0);
                Aml *event = aml_arg(1);

                ifctx = aml_if(aml_equal(uid, aml_int(i)));
                {
                    aml_append(ifctx, aml_notify(cpu, event));
                }
                aml_append(method, ifctx);
            }
            aml_append(cpus_dev, method);

            method = aml_method(CPU_STS_METHOD, 1, AML_SERIALIZED);
            {
                Aml *idx = aml_arg(0);
                Aml *sta = aml_local(0);

                aml_append(method, aml_acquire(ctrl_lock, 0xFFFF));
                aml_append(method, aml_store(idx, cpu_selector));
                aml_append(method, aml_store(zero, sta));
                ifctx = aml_if(aml_equal(is_enabled, one));
                {
                    aml_append(ifctx, aml_store(aml_int(0xF), sta));
                }
                aml_append(method, ifctx);
                aml_append(method, aml_release(ctrl_lock));
                aml_append(method, aml_return(sta));
            }
            aml_append(cpus_dev, method);

            method = aml_method(CPU_EJECT_METHOD, 1, AML_SERIALIZED);
            {
                Aml *idx = aml_arg(0);

                aml_append(method, aml_acquire(ctrl_lock, 0xFFFF));
                aml_append(method, aml_store(idx, cpu_selector));
                aml_append(method, aml_store(one, ej_evt));
                aml_append(method, aml_release(ctrl_lock));
            }
            aml_append(cpus_dev, method);

            method = aml_method(CPU_SCAN_METHOD, 0, AML_SERIALIZED);
            {
                Aml *else_ctx;
                Aml *while_ctx;
                Aml *idx = aml_local(0);
                Aml *eject_req = aml_int(3);
                Aml *dev_chk = aml_int(1);
                Aml *cpus_count = aml_int(arch_ids->len);

                aml_append(method, aml_acquire(ctrl_lock, 0xFFFF));
                aml_append(method, aml_store(zero, idx));
                while_ctx = aml_while(aml_lless(idx, cpus_count));
                {
                     aml_append(while_ctx, aml_store(idx, cpu_selector));
                     ifctx = aml_if(aml_equal(ins_evt, one));
                     {
                         aml_append(ifctx,
                             aml_call2(CPU_NOTIFY_METHOD, idx, dev_chk));
                         aml_append(ifctx, aml_store(one, ins_evt));
                     }
                     aml_append(while_ctx, ifctx);
                     else_ctx = aml_else();
                     ifctx = aml_if(aml_equal(rm_evt, one));
                     {
                         aml_append(ifctx,
                             aml_call2(CPU_NOTIFY_METHOD, idx, eject_req));
                         aml_append(ifctx, aml_store(one, rm_evt));
                     }
                     aml_append(else_ctx, ifctx);
                     aml_append(while_ctx, else_ctx);

                     aml_append(while_ctx, aml_add(idx, one, idx));
                }
                aml_append(method, while_ctx);
                aml_append(method, aml_release(ctrl_lock));
            }
            aml_append(cpus_dev, method);
        }

        /* build Processor object for each processor */
        for (i = 0; i < arch_ids->len; i++) {
            Aml *dev;
            Aml *uid = aml_int(i);
            int arch_id = arch_ids->cpus[i].arch_id;

            if (acpi1_compat && arch_id < 255) {
                dev = aml_processor(i, 0, 0, CPU_NAME_FMT, i);
            } else {
                dev = aml_device(CPU_NAME_FMT, arch_id);
                aml_append(dev, aml_name_decl("_HID", aml_string("ACPI0007")));
                aml_append(dev, aml_name_decl("_UID", uid));
            }

            if (machine->cpu_hotplug) {
                method = aml_method("_STA", 0, AML_SERIALIZED);
                aml_append(method, aml_return(aml_call1(CPU_STS_METHOD, uid)));
                aml_append(dev, method);

                method = aml_method("_MAT", 0, AML_SERIALIZED);
                {
                    Aml *madt = aml_local(1);
                    GArray *buf = g_array_new(0, 1, 1);

                    adevc->madt_cpu(adev, i, arch_ids, buf);
                    switch (buf->data[0]) {
                    case ACPI_APIC_PROCESSOR: {
                        AcpiMadtProcessorApic *apic = (void *)buf->data;
                        apic->flags = cpu_to_le32(1);
                        break;
                    }
                    default:
                        assert(0);
                    }
                    aml_append(method,
                        aml_store(aml_buffer(buf->len, (uint8_t *)buf->data),
                                  madt));
                    aml_append(method, aml_return(madt));
                    g_array_free(buf, true);
                }

                aml_append(dev, method);

                method = aml_method("_EJ0", 1, AML_NOTSERIALIZED);
                aml_append(method, aml_call1(CPU_EJECT_METHOD, uid));
                aml_append(dev, method);
            }
            aml_append(cpus_dev, dev);
        }
    }
    aml_append(sb_scope, cpus_dev);
    aml_append(table, sb_scope);

    if (machine->cpu_hotplug) {
        method = aml_method(event_handler_method, 0, AML_NOTSERIALIZED);
        aml_append(method, aml_call0("\\_SB.CPUS." CPU_SCAN_METHOD));
        aml_append(table, method);
    }

    g_free(cphp_res_path);
    g_free(arch_ids);
}
