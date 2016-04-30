#include "qemu/osdep.h"
#include "hw/boards.h"
#include "hw/acpi/cpu.h"
#include "qapi/error.h"
#include "trace.h"

#define ACPI_CPU_HOTPLUG_REG_LEN 12
#define ACPI_CPU_SELECTOR_OFFSET_WR 0
#define ACPI_CPU_FLAGS_OFFSET_RW 4

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
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(devs, CPUHotplugState, dev_count,
                                             vmstate_cpuhp_sts, AcpiCpuStatus),
        VMSTATE_END_OF_LIST()
    }
};

#define CPU_NAME_FMT      "C%.03X"

void build_cpus_aml(Aml *table, MachineState *machine, bool acpi1_compat)
{
    Aml *cpus_dev;
    Aml *sb_scope = aml_scope("_SB");
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    CPUArchIdList *arch_ids = mc->possible_cpu_arch_ids(machine);
    cpus_dev = aml_device("\\_SB.CPUS");
    {
        int i;

        aml_append(cpus_dev, aml_name_decl("_HID", aml_string("ACPI0010")));
        aml_append(cpus_dev, aml_name_decl("_CID", aml_eisaid("PNP0A05")));

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

            aml_append(cpus_dev, dev);
        }
    }
    aml_append(sb_scope, cpus_dev);
    aml_append(table, sb_scope);

    g_free(arch_ids);
}
