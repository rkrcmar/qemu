/*
 * QEMU emulation of common X86 IOMMU
 *
 * Copyright (C) 2016 Peter Xu, Red Hat <peterx@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/i386/x86-iommu.h"

static void x86_iommu_class_init(ObjectClass *klass, void *data)
{
}

static const TypeInfo x86_iommu_info = {
    .name          = TYPE_X86_IOMMU_DEVICE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(X86IOMMUState),
    .class_init    = x86_iommu_class_init,
    .abstract      = true,
};

static void x86_iommu_register_types(void)
{
    type_register_static(&x86_iommu_info);
}

type_init(x86_iommu_register_types)
