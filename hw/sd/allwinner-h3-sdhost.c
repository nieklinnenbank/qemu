/*
 * Allwinner H3 SD Host Controller emulation
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
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/blockdev.h"
#include "hw/irq.h"
#include "hw/sd/allwinner-h3-sdhost.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_AW_H3_SDHOST_BUS "allwinner-h3-sdhost-bus"
#define AW_H3_SDHOST_BUS(obj) \
    OBJECT_CHECK(SDBus, (obj), TYPE_AW_H3_SDHOST_BUS)

/* SD Host register offsets */
#define REG_SD_GCTL        (0x00)  /* Global Control */
#define REG_SD_CKCR        (0x04)  /* Clock Control */
#define REG_SD_TMOR        (0x08)  /* Timeout */
#define REG_SD_BWDR        (0x0C)  /* Bus Width */
#define REG_SD_BKSR        (0x10)  /* Block Size */
#define REG_SD_BYCR        (0x14)  /* Byte Count */
#define REG_SD_CMDR        (0x18)  /* Command */
#define REG_SD_CAGR        (0x1C)  /* Command Argument */
#define REG_SD_RESP0       (0x20)  /* Response Zero */
#define REG_SD_RESP1       (0x24)  /* Response One */
#define REG_SD_RESP2       (0x28)  /* Response Two */
#define REG_SD_RESP3       (0x2C)  /* Response Three */
#define REG_SD_IMKR        (0x30)  /* Interrupt Mask */
#define REG_SD_MISR        (0x34)  /* Masked Interrupt Status */
#define REG_SD_RISR        (0x38)  /* Raw Interrupt Status */
#define REG_SD_STAR        (0x3C)  /* Status */
#define REG_SD_FWLR        (0x40)  /* FIFO Water Level */
#define REG_SD_FUNS        (0x44)  /* FIFO Function Select */
#define REG_SD_DBGC        (0x50)  /* Debug Enable */
#define REG_SD_A12A        (0x58)  /* Auto command 12 argument */
#define REG_SD_NTSR        (0x5C)  /* SD NewTiming Set */
#define REG_SD_SDBG        (0x60)  /* SD newTiming Set Debug */
#define REG_SD_HWRST       (0x78)  /* Hardware Reset Register */
#define REG_SD_DMAC        (0x80)  /* Internal DMA Controller Control */
#define REG_SD_DLBA        (0x84)  /* Descriptor List Base Address */
#define REG_SD_IDST        (0x88)  /* Internal DMA Controller Status */
#define REG_SD_IDIE        (0x8C)  /* Internal DMA Controller IRQ Enable */
#define REG_SD_THLDC       (0x100) /* Card Threshold Control */
#define REG_SD_DSBD        (0x10C) /* eMMC DDR Start Bit Detection Control */
#define REG_SD_RES_CRC     (0x110) /* Response CRC from card/eMMC */
#define REG_SD_DATA7_CRC   (0x114) /* CRC Data 7 from card/eMMC */
#define REG_SD_DATA6_CRC   (0x118) /* CRC Data 6 from card/eMMC */
#define REG_SD_DATA5_CRC   (0x11C) /* CRC Data 5 from card/eMMC */
#define REG_SD_DATA4_CRC   (0x120) /* CRC Data 4 from card/eMMC */
#define REG_SD_DATA3_CRC   (0x124) /* CRC Data 3 from card/eMMC */
#define REG_SD_DATA2_CRC   (0x128) /* CRC Data 2 from card/eMMC */
#define REG_SD_DATA1_CRC   (0x12C) /* CRC Data 1 from card/eMMC */
#define REG_SD_DATA0_CRC   (0x130) /* CRC Data 0 from card/eMMC */
#define REG_SD_CRC_STA     (0x134) /* CRC status from card/eMMC during write */
#define REG_SD_FIFO        (0x200) /* Read/Write FIFO */

