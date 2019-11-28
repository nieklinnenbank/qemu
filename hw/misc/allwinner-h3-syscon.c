/*
 * Allwinner H3 System Control emulation
 *
 * Copyright (C) 2019 Niek Linnenbank <nieklinnenbank@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/allwinner-h3-syscon.h"

/* SYSCON register offsets */
enum {
    REG_VER               = 0x24,  /* Version */
    REG_EMAC_PHY_CLK      = 0x30,  /* EMAC PHY Clock */
};

#define REG_INDEX(offset)   (offset / sizeof(uint32_t))

/* SYSCON register reset values */
enum {
    REG_VER_RST           = 0x0,
    REG_EMAC_PHY_CLK_RST  = 0x58000,
};

static uint64_t allwinner_h3_syscon_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    const AwH3SysconState *s = (AwH3SysconState *)opaque;
    const uint32_t idx = REG_INDEX(offset);

    if (idx >= AW_H3_SYSCON_REGS_NUM) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return 0;
    }

    return s->regs[idx];
}

static void allwinner_h3_syscon_write(void *opaque, hwaddr offset,
                                      uint64_t val, unsigned size)
{
    AwH3SysconState *s = (AwH3SysconState *)opaque;
    const uint32_t idx = REG_INDEX(offset);

    if (idx >= AW_H3_SYSCON_REGS_NUM) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return;
    }

    switch (offset) {
    case REG_VER:       /* Version */
        break;
    default:
        s->regs[idx] = (uint32_t) val;
        break;
    }
}

static const MemoryRegionOps allwinner_h3_syscon_ops = {
    .read = allwinner_h3_syscon_read,
    .write = allwinner_h3_syscon_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false
    },
    .impl.min_access_size = 4,
};

static void allwinner_h3_syscon_reset(DeviceState *dev)
{
    AwH3SysconState *s = AW_H3_SYSCON(dev);

    /* Set default values for registers */
    s->regs[REG_INDEX(REG_VER)] = REG_VER_RST;
    s->regs[REG_INDEX(REG_EMAC_PHY_CLK)] = REG_EMAC_PHY_CLK_RST;
}

static void allwinner_h3_syscon_realize(DeviceState *dev, Error **errp)
{
}

static void allwinner_h3_syscon_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    AwH3SysconState *s = AW_H3_SYSCON(obj);

    /* Memory mapping */
    memory_region_init_io(&s->iomem, OBJECT(s), &allwinner_h3_syscon_ops, s,
                          TYPE_AW_H3_SYSCON, 4 * KiB);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription allwinner_h3_syscon_vmstate = {
    .name = "allwinner-h3-syscon",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AwH3SysconState, AW_H3_SYSCON_REGS_NUM),
        VMSTATE_END_OF_LIST()
    }
};

static void allwinner_h3_syscon_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = allwinner_h3_syscon_reset;
    dc->realize = allwinner_h3_syscon_realize;
    dc->vmsd = &allwinner_h3_syscon_vmstate;
}

static const TypeInfo allwinner_h3_syscon_info = {
    .name          = TYPE_AW_H3_SYSCON,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = allwinner_h3_syscon_init,
    .instance_size = sizeof(AwH3SysconState),
    .class_init    = allwinner_h3_syscon_class_init,
};

static void allwinner_h3_syscon_register(void)
{
    type_register_static(&allwinner_h3_syscon_info);
}

type_init(allwinner_h3_syscon_register)
