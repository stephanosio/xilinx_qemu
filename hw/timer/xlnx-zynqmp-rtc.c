/*
 * QEMU model of the Xilinx ZynqMP Real Time Clock (RTC).
 *
 * Copyright (c) 2017 Xilinx Inc.
 *
 * Written-by: Alistair Francis <alistair.francis@xilinx.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/register.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "hw/ptimer.h"
#include "qemu/cutils.h"
#include "sysemu/sysemu.h"
#include "trace.h"
#include "hw/timer/xlnx-zynqmp-rtc.h"

#ifndef XLNX_ZYNQMP_RTC_ERR_DEBUG
#define XLNX_ZYNQMP_RTC_ERR_DEBUG 0
#endif

enum version_id {
    IP_VERSION_1_0_1 = 0,
    IP_VERSION_2_0_0 = 1
};

struct version_item_lookup {
    enum version_id id;
    const char *str;
};

static struct version_item_lookup  version_table_lookup[] = {
    { IP_VERSION_1_0_1, "1.0.1" },
    { IP_VERSION_2_0_0, "2.0.0" }
};

static Property xlnx_rtc_properties[] = {
    DEFINE_PROP_STRING("version", XlnxZynqMPRTC, cfg.version),
    DEFINE_PROP_END_OF_LIST(),
};

static void rtc_int_update_irq(XlnxZynqMPRTC *s)
{
    bool pending = s->regs[R_RTC_INT_STATUS] & ~s->regs[R_RTC_INT_MASK];
    qemu_set_irq(s->irq_rtc_int, pending);
}

static void addr_error_int_update_irq(XlnxZynqMPRTC *s)
{
    bool pending = s->regs[R_ADDR_ERROR] & ~s->regs[R_ADDR_ERROR_INT_MASK];
    qemu_set_irq(s->irq_addr_error_int, pending);
}

static uint32_t rtc_get_count(XlnxZynqMPRTC *s)
{
    int64_t now = qemu_clock_get_ns(rtc_clock);
    return s->tick_offset + now / NANOSECONDS_PER_SECOND;
}

static uint64_t current_time_postr(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);

    return rtc_get_count(s);
}

static void rtc_int_status_postw(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);
    rtc_int_update_irq(s);
}

static uint64_t rtc_int_en_prew(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);

    s->regs[R_RTC_INT_MASK] &= (uint32_t) ~val64;
    rtc_int_update_irq(s);
    return 0;
}

static uint64_t rtc_int_dis_prew(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);

    s->regs[R_RTC_INT_MASK] |= (uint32_t) val64;
    rtc_int_update_irq(s);
    return 0;
}

static void addr_error_postw(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);
    addr_error_int_update_irq(s);
}

static uint64_t addr_error_int_en_prew(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);

    s->regs[R_ADDR_ERROR_INT_MASK] &= (uint32_t) ~val64;
    addr_error_int_update_irq(s);
    return 0;
}

static uint64_t addr_error_int_dis_prew(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);

    s->regs[R_ADDR_ERROR_INT_MASK] |= (uint32_t) val64;
    addr_error_int_update_irq(s);
    return 0;
}

static void rtc_set_timer_postw(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);
    s->tick_offset = (uint32_t) val64;
}

static void rtc_calib_write_postw(RegisterInfo *reg, uint64_t val64)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(reg->opaque);
    s->regs[R_CALIB_READ] = (uint32_t) val64;
}

static const RegisterAccessInfo rtc_regs_info[] = {
    {   .name = "SET_TIME_WRITE",  .addr = A_SET_TIME_WRITE,
        .post_write = rtc_set_timer_postw,
    },{ .name = "SET_TIME_READ",  .addr = A_SET_TIME_READ,
        .ro = 0xffffffff,
        .post_read = current_time_postr,
    },{ .name = "CALIB_WRITE",  .addr = A_CALIB_WRITE,
        .post_write = rtc_calib_write_postw,
    },{ .name = "CALIB_READ",  .addr = A_CALIB_READ,
        .ro = 0x1fffff,
    },{ .name = "CURRENT_TIME",  .addr = A_CURRENT_TIME,
        .ro = 0xffffffff,
        .post_read = current_time_postr,
    },{ .name = "CURRENT_TICK",  .addr = A_CURRENT_TICK,
        .ro = 0xffff,
    },{ .name = "ALARM",  .addr = A_ALARM,
    },{ .name = "RTC_INT_STATUS",  .addr = A_RTC_INT_STATUS,
        .w1c = 0x3,
        .post_write = rtc_int_status_postw,
    },{ .name = "RTC_INT_MASK",  .addr = A_RTC_INT_MASK,
        .reset = 0x3,
        .ro = 0x3,
    },{ .name = "RTC_INT_EN",  .addr = A_RTC_INT_EN,
        .pre_write = rtc_int_en_prew,
    },{ .name = "RTC_INT_DIS",  .addr = A_RTC_INT_DIS,
        .pre_write = rtc_int_dis_prew,
    },{ .name = "ADDR_ERROR",  .addr = A_ADDR_ERROR,
        .w1c = 0x1,
        .post_write = addr_error_postw,
    },{ .name = "ADDR_ERROR_INT_MASK",  .addr = A_ADDR_ERROR_INT_MASK,
        .reset = 0x1,
        .ro = 0x1,
    },{ .name = "ADDR_ERROR_INT_EN",  .addr = A_ADDR_ERROR_INT_EN,
        .pre_write = addr_error_int_en_prew,
    },{ .name = "ADDR_ERROR_INT_DIS",  .addr = A_ADDR_ERROR_INT_DIS,
        .pre_write = addr_error_int_dis_prew,
    },{ .name = "CONTROL",  .addr = A_CONTROL,
        .reset = 0x1000000,
        .rsvd = 0x70fffffe,
    },{ .name = "SAFETY_CHK",  .addr = A_SAFETY_CHK,
    }
};

static const RegisterAccessInfo rtc_regs_control_v2_info = {
    .name = "CONTROL",   .addr = A_CONTROL,
    .reset = 0x2000000,  .rsvd = 0x70fffffe,
};

static enum version_id version_id_lookup(const char *str)
{
    uint32_t i;
    enum version_id version;

    version = IP_VERSION_1_0_1;

    if (str) {
        for (i = 0; i < ARRAY_SIZE(version_table_lookup); ++i) {
            if (!strcmp(str, version_table_lookup[i].str)) {
                version =  version_table_lookup[i].id;
                break;
            }
        }
    }

    return version;
}

static void rtc_reset(DeviceState *dev)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(dev);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(s->regs_info); ++i) {
        register_reset(&s->regs_info[i]);
    }

    if (version_id_lookup(s->cfg.version) == IP_VERSION_2_0_0) {
        s->regs_info[R_CONTROL].access = &rtc_regs_control_v2_info;
        register_reset(&s->regs_info[R_CONTROL]);
    }

    trace_xlnx_zynqmp_rtc_gettime(s->current_tm.tm_year, s->current_tm.tm_mon,
                                  s->current_tm.tm_mday, s->current_tm.tm_hour,
                                  s->current_tm.tm_min, s->current_tm.tm_sec);

    rtc_int_update_irq(s);
    addr_error_int_update_irq(s);
}

static const MemoryRegionOps rtc_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rtc_init(Object *obj)
{
    XlnxZynqMPRTC *s = XLNX_ZYNQMP_RTC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RegisterInfoArray *reg_array;

    memory_region_init(&s->iomem, obj, TYPE_XLNX_ZYNQMP_RTC,
                       XLNX_ZYNQMP_RTC_R_MAX * 4);
    reg_array =
        register_init_block32(DEVICE(obj), rtc_regs_info,
                              ARRAY_SIZE(rtc_regs_info),
                              s->regs_info, s->regs,
                              &rtc_ops,
                              XLNX_ZYNQMP_RTC_ERR_DEBUG,
                              XLNX_ZYNQMP_RTC_R_MAX * 4);
    memory_region_add_subregion(&s->iomem,
                                0x0,
                                &reg_array->mem);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_rtc_int);
    sysbus_init_irq(sbd, &s->irq_addr_error_int);

    qemu_get_timedate(&s->current_tm, 0);
    s->tick_offset = mktimegm(&s->current_tm) -
        qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
}

static int rtc_post_load(void *opaque, int version_id)
{
    XlnxZynqMPRTC *s = opaque;

    /* The tick_offset is added to the current time to determine the guest
     * time. After migration we don't want to use the original time as that
     * will indicate to the guest that time has passed, so we need to
     * recalculate the tick_offset here.
     */
    s->tick_offset = mktimegm(&s->current_tm) -
        qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;

    return 0;
}