/* SD Host register flags */
#define SD_GCTL_FIFO_AC_MOD     (1 << 31)
#define SD_GCTL_DDR_MOD_SEL     (1 << 10)
#define SD_GCTL_CD_DBC_ENB      (1 << 8)
#define SD_GCTL_DMA_ENB         (1 << 5)
#define SD_GCTL_INT_ENB         (1 << 4)
#define SD_GCTL_DMA_RST         (1 << 2)
#define SD_GCTL_FIFO_RST        (1 << 1)
#define SD_GCTL_SOFT_RST        (1 << 0)

#define SD_CMDR_LOAD            (1 << 31)
#define SD_CMDR_CLKCHANGE       (1 << 21)
#define SD_CMDR_WRITE           (1 << 10)
#define SD_CMDR_AUTOSTOP        (1 << 12)
#define SD_CMDR_DATA            (1 << 9)
#define SD_CMDR_RESPONSE_LONG   (1 << 7)
#define SD_CMDR_RESPONSE        (1 << 6)
#define SD_CMDR_CMDID_MASK      (0x3f)

#define SD_RISR_CARD_REMOVE     (1 << 31)
#define SD_RISR_CARD_INSERT     (1 << 30)
#define SD_RISR_AUTOCMD_DONE    (1 << 14)
#define SD_RISR_DATA_COMPLETE   (1 << 3)
#define SD_RISR_CMD_COMPLETE    (1 << 2)
#define SD_RISR_NO_RESPONSE     (1 << 1)

#define SD_STAR_CARD_PRESENT    (1 << 8)

#define SD_IDST_SUM_RECEIVE_IRQ (1 << 8)
#define SD_IDST_RECEIVE_IRQ     (1 << 1)
#define SD_IDST_TRANSMIT_IRQ    (1 << 0)
#define SD_IDST_IRQ_MASK        (SD_IDST_RECEIVE_IRQ | SD_IDST_TRANSMIT_IRQ | \
                                 SD_IDST_SUM_RECEIVE_IRQ)
#define SD_IDST_WR_MASK         (0x3ff)

/* SD Host register reset values */
#define REG_SD_GCTL_RST         (0x00000300)
#define REG_SD_CKCR_RST         (0x0)
#define REG_SD_TMOR_RST         (0xFFFFFF40)
#define REG_SD_BWDR_RST         (0x0)
#define REG_SD_BKSR_RST         (0x00000200)
#define REG_SD_BYCR_RST         (0x00000200)
#define REG_SD_CMDR_RST         (0x0)
#define REG_SD_CAGR_RST         (0x0)
#define REG_SD_RESP_RST         (0x0)
#define REG_SD_IMKR_RST         (0x0)
#define REG_SD_MISR_RST         (0x0)
#define REG_SD_RISR_RST         (0x0)
#define REG_SD_STAR_RST         (0x00000100)
#define REG_SD_FWLR_RST         (0x000F0000)
#define REG_SD_FUNS_RST         (0x0)
#define REG_SD_DBGC_RST         (0x0)
#define REG_SD_A12A_RST         (0x0000FFFF)
#define REG_SD_NTSR_RST         (0x00000001)
#define REG_SD_SDBG_RST         (0x0)
#define REG_SD_HWRST_RST        (0x00000001)
#define REG_SD_DMAC_RST         (0x0)
#define REG_SD_DLBA_RST         (0x0)
#define REG_SD_IDST_RST         (0x0)
#define REG_SD_IDIE_RST         (0x0)
#define REG_SD_THLDC_RST        (0x0)
#define REG_SD_DSBD_RST         (0x0)
#define REG_SD_RES_CRC_RST      (0x0)
#define REG_SD_DATA_CRC_RST     (0x0)
#define REG_SD_CRC_STA_RST      (0x0)
#define REG_SD_FIFO_RST         (0x0)

/* Data transfer descriptor for DMA */
typedef struct TransferDescriptor {
    uint32_t status; /* Status flags */
    uint32_t size;   /* Data buffer size */
    uint32_t addr;   /* Data buffer address */
    uint32_t next;   /* Physical address of next descriptor */
} TransferDescriptor;

/* Data transfer descriptor flags */
#define DESC_STATUS_HOLD   (1 << 31) /* Set when descriptor is in use by DMA */
#define DESC_STATUS_ERROR  (1 << 30) /* Set when DMA transfer error occurred */
#define DESC_STATUS_CHAIN  (1 << 4)  /* Indicates chained descriptor. */
#define DESC_STATUS_FIRST  (1 << 3)  /* Set on the first descriptor */
#define DESC_STATUS_LAST   (1 << 2)  /* Set on the last descriptor */
#define DESC_STATUS_NOIRQ  (1 << 1)  /* Skip raising interrupt after transfer */

#define DESC_SIZE_MASK     (0xfffffffc)

static void aw_h3_sdhost_update_irq(AwH3SDHostState *s)
{
    uint32_t irq_en = s->global_ctl & SD_GCTL_INT_ENB;
    uint32_t irq = irq_en ? s->irq_status & s->irq_mask : 0;

    trace_aw_h3_sdhost_update_irq(irq);
    qemu_set_irq(s->irq, irq);
}

static void aw_h3_sdhost_update_transfer_cnt(AwH3SDHostState *s, uint32_t bytes)
{
    if (s->transfer_cnt > bytes) {
        s->transfer_cnt -= bytes;
    } else {
        s->transfer_cnt = 0;
    }

    if (!s->transfer_cnt) {
        s->irq_status |= SD_RISR_DATA_COMPLETE | SD_RISR_AUTOCMD_DONE;
    }
}

static void aw_h3_sdhost_set_inserted(DeviceState *dev, bool inserted)
{
    AwH3SDHostState *s = AW_H3_SDHOST(dev);

    trace_aw_h3_sdhost_set_inserted(inserted);

    if (inserted) {
        s->irq_status |= SD_RISR_CARD_INSERT;
        s->irq_status &= ~SD_RISR_CARD_REMOVE;
        s->status |= SD_STAR_CARD_PRESENT;
    } else {
        s->irq_status &= ~SD_RISR_CARD_INSERT;
        s->irq_status |= SD_RISR_CARD_REMOVE;
        s->status &= ~SD_STAR_CARD_PRESENT;
    }

    aw_h3_sdhost_update_irq(s);
}

static void aw_h3_sdhost_send_command(AwH3SDHostState *s)
{
    SDRequest request;
    uint8_t resp[16];
    int rlen;

    /* Auto clear load flag */
    s->command &= ~SD_CMDR_LOAD;

    /* Clock change does not actually interact with the SD bus */
    if (!(s->command & SD_CMDR_CLKCHANGE)) {

        /* Prepare request */
        request.cmd = s->command & SD_CMDR_CMDID_MASK;
        request.arg = s->command_arg;

        /* Send request to SD bus */
        rlen = sdbus_do_command(&s->sdbus, &request, resp);
        if (rlen < 0) {
            goto error;
        }

        /* If the command has a response, store it in the response registers */
        if ((s->command & SD_CMDR_RESPONSE)) {
            if (rlen == 0 ||
               (rlen == 4 && (s->command & SD_CMDR_RESPONSE_LONG))) {
                goto error;
            }
            if (rlen != 4 && rlen != 16) {
                goto error;
            }
            if (rlen == 4) {
                s->response[0] = ldl_be_p(&resp[0]);
                s->response[1] = s->response[2] = s->response[3] = 0;
            } else {
                s->response[0] = ldl_be_p(&resp[12]);
                s->response[1] = ldl_be_p(&resp[8]);
                s->response[2] = ldl_be_p(&resp[4]);
                s->response[3] = ldl_be_p(&resp[0]);
            }
        }
    }

    /* Set interrupt status bits */
    s->irq_status |= SD_RISR_CMD_COMPLETE;
    return;

error:
    s->irq_status |= SD_RISR_NO_RESPONSE;
}