static const VMStateDescription vmstate_rtc = {
    .name = TYPE_XLNX_ZYNQMP_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rtc_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, XlnxZynqMPRTC, XLNX_ZYNQMP_RTC_R_MAX),
        VMSTATE_INT32(current_tm.tm_sec, XlnxZynqMPRTC),
        VMSTATE_INT32(current_tm.tm_min, XlnxZynqMPRTC),
        VMSTATE_INT32(current_tm.tm_hour, XlnxZynqMPRTC),
        VMSTATE_INT32(current_tm.tm_wday, XlnxZynqMPRTC),
        VMSTATE_INT32(current_tm.tm_mday, XlnxZynqMPRTC),
        VMSTATE_INT32(current_tm.tm_mon, XlnxZynqMPRTC),
        VMSTATE_INT32(current_tm.tm_year, XlnxZynqMPRTC),
        VMSTATE_END_OF_LIST(),
    }
};

static void rtc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = rtc_reset;
    dc->props = xlnx_rtc_properties;
    dc->vmsd = &vmstate_rtc;
}

static const TypeInfo rtc_info = {
    .name          = TYPE_XLNX_ZYNQMP_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XlnxZynqMPRTC),
    .class_init    = rtc_class_init,
    .instance_init = rtc_init,
};

static const TypeInfo rtc_alias_info = {
    .name           = TYPE_XLNX_ZYNQMP_ALIAS_RTC,
    .parent         = TYPE_XLNX_ZYNQMP_RTC,
};

static void rtc_register_types(void)
{
    type_register_static(&rtc_info);
    type_register_static(&rtc_alias_info);
}

type_init(rtc_register_types)