static void aw_h3_sdhost_auto_stop(AwH3SDHostState *s)
{
    /*
     * The stop command (CMD12) ensures the SD bus
     * returns to the transfer state.
     */
    if ((s->command & SD_CMDR_AUTOSTOP) && (s->transfer_cnt == 0)) {
        /* First save current command registers */
        uint32_t saved_cmd = s->command;
        uint32_t saved_arg = s->command_arg;

        /* Prepare stop command (CMD12) */
        s->command &= ~SD_CMDR_CMDID_MASK;
        s->command |= 12; /* CMD12 */
        s->command_arg = 0;

        /* Put the command on SD bus */
        aw_h3_sdhost_send_command(s);

        /* Restore command values */
        s->command = saved_cmd;
        s->command_arg = saved_arg;
    }
}

static uint32_t aw_h3_sdhost_process_desc(AwH3SDHostState *s,
                                          hwaddr desc_addr,
                                          TransferDescriptor *desc,
                                          bool is_write, uint32_t max_bytes)
{
    uint32_t num_done = 0;
    uint32_t num_bytes = max_bytes;
    uint8_t buf[1024];

    /* Read descriptor */
    cpu_physical_memory_read(desc_addr, desc, sizeof(*desc));
    if (desc->size == 0) {
        desc->size = 0xffff + 1;
    }
    if (desc->size < num_bytes) {
        num_bytes = desc->size;
    }

    trace_aw_h3_sdhost_process_desc(desc_addr, desc->size, is_write, max_bytes);

    while (num_done < num_bytes) {
        /* Try to completely fill the local buffer */
        uint32_t buf_bytes = num_bytes - num_done;
        if (buf_bytes > sizeof(buf)) {
            buf_bytes = sizeof(buf);
        }

        /* Write to SD bus */
        if (is_write) {
            cpu_physical_memory_read((desc->addr & DESC_SIZE_MASK) + num_done,
                                      buf, buf_bytes);

            for (uint32_t i = 0; i < buf_bytes; i++) {
                sdbus_write_data(&s->sdbus, buf[i]);
            }

        /* Read from SD bus */
        } else {
            for (uint32_t i = 0; i < buf_bytes; i++) {
                buf[i] = sdbus_read_data(&s->sdbus);
            }
            cpu_physical_memory_write((desc->addr & DESC_SIZE_MASK) + num_done,
                                       buf, buf_bytes);
        }
        num_done += buf_bytes;
    }

    /* Clear hold flag and flush descriptor */
    desc->status &= ~DESC_STATUS_HOLD;
    cpu_physical_memory_write(desc_addr, desc, sizeof(*desc));

    return num_done;
}

static void aw_h3_sdhost_dma(AwH3SDHostState *s)
{
    TransferDescriptor desc;
    hwaddr desc_addr = s->desc_base;
    bool is_write = (s->command & SD_CMDR_WRITE);
    uint32_t bytes_done = 0;

    /* Check if DMA can be performed */
    if (s->byte_count == 0 || s->block_size == 0 ||
      !(s->global_ctl & SD_GCTL_DMA_ENB)) {
        return;
    }

    /*
     * For read operations, data must be available on the SD bus
     * If not, it is an error and we should not act at all
     */
    if (!is_write && !sdbus_data_ready(&s->sdbus)) {
        return;
    }

    /* Process the DMA descriptors until all data is copied */
    while (s->byte_count > 0) {
        bytes_done = aw_h3_sdhost_process_desc(s, desc_addr, &desc,
                                               is_write, s->byte_count);
        aw_h3_sdhost_update_transfer_cnt(s, bytes_done);

        if (bytes_done <= s->byte_count) {
            s->byte_count -= bytes_done;
        } else {
            s->byte_count = 0;
        }

        if (desc.status & DESC_STATUS_LAST) {
            break;
        } else {
            desc_addr = desc.next;
        }
    }

    /* Raise IRQ to signal DMA is completed */
    s->irq_status |= SD_RISR_DATA_COMPLETE | SD_RISR_AUTOCMD_DONE;

    /* Update DMAC bits */
    if (is_write) {
        s->dmac_status |= SD_IDST_TRANSMIT_IRQ;
    } else {
        s->dmac_status |= (SD_IDST_SUM_RECEIVE_IRQ | SD_IDST_RECEIVE_IRQ);
    }
}

static uint64_t aw_h3_sdhost_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    AwH3SDHostState *s = (AwH3SDHostState *)opaque;
    uint32_t res = 0;

    switch (offset) {
    case REG_SD_GCTL:      /* Global Control */
        res = s->global_ctl;
        break;
    case REG_SD_CKCR:      /* Clock Control */
        res = s->clock_ctl;
        break;
    case REG_SD_TMOR:      /* Timeout */
        res = s->timeout;
        break;
    case REG_SD_BWDR:      /* Bus Width */
        res = s->bus_width;
        break;
    case REG_SD_BKSR:      /* Block Size */
        res = s->block_size;
        break;
    case REG_SD_BYCR:      /* Byte Count */
        res = s->byte_count;
        break;
    case REG_SD_CMDR:      /* Command */
        res = s->command;
        break;
    case REG_SD_CAGR:      /* Command Argument */
        res = s->command_arg;
        break;
    case REG_SD_RESP0:     /* Response Zero */
        res = s->response[0];
        break;
    case REG_SD_RESP1:     /* Response One */
        res = s->response[1];
        break;
    case REG_SD_RESP2:     /* Response Two */
        res = s->response[2];
        break;
    case REG_SD_RESP3:     /* Response Three */
        res = s->response[3];
        break;
    case REG_SD_IMKR:      /* Interrupt Mask */
        res = s->irq_mask;
        break;
    case REG_SD_MISR:      /* Masked Interrupt Status */
        res = s->irq_status & s->irq_mask;
        break;
    case REG_SD_RISR:      /* Raw Interrupt Status */
        res = s->irq_status;
        break;
    case REG_SD_STAR:      /* Status */
        res = s->status;
        break;
    case REG_SD_FWLR:      /* FIFO Water Level */
        res = s->fifo_wlevel;
        break;
    case REG_SD_FUNS:      /* FIFO Function Select */
        res = s->fifo_func_sel;
        break;
    case REG_SD_DBGC:      /* Debug Enable */
        res = s->debug_enable;
        break;
    case REG_SD_A12A:      /* Auto command 12 argument */
        res = s->auto12_arg;
        break;
    case REG_SD_NTSR:      /* SD NewTiming Set */
        res = s->newtiming_set;
        break;
    case REG_SD_SDBG:      /* SD newTiming Set Debug */
        res = s->newtiming_debug;
        break;
    case REG_SD_HWRST:     /* Hardware Reset Register */
        res = s->hardware_rst;
        break;
    case REG_SD_DMAC:      /* Internal DMA Controller Control */
        res = s->dmac;
        break;
    case REG_SD_DLBA:      /* Descriptor List Base Address */
        res = s->desc_base;
        break;
    case REG_SD_IDST:      /* Internal DMA Controller Status */
        res = s->dmac_status;
        break;
    case REG_SD_IDIE:      /* Internal DMA Controller Interrupt Enable */
        res = s->dmac_irq;
        break;
    case REG_SD_THLDC:     /* Card Threshold Control */
        res = s->card_threshold;
        break;
    case REG_SD_DSBD:      /* eMMC DDR Start Bit Detection Control */
        res = s->startbit_detect;
        break;
    case REG_SD_RES_CRC:   /* Response CRC from card/eMMC */
        res = s->response_crc;
        break;
    case REG_SD_DATA7_CRC: /* CRC Data 7 from card/eMMC */
    case REG_SD_DATA6_CRC: /* CRC Data 6 from card/eMMC */
    case REG_SD_DATA5_CRC: /* CRC Data 5 from card/eMMC */
    case REG_SD_DATA4_CRC: /* CRC Data 4 from card/eMMC */
    case REG_SD_DATA3_CRC: /* CRC Data 3 from card/eMMC */
    case REG_SD_DATA2_CRC: /* CRC Data 2 from card/eMMC */
    case REG_SD_DATA1_CRC: /* CRC Data 1 from card/eMMC */
    case REG_SD_DATA0_CRC: /* CRC Data 0 from card/eMMC */
        res = s->data_crc[((offset - REG_SD_DATA7_CRC) / sizeof(uint32_t))];
        break;
    case REG_SD_CRC_STA:   /* CRC status from card/eMMC in write operation */
        res = s->status_crc;
        break;
    case REG_SD_FIFO:      /* Read/Write FIFO */
        if (sdbus_data_ready(&s->sdbus)) {
            res = sdbus_read_data(&s->sdbus);
            res |= sdbus_read_data(&s->sdbus) << 8;
            res |= sdbus_read_data(&s->sdbus) << 16;
            res |= sdbus_read_data(&s->sdbus) << 24;
            aw_h3_sdhost_update_transfer_cnt(s, sizeof(uint32_t));
            aw_h3_sdhost_auto_stop(s);
            aw_h3_sdhost_update_irq(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: no data ready on SD bus\n",
                          __func__);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        res = 0;
        break;
    }

    trace_aw_h3_sdhost_read(offset, res, size);
    return res;
}

static void aw_h3_sdhost_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    AwH3SDHostState *s = (AwH3SDHostState *)opaque;

    trace_aw_h3_sdhost_write(offset, value, size);

    switch (offset) {
    case REG_SD_GCTL:      /* Global Control */
        s->global_ctl = value;
        s->global_ctl &= ~(SD_GCTL_DMA_RST | SD_GCTL_FIFO_RST |
                           SD_GCTL_SOFT_RST);
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_CKCR:      /* Clock Control */
        s->clock_ctl = value;
        break;
    case REG_SD_TMOR:      /* Timeout */
        s->timeout = value;
        break;
    case REG_SD_BWDR:      /* Bus Width */
        s->bus_width = value;
        break;
    case REG_SD_BKSR:      /* Block Size */
        s->block_size = value;
        break;
    case REG_SD_BYCR:      /* Byte Count */
        s->byte_count = value;
        s->transfer_cnt = value;
        break;
    case REG_SD_CMDR:      /* Command */
        s->command = value;
        if (value & SD_CMDR_LOAD) {
            aw_h3_sdhost_send_command(s);
            aw_h3_sdhost_dma(s);
            aw_h3_sdhost_auto_stop(s);
        }
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_CAGR:      /* Command Argument */
        s->command_arg = value;
        break;
    case REG_SD_RESP0:     /* Response Zero */
        s->response[0] = value;
        break;
    case REG_SD_RESP1:     /* Response One */
        s->response[1] = value;
        break;
    case REG_SD_RESP2:     /* Response Two */
        s->response[2] = value;
        break;
    case REG_SD_RESP3:     /* Response Three */
        s->response[3] = value;
        break;
    case REG_SD_IMKR:      /* Interrupt Mask */
        s->irq_mask = value;
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_MISR:      /* Masked Interrupt Status */
    case REG_SD_RISR:      /* Raw Interrupt Status */
        s->irq_status &= ~value;
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_STAR:      /* Status */
        s->status &= ~value;
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_FWLR:      /* FIFO Water Level */
        s->fifo_wlevel = value;
        break;
    case REG_SD_FUNS:      /* FIFO Function Select */
        s->fifo_func_sel = value;
        break;
    case REG_SD_DBGC:      /* Debug Enable */
        s->debug_enable = value;
        break;
    case REG_SD_A12A:      /* Auto command 12 argument */
        s->auto12_arg = value;
        break;
    case REG_SD_NTSR:      /* SD NewTiming Set */
        s->newtiming_set = value;
        break;
    case REG_SD_SDBG:      /* SD newTiming Set Debug */
        s->newtiming_debug = value;
        break;
    case REG_SD_HWRST:     /* Hardware Reset Register */
        s->hardware_rst = value;
        break;
    case REG_SD_DMAC:      /* Internal DMA Controller Control */
        s->dmac = value;
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_DLBA:      /* Descriptor List Base Address */
        s->desc_base = value;
        break;
    case REG_SD_IDST:      /* Internal DMA Controller Status */
        s->dmac_status &= (~SD_IDST_WR_MASK) | (~value & SD_IDST_WR_MASK);
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_IDIE:      /* Internal DMA Controller Interrupt Enable */
        s->dmac_irq = value;
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_THLDC:     /* Card Threshold Control */
        s->card_threshold = value;
        break;
    case REG_SD_DSBD:      /* eMMC DDR Start Bit Detection Control */
        s->startbit_detect = value;
        break;
    case REG_SD_FIFO:      /* Read/Write FIFO */
        sdbus_write_data(&s->sdbus, value & 0xff);
        sdbus_write_data(&s->sdbus, (value >> 8) & 0xff);
        sdbus_write_data(&s->sdbus, (value >> 16) & 0xff);
        sdbus_write_data(&s->sdbus, (value >> 24) & 0xff);
        aw_h3_sdhost_update_transfer_cnt(s, sizeof(uint32_t));
        aw_h3_sdhost_auto_stop(s);
        aw_h3_sdhost_update_irq(s);
        break;
    case REG_SD_RES_CRC:   /* Response CRC from card/eMMC */
    case REG_SD_DATA7_CRC: /* CRC Data 7 from card/eMMC */
    case REG_SD_DATA6_CRC: /* CRC Data 6 from card/eMMC */
    case REG_SD_DATA5_CRC: /* CRC Data 5 from card/eMMC */
    case REG_SD_DATA4_CRC: /* CRC Data 4 from card/eMMC */
    case REG_SD_DATA3_CRC: /* CRC Data 3 from card/eMMC */
    case REG_SD_DATA2_CRC: /* CRC Data 2 from card/eMMC */
    case REG_SD_DATA1_CRC: /* CRC Data 1 from card/eMMC */
    case REG_SD_DATA0_CRC: /* CRC Data 0 from card/eMMC */
    case REG_SD_CRC_STA:   /* CRC status from card/eMMC in write operation */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps aw_h3_sdhost_ops = {
    .read = aw_h3_sdhost_read,
    .write = aw_h3_sdhost_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_aw_h3_sdhost = {
    .name = TYPE_AW_H3_SDHOST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(global_ctl, AwH3SDHostState),
        VMSTATE_UINT32(clock_ctl, AwH3SDHostState),
        VMSTATE_UINT32(timeout, AwH3SDHostState),
        VMSTATE_UINT32(bus_width, AwH3SDHostState),
        VMSTATE_UINT32(block_size, AwH3SDHostState),
        VMSTATE_UINT32(byte_count, AwH3SDHostState),
        VMSTATE_UINT32(transfer_cnt, AwH3SDHostState),
        VMSTATE_UINT32(command, AwH3SDHostState),
        VMSTATE_UINT32(command_arg, AwH3SDHostState),
        VMSTATE_UINT32_ARRAY(response, AwH3SDHostState, 4),
        VMSTATE_UINT32(irq_mask, AwH3SDHostState),
        VMSTATE_UINT32(irq_status, AwH3SDHostState),
        VMSTATE_UINT32(status, AwH3SDHostState),
        VMSTATE_UINT32(fifo_wlevel, AwH3SDHostState),
        VMSTATE_UINT32(fifo_func_sel, AwH3SDHostState),
        VMSTATE_UINT32(debug_enable, AwH3SDHostState),
        VMSTATE_UINT32(auto12_arg, AwH3SDHostState),
        VMSTATE_UINT32(newtiming_set, AwH3SDHostState),
        VMSTATE_UINT32(newtiming_debug, AwH3SDHostState),
        VMSTATE_UINT32(hardware_rst, AwH3SDHostState),
        VMSTATE_UINT32(dmac, AwH3SDHostState),
        VMSTATE_UINT32(desc_base, AwH3SDHostState),
        VMSTATE_UINT32(dmac_status, AwH3SDHostState),
        VMSTATE_UINT32(dmac_irq, AwH3SDHostState),
        VMSTATE_UINT32(card_threshold, AwH3SDHostState),
        VMSTATE_UINT32(startbit_detect, AwH3SDHostState),
        VMSTATE_UINT32(response_crc, AwH3SDHostState),
        VMSTATE_UINT32_ARRAY(data_crc, AwH3SDHostState, 8),
        VMSTATE_UINT32(status_crc, AwH3SDHostState),
        VMSTATE_END_OF_LIST()
    }
};

static void aw_h3_sdhost_init(Object *obj)
{
    AwH3SDHostState *s = AW_H3_SDHOST(obj);

    qbus_create_inplace(&s->sdbus, sizeof(s->sdbus),
                        TYPE_AW_H3_SDHOST_BUS, DEVICE(s), "sd-bus");

    memory_region_init_io(&s->iomem, obj, &aw_h3_sdhost_ops, s,
                          TYPE_AW_H3_SDHOST, AW_H3_SDHOST_REGS_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void aw_h3_sdhost_reset(DeviceState *dev)
{
    AwH3SDHostState *s = AW_H3_SDHOST(dev);

    s->global_ctl = REG_SD_GCTL_RST;
    s->clock_ctl = REG_SD_CKCR_RST;
    s->timeout = REG_SD_TMOR_RST;
    s->bus_width = REG_SD_BWDR_RST;
    s->block_size = REG_SD_BKSR_RST;
    s->byte_count = REG_SD_BYCR_RST;
    s->transfer_cnt = 0;

    s->command = REG_SD_CMDR_RST;
    s->command_arg = REG_SD_CAGR_RST;

    for (int i = 0; i < sizeof(s->response) / sizeof(s->response[0]); i++) {
        s->response[i] = REG_SD_RESP_RST;
    }

    s->irq_mask = REG_SD_IMKR_RST;
    s->irq_status = REG_SD_RISR_RST;
    s->status = REG_SD_STAR_RST;

    s->fifo_wlevel = REG_SD_FWLR_RST;
    s->fifo_func_sel = REG_SD_FUNS_RST;
    s->debug_enable = REG_SD_DBGC_RST;
    s->auto12_arg = REG_SD_A12A_RST;
    s->newtiming_set = REG_SD_NTSR_RST;
    s->newtiming_debug = REG_SD_SDBG_RST;
    s->hardware_rst = REG_SD_HWRST_RST;
    s->dmac = REG_SD_DMAC_RST;
    s->desc_base = REG_SD_DLBA_RST;
    s->dmac_status = REG_SD_IDST_RST;
    s->dmac_irq = REG_SD_IDIE_RST;
    s->card_threshold = REG_SD_THLDC_RST;
    s->startbit_detect = REG_SD_DSBD_RST;
    s->response_crc = REG_SD_RES_CRC_RST;

    for (int i = 0; i < sizeof(s->data_crc) / sizeof(s->data_crc[0]); i++) {
        s->data_crc[i] = REG_SD_DATA_CRC_RST;
    }

    s->status_crc = REG_SD_CRC_STA_RST;
}

static void aw_h3_sdhost_bus_class_init(ObjectClass *klass, void *data)
{
    SDBusClass *sbc = SD_BUS_CLASS(klass);

    sbc->set_inserted = aw_h3_sdhost_set_inserted;
}

static void aw_h3_sdhost_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = aw_h3_sdhost_reset;
    dc->vmsd = &vmstate_aw_h3_sdhost;
}

static TypeInfo aw_h3_sdhost_info = {
    .name          = TYPE_AW_H3_SDHOST,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AwH3SDHostState),
    .class_init    = aw_h3_sdhost_class_init,
    .instance_init = aw_h3_sdhost_init,
};

static const TypeInfo aw_h3_sdhost_bus_info = {
    .name = TYPE_AW_H3_SDHOST_BUS,
    .parent = TYPE_SD_BUS,
    .instance_size = sizeof(SDBus),
    .class_init = aw_h3_sdhost_bus_class_init,
};

static void aw_h3_sdhost_register_types(void)
{
    type_register_static(&aw_h3_sdhost_info);
    type_register_static(&aw_h3_sdhost_bus_info);
}

type_init(aw_h3_sdhost_register_types)
