/*
 * BCM2835 MMC host driver.
 *
 * Author:      Gellert Weisz <gellert@raspberrypi.org>
 *              Copyright 2014
 *
 * Based on
 *  sdhci-bcm2708.c by Broadcom
 *  sdhci-bcm2835.c by Stephen Warren and Oleksandr Tymoshenko
 *  sdhci.c and sdhci-pci.c by Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sd.h>
#include <linux/scatterlist.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/blkdev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/of_dma.h>

#include "sdhci.h"


#define DRIVER_NAME "mmc-bcm2835"

#define DBG(f, x...) \
pr_debug(DRIVER_NAME " [%s()]: " f, __func__, ## x)

#ifndef CONFIG_MMC_BCM2835_DMA
 #define FORCE_PIO
#endif


/* the inclusive limit in bytes under which PIO will be used instead of DMA */
#ifdef CONFIG_MMC_BCM2835_PIO_DMA_BARRIER
#define PIO_DMA_BARRIER CONFIG_MMC_BCM2835_PIO_DMA_BARRIER
#else
#define PIO_DMA_BARRIER 00
#endif

#define MIN_FREQ 400000
#define TIMEOUT_VAL 0xE
#define BCM2835_SDHCI_WRITE_DELAY(f)	(((2 * 1000000) / f) + 1)

#ifndef BCM2708_PERI_BASE
 #define BCM2708_PERI_BASE 0x20000000
#endif

/* FIXME: Needs IOMMU support */
#define BCM2835_VCMMU_SHIFT		(0x7E000000 - BCM2708_PERI_BASE)


unsigned mmc_debug;
unsigned mmc_debug2;

struct bcm2835_host {
	spinlock_t				lock;

	void __iomem			*ioaddr;
	u32						phys_addr;

	struct mmc_host			*mmc;

	u32						timeout;

	int						clock;	/* Current clock speed */
	u8						pwr;	/* Current voltage */

	unsigned int			max_clk;		/* Max possible freq */
	unsigned int			timeout_clk;	/* Timeout freq (KHz) */
	unsigned int			clk_mul;		/* Clock Muliplier value */

	struct tasklet_struct	finish_tasklet;		/* Tasklet structures */

	struct timer_list		timer;			/* Timer for timeouts */

	struct sg_mapping_iter	sg_miter;		/* SG state for PIO */
	unsigned int			blocks;			/* remaining PIO blocks */

	int						irq;			/* Device IRQ */


	u32						ier;			/* cached registers */

	struct mmc_request		*mrq;			/* Current request */
	struct mmc_command		*cmd;			/* Current command */
	struct mmc_data			*data;			/* Current data request */
	unsigned int			data_early:1;		/* Data finished before cmd */

	wait_queue_head_t		buf_ready_int;		/* Waitqueue for Buffer Read Ready interrupt */

	u32						thread_isr;

	u32						shadow;

	/*DMA part*/
	struct dma_chan			*dma_chan_rx;		/* DMA channel for reads */
	struct dma_chan			*dma_chan_tx;		/* DMA channel for writes */
	struct dma_async_tx_descriptor	*tx_desc;	/* descriptor */

	bool					have_dma;
	bool					use_dma;
	/*end of DMA part*/

	int						max_delay;	/* maximum length of time spent waiting */

	int						flags;				/* Host attributes */
#define SDHCI_REQ_USE_DMA	(1<<2)	/* Use DMA for this req. */
#define SDHCI_DEVICE_DEAD	(1<<3)	/* Device unresponsive */
#define SDHCI_AUTO_CMD12	(1<<6)	/* Auto CMD12 support */
#define SDHCI_AUTO_CMD23	(1<<7)	/* Auto CMD23 support */
#define SDHCI_SDIO_IRQ_ENABLED	(1<<9)	/* SDIO irq enabled */

	u32				overclock_50;	/* frequency to use when 50MHz is requested (in MHz) */
	u32				max_overclock;	/* Highest reported */
};


static inline void bcm2835_mmc_writel(struct bcm2835_host *host, u32 val, int reg, int from)
{
	unsigned delay;
	lockdep_assert_held_once(&host->lock);
	writel(val, host->ioaddr + reg);
	udelay(BCM2835_SDHCI_WRITE_DELAY(max(host->clock, MIN_FREQ)));

	delay = ((mmc_debug >> 16) & 0xf) << ((mmc_debug >> 20) & 0xf);
	if (delay && !((1<<from) & mmc_debug2))
		udelay(delay);
}

static inline void mmc_raw_writel(struct bcm2835_host *host, u32 val, int reg)
{
	unsigned delay;
	lockdep_assert_held_once(&host->lock);
	writel(val, host->ioaddr + reg);

	delay = ((mmc_debug >> 24) & 0xf) << ((mmc_debug >> 28) & 0xf);
	if (delay)
		udelay(delay);
}

static inline u32 bcm2835_mmc_readl(struct bcm2835_host *host, int reg)
{
	lockdep_assert_held_once(&host->lock);
	return readl(host->ioaddr + reg);
}

static inline void bcm2835_mmc_writew(struct bcm2835_host *host, u16 val, int reg)
{
	u32 oldval = (reg == SDHCI_COMMAND) ? host->shadow :
		bcm2835_mmc_readl(host, reg & ~3);
	u32 word_num = (reg >> 1) & 1;
	u32 word_shift = word_num * 16;
	u32 mask = 0xffff << word_shift;
	u32 newval = (oldval & ~mask) | (val << word_shift);

	if (reg == SDHCI_TRANSFER_MODE)
		host->shadow = newval;
	else
		bcm2835_mmc_writel(host, newval, reg & ~3, 0);

}

static inline void bcm2835_mmc_writeb(struct bcm2835_host *host, u8 val, int reg)
{
	u32 oldval = bcm2835_mmc_readl(host, reg & ~3);
	u32 byte_num = reg & 3;
	u32 byte_shift = byte_num * 8;
	u32 mask = 0xff << byte_shift;
	u32 newval = (oldval & ~mask) | (val << byte_shift);

	bcm2835_mmc_writel(host, newval, reg & ~3, 1);
}


static inline u16 bcm2835_mmc_readw(struct bcm2835_host *host, int reg)
{
	u32 val = bcm2835_mmc_readl(host, (reg & ~3));
	u32 word_num = (reg >> 1) & 1;
	u32 word_shift = word_num * 16;
	u32 word = (val >> word_shift) & 0xffff;

	return word;
}

static inline u8 bcm2835_mmc_readb(struct bcm2835_host *host, int reg)
{
	u32 val = bcm2835_mmc_readl(host, (reg & ~3));
	u32 byte_num = reg & 3;
	u32 byte_shift = byte_num * 8;
	u32 byte = (val >> byte_shift) & 0xff;

	return byte;
}

static void bcm2835_mmc_unsignal_irqs(struct bcm2835_host *host, u32 clear)
{
	u32 ier;

	ier = bcm2835_mmc_readl(host, SDHCI_SIGNAL_ENABLE);
	ier &= ~clear;
	/* change which requests generate IRQs - makes no difference to
	   the content of SDHCI_INT_STATUS, or the need to acknowledge IRQs */
	bcm2835_mmc_writel(host, ier, SDHCI_SIGNAL_ENABLE, 2);
}


static void bcm2835_mmc_dumpregs(struct bcm2835_host *host)
{
	pr_debug(DRIVER_NAME ": =========== REGISTER DUMP (%s)===========\n",
		mmc_hostname(host->mmc));

	pr_debug(DRIVER_NAME ": Sys addr: 0x%08x | Version:  0x%08x\n",
		bcm2835_mmc_readl(host, SDHCI_DMA_ADDRESS),
		bcm2835_mmc_readw(host, SDHCI_HOST_VERSION));
	pr_debug(DRIVER_NAME ": Blk size: 0x%08x | Blk cnt:  0x%08x\n",
		bcm2835_mmc_readw(host, SDHCI_BLOCK_SIZE),
		bcm2835_mmc_readw(host, SDHCI_BLOCK_COUNT));
	pr_debug(DRIVER_NAME ": Argument: 0x%08x | Trn mode: 0x%08x\n",
		bcm2835_mmc_readl(host, SDHCI_ARGUMENT),
		bcm2835_mmc_readw(host, SDHCI_TRANSFER_MODE));
	pr_debug(DRIVER_NAME ": Present:  0x%08x | Host ctl: 0x%08x\n",
		bcm2835_mmc_readl(host, SDHCI_PRESENT_STATE),
		bcm2835_mmc_readb(host, SDHCI_HOST_CONTROL));
	pr_debug(DRIVER_NAME ": Power:    0x%08x | Blk gap:  0x%08x\n",
		bcm2835_mmc_readb(host, SDHCI_POWER_CONTROL),
		bcm2835_mmc_readb(host, SDHCI_BLOCK_GAP_CONTROL));
	pr_debug(DRIVER_NAME ": Wake-up:  0x%08x | Clock:    0x%08x\n",
		bcm2835_mmc_readb(host, SDHCI_WAKE_UP_CONTROL),
		bcm2835_mmc_readw(host, SDHCI_CLOCK_CONTROL));
	pr_debug(DRIVER_NAME ": Timeout:  0x%08x | Int stat: 0x%08x\n",
		bcm2835_mmc_readb(host, SDHCI_TIMEOUT_CONTROL),
		bcm2835_mmc_readl(host, SDHCI_INT_STATUS));
	pr_debug(DRIVER_NAME ": Int enab: 0x%08x | Sig enab: 0x%08x\n",
		bcm2835_mmc_readl(host, SDHCI_INT_ENABLE),
		bcm2835_mmc_readl(host, SDHCI_SIGNAL_ENABLE));
	pr_debug(DRIVER_NAME ": AC12 err: 0x%08x | Slot int: 0x%08x\n",
		bcm2835_mmc_readw(host, SDHCI_ACMD12_ERR),
		bcm2835_mmc_readw(host, SDHCI_SLOT_INT_STATUS));
	pr_debug(DRIVER_NAME ": Caps:     0x%08x | Caps_1:   0x%08x\n",
		bcm2835_mmc_readl(host, SDHCI_CAPABILITIES),
		bcm2835_mmc_readl(host, SDHCI_CAPABILITIES_1));
	pr_debug(DRIVER_NAME ": Cmd:      0x%08x | Max curr: 0x%08x\n",
		bcm2835_mmc_readw(host, SDHCI_COMMAND),
		bcm2835_mmc_readl(host, SDHCI_MAX_CURRENT));
	pr_debug(DRIVER_NAME ": Host ctl2: 0x%08x\n",
		bcm2835_mmc_readw(host, SDHCI_HOST_CONTROL2));

	pr_debug(DRIVER_NAME ": ===========================================\n");
}


static void bcm2835_mmc_reset(struct bcm2835_host *host, u8 mask)
{
	unsigned long timeout;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	bcm2835_mmc_writeb(host, mask, SDHCI_SOFTWARE_RESET);

	if (mask & SDHCI_RESET_ALL)
		host->clock = 0;

	/* Wait max 100 ms */
	timeout = 100;

	/* hw clears the bit when it's done */
	while (bcm2835_mmc_readb(host, SDHCI_SOFTWARE_RESET) & mask) {
		if (timeout == 0) {
			pr_err("%s: Reset 0x%x never completed.\n",
				mmc_hostname(host->mmc), (int)mask);
			bcm2835_mmc_dumpregs(host);
			return;
		}
		timeout--;
		spin_unlock_irqrestore(&host->lock, flags);
		mdelay(1);
		spin_lock_irqsave(&host->lock, flags);
	}

	if (100-timeout > 10 && 100-timeout > host->max_delay) {
		host->max_delay = 100-timeout;
		pr_warning("Warning: MMC controller hung for %d ms\n", host->max_delay);
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

static void bcm2835_mmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios);

static void bcm2835_mmc_init(struct bcm2835_host *host, int soft)
{
	unsigned long flags;
	if (soft)
		bcm2835_mmc_reset(host, SDHCI_RESET_CMD|SDHCI_RESET_DATA);
	else
		bcm2835_mmc_reset(host, SDHCI_RESET_ALL);

	host->ier = SDHCI_INT_BUS_POWER | SDHCI_INT_DATA_END_BIT |
		    SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_TIMEOUT |
		    SDHCI_INT_INDEX | SDHCI_INT_END_BIT | SDHCI_INT_CRC |
		    SDHCI_INT_TIMEOUT | SDHCI_INT_DATA_END |
		    SDHCI_INT_RESPONSE;

	spin_lock_irqsave(&host->lock, flags);
	bcm2835_mmc_writel(host, host->ier, SDHCI_INT_ENABLE, 3);
	bcm2835_mmc_writel(host, host->ier, SDHCI_SIGNAL_ENABLE, 3);
	spin_unlock_irqrestore(&host->lock, flags);

	if (soft) {
		/* force clock reconfiguration */
		host->clock = 0;
		bcm2835_mmc_set_ios(host->mmc, &host->mmc->ios);
	}
}



static void bcm2835_mmc_finish_data(struct bcm2835_host *host);

static void bcm2835_mmc_dma_complete(void *param)
{
	struct bcm2835_host *host = param;
	struct dma_chan *dma_chan;
	unsigned long flags;
	u32 dir_data;

	spin_lock_irqsave(&host->lock, flags);

	if (host->data && !(host->data->flags & MMC_DATA_WRITE)) {
		/* otherwise handled in SDHCI IRQ */
		dma_chan = host->dma_chan_rx;
		dir_data = DMA_FROM_DEVICE;

		dma_unmap_sg(dma_chan->device->dev,
		     host->data->sg, host->data->sg_len,
		     dir_data);

		bcm2835_mmc_finish_data(host);
	}

	spin_unlock_irqrestore(&host->lock, flags);
}

static void bcm2835_bcm2835_mmc_read_block_pio(struct bcm2835_host *host)
{
	unsigned long flags;
	size_t blksize, len, chunk;

	u32 uninitialized_var(scratch);
	u8 *buf;

	blksize = host->data->blksz;
	chunk = 0;

	local_irq_save(flags);

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		while (len) {
			if (chunk == 0) {
				scratch = bcm2835_mmc_readl(host, SDHCI_BUFFER);
				chunk = 4;
			}

			*buf = scratch & 0xFF;

			buf++;
			scratch >>= 8;
			chunk--;
			len--;
		}
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void bcm2835_bcm2835_mmc_write_block_pio(struct bcm2835_host *host)
{
	unsigned long flags;
	size_t blksize, len, chunk;
	u32 scratch;
	u8 *buf;

	blksize = host->data->blksz;
	chunk = 0;
	chunk = 0;
	scratch = 0;

	local_irq_save(flags);

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		while (len) {
			scratch |= (u32)*buf << (chunk * 8);

			buf++;
			chunk++;
			len--;

			if ((chunk == 4) || ((len == 0) && (blksize == 0))) {
				mmc_raw_writel(host, scratch, SDHCI_BUFFER);
				chunk = 0;
				scratch = 0;
			}
		}
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}


static void bcm2835_mmc_transfer_pio(struct bcm2835_host *host)
{
	u32 mask;

	BUG_ON(!host->data);

	if (host->blocks == 0)
		return;

	if (host->data->flags & MMC_DATA_READ)
		mask = SDHCI_DATA_AVAILABLE;
	else
		mask = SDHCI_SPACE_AVAILABLE;

	while (bcm2835_mmc_readl(host, SDHCI_PRESENT_STATE) & mask) {

		if (host->data->flags & MMC_DATA_READ)
			bcm2835_bcm2835_mmc_read_block_pio(host);
		else
			bcm2835_bcm2835_mmc_write_block_pio(host);

		host->blocks--;

		/* QUIRK used in sdhci.c removes the 'if' */
		/* but it seems this is unnecessary */
		if (host->blocks == 0)
			break;


	}
}


static void bcm2835_mmc_transfer_dma(struct bcm2835_host *host)
{
	u32 len, dir_data, dir_slave;
	struct dma_async_tx_descriptor *desc = NULL;
	struct dma_chan *dma_chan;


	WARN_ON(!host->data);

	if (!host->data)
		return;

	if (host->blocks == 0)
		return;

	if (host->data->flags & MMC_DATA_READ) {
		dma_chan = host->dma_chan_rx;
		dir_data = DMA_FROM_DEVICE;
		dir_slave = DMA_DEV_TO_MEM;
	} else {
		dma_chan = host->dma_chan_tx;
		dir_data = DMA_TO_DEVICE;
		dir_slave = DMA_MEM_TO_DEV;
	}

	BUG_ON(!dma_chan->device);
	BUG_ON(!dma_chan->device->dev);
	BUG_ON(!host->data->sg);

	len = dma_map_sg(dma_chan->device->dev, host->data->sg,
			 host->data->sg_len, dir_data);
	if (len > 0) {
		desc = dmaengine_prep_slave_sg(dma_chan, host->data->sg,
					       len, dir_slave,
					       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	} else {
		dev_err(mmc_dev(host->mmc), "dma_map_sg returned zero length\n");
	}
	if (desc) {
		unsigned long flags;
		spin_lock_irqsave(&host->lock, flags);
		bcm2835_mmc_unsignal_irqs(host, SDHCI_INT_DATA_AVAIL |
						    SDHCI_INT_SPACE_AVAIL);
		host->tx_desc = desc;
		desc->callback = bcm2835_mmc_dma_complete;
		desc->callback_param = host;
		spin_unlock_irqrestore(&host->lock, flags);
		dmaengine_submit(desc);
		dma_async_issue_pending(dma_chan);
	}

}



static void bcm2835_mmc_set_transfer_irqs(struct bcm2835_host *host)
{
	u32 pio_irqs = SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL;
	u32 dma_irqs = SDHCI_INT_DMA_END | SDHCI_INT_ADMA_ERROR;

	if (host->use_dma)
		host->ier = (host->ier & ~pio_irqs) | dma_irqs;
	else
		host->ier = (host->ier & ~dma_irqs) | pio_irqs;

	bcm2835_mmc_writel(host, host->ier, SDHCI_INT_ENABLE, 4);
	bcm2835_mmc_writel(host, host->ier, SDHCI_SIGNAL_ENABLE, 4);
}


static void bcm2835_mmc_prepare_data(struct bcm2835_host *host, struct mmc_command *cmd)
{
	u8 count;
	struct mmc_data *data = cmd->data;

	WARN_ON(host->data);

	if (data || (cmd->flags & MMC_RSP_BUSY)) {
		count = TIMEOUT_VAL;
		bcm2835_mmc_writeb(host, count, SDHCI_TIMEOUT_CONTROL);
	}

	if (!data)
		return;

	/* Sanity checks */
	BUG_ON(data->blksz * data->blocks > 524288);
	BUG_ON(data->blksz > host->mmc->max_blk_size);
	BUG_ON(data->blocks > 65535);

	host->data = data;
	host->data_early = 0;
	host->data->bytes_xfered = 0;


	if (!(host->flags & SDHCI_REQ_USE_DMA)) {
		int flags;

		flags = SG_MITER_ATOMIC;
		if (host->data->flags & MMC_DATA_READ)
			flags |= SG_MITER_TO_SG;
		else
			flags |= SG_MITER_FROM_SG;
		sg_miter_start(&host->sg_miter, data->sg, data->sg_len, flags);
		host->blocks = data->blocks;
	}

	host->use_dma = host->have_dma && data->blocks > PIO_DMA_BARRIER;

	bcm2835_mmc_set_transfer_irqs(host);

	/* Set the DMA boundary value and block size */
	bcm2835_mmc_writew(host, SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY_ARG,
		data->blksz), SDHCI_BLOCK_SIZE);
	bcm2835_mmc_writew(host, data->blocks, SDHCI_BLOCK_COUNT);

	BUG_ON(!host->data);
}

static void bcm2835_mmc_set_transfer_mode(struct bcm2835_host *host,
	struct mmc_command *cmd)
{
	u16 mode;
	struct mmc_data *data = cmd->data;

	if (data == NULL) {
		/* clear Auto CMD settings for no data CMDs */
		mode = bcm2835_mmc_readw(host, SDHCI_TRANSFER_MODE);
		bcm2835_mmc_writew(host, mode & ~(SDHCI_TRNS_AUTO_CMD12 |
				SDHCI_TRNS_AUTO_CMD23), SDHCI_TRANSFER_MODE);
		return;
	}

	WARN_ON(!host->data);

	mode = SDHCI_TRNS_BLK_CNT_EN;

	if ((mmc_op_multi(cmd->opcode) || data->blocks > 1)) {
		mode |= SDHCI_TRNS_MULTI;

		/*
		 * If we are sending CMD23, CMD12 never gets sent
		 * on successful completion (so no Auto-CMD12).
		 */
		if (!host->mrq->sbc && (host->flags & SDHCI_AUTO_CMD12))
			mode |= SDHCI_TRNS_AUTO_CMD12;
		else if (host->mrq->sbc && (host->flags & SDHCI_AUTO_CMD23)) {
			mode |= SDHCI_TRNS_AUTO_CMD23;
			bcm2835_mmc_writel(host, host->mrq->sbc->arg, SDHCI_ARGUMENT2, 5);
		}
	}

	if (data->flags & MMC_DATA_READ)
		mode |= SDHCI_TRNS_READ;
	if (host->flags & SDHCI_REQ_USE_DMA)
		mode |= SDHCI_TRNS_DMA;

	bcm2835_mmc_writew(host, mode, SDHCI_TRANSFER_MODE);
}

void bcm2835_mmc_send_command(struct bcm2835_host *host, struct mmc_command *cmd)
{
	int flags;
	u32 mask;
	unsigned long timeout;

	WARN_ON(host->cmd);

	/* Wait max 10 ms */
	timeout = 1000;

	mask = SDHCI_CMD_INHIBIT;
	if ((cmd->data != NULL) || (cmd->flags & MMC_RSP_BUSY))
		mask |= SDHCI_DATA_INHIBIT;

	/* We shouldn't wait for data inihibit for stop commands, even
	   though they might use busy signaling */
	if (host->mrq->data && (cmd == host->mrq->data->stop))
		mask &= ~SDHCI_DATA_INHIBIT;

	while (bcm2835_mmc_readl(host, SDHCI_PRESENT_STATE) & mask) {
		if (timeout == 0) {
			pr_err("%s: Controller never released inhibit bit(s).\n",
				mmc_hostname(host->mmc));
			bcm2835_mmc_dumpregs(host);
			cmd->error = -EIO;
			tasklet_schedule(&host->finish_tasklet);
			return;
		}
		timeout--;
		udelay(10);
	}

	if ((1000-timeout)/100 > 1 && (1000-timeout)/100 > host->max_delay) {
		host->max_delay = (1000-timeout)/100;
		pr_warning("Warning: MMC controller hung for %d ms\n", host->max_delay);
	}

	timeout = jiffies;
	if (!cmd->data && cmd->busy_timeout > 9000)
		timeout += DIV_ROUND_UP(cmd->busy_timeout, 1000) * HZ + HZ;
	else
		timeout += 10 * HZ;
	mod_timer(&host->timer, timeout);

	host->cmd = cmd;

	bcm2835_mmc_prepare_data(host, cmd);

	bcm2835_mmc_writel(host, cmd->arg, SDHCI_ARGUMENT, 6);

	bcm2835_mmc_set_transfer_mode(host, cmd);

	if ((cmd->flags & MMC_RSP_136) && (cmd->flags & MMC_RSP_BUSY)) {
		pr_err("%s: Unsupported response type!\n",
			mmc_hostname(host->mmc));
		cmd->error = -EINVAL;
		tasklet_schedule(&host->finish_tasklet);
		return;
	}

	if (!(cmd->flags & MMC_RSP_PRESENT))
		flags = SDHCI_CMD_RESP_NONE;
	else if (cmd->flags & MMC_RSP_136)
		flags = SDHCI_CMD_RESP_LONG;
	else if (cmd->flags & MMC_RSP_BUSY)
		flags = SDHCI_CMD_RESP_SHORT_BUSY;
	else
		flags = SDHCI_CMD_RESP_SHORT;

	if (cmd->flags & MMC_RSP_CRC)
		flags |= SDHCI_CMD_CRC;
	if (cmd->flags & MMC_RSP_OPCODE)
		flags |= SDHCI_CMD_INDEX;

	if (cmd->data)
		flags |= SDHCI_CMD_DATA;

	bcm2835_mmc_writew(host, SDHCI_MAKE_CMD(cmd->opcode, flags), SDHCI_COMMAND);
}


static void bcm2835_mmc_finish_data(struct bcm2835_host *host)
{
	struct mmc_data *data;

	BUG_ON(!host->data);

	data = host->data;
	host->data = NULL;

	if (data->error)
		data->bytes_xfered = 0;
	else
		data->bytes_xfered = data->blksz * data->blocks;

	/*
	 * Need to send CMD12 if -
	 * a) open-ended multiblock transfer (no CMD23)
	 * b) error in multiblock transfer
	 */
	if (data->stop &&
	    (data->error ||
	     !host->mrq->sbc)) {

		/*
		 * The controller needs a reset of internal state machines
		 * upon error conditions.
		 */
		if (data->error) {
			bcm2835_mmc_reset(host, SDHCI_RESET_CMD);
			bcm2835_mmc_reset(host, SDHCI_RESET_DATA);
		}

		bcm2835_mmc_send_command(host, data->stop);
	} else
		tasklet_schedule(&host->finish_tasklet);
}

static void bcm2835_mmc_finish_command(struct bcm2835_host *host)
{
	int i;

	BUG_ON(host->cmd == NULL);

	if (host->cmd->flags & MMC_RSP_PRESENT) {
		if (host->cmd->flags & MMC_RSP_136) {
			/* CRC is stripped so we need to do some shifting. */
			for (i = 0; i < 4; i++) {
				host->cmd->resp[i] = bcm2835_mmc_readl(host,
					SDHCI_RESPONSE + (3-i)*4) << 8;
				if (i != 3)
					host->cmd->resp[i] |=
						bcm2835_mmc_readb(host,
						SDHCI_RESPONSE + (3-i)*4-1);
			}
		} else {
			host->cmd->resp[0] = bcm2835_mmc_readl(host, SDHCI_RESPONSE);
		}
	}

	host->cmd->error = 0;

	/* Finished CMD23, now send actual command. */
	if (host->cmd == host->mrq->sbc) {
		host->cmd = NULL;
		bcm2835_mmc_send_command(host, host->mrq->cmd);

		if (host->mrq->cmd->data && host->use_dma) {
			/* DMA transfer starts now, PIO starts after interrupt */
			bcm2835_mmc_transfer_dma(host);
		}
	} else {

		/* Processed actual command. */
		if (host->data && host->data_early)
			bcm2835_mmc_finish_data(host);

		if (!host->cmd->data)
			tasklet_schedule(&host->finish_tasklet);

		host->cmd = NULL;
	}
}


static void bcm2835_mmc_timeout_timer(unsigned long data)
{
	struct bcm2835_host *host;
	unsigned long flags;

	host = (struct bcm2835_host *)data;

	spin_lock_irqsave(&host->lock, flags);

	if (host->mrq) {
		pr_err("%s: Timeout waiting for hardware interrupt.\n",
			mmc_hostname(host->mmc));
		bcm2835_mmc_dumpregs(host);

		if (host->data) {
			host->data->error = -ETIMEDOUT;
			bcm2835_mmc_finish_data(host);
		} else {
			if (host->cmd)
				host->cmd->error = -ETIMEDOUT;
			else
				host->mrq->cmd->error = -ETIMEDOUT;

			tasklet_schedule(&host->finish_tasklet);
		}
	}

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}


static void bcm2835_mmc_enable_sdio_irq_nolock(struct bcm2835_host *host, int enable)
{
	if (!(host->flags & SDHCI_DEVICE_DEAD)) {
		if (enable)
			host->ier |= SDHCI_INT_CARD_INT;
		else
			host->ier &= ~SDHCI_INT_CARD_INT;

		bcm2835_mmc_writel(host, host->ier, SDHCI_INT_ENABLE, 7);
		bcm2835_mmc_writel(host, host->ier, SDHCI_SIGNAL_ENABLE, 7);
		mmiowb();
	}
}

static void bcm2835_mmc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct bcm2835_host *host = mmc_priv(mmc);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	if (enable)
		host->flags |= SDHCI_SDIO_IRQ_ENABLED;
	else
		host->flags &= ~SDHCI_SDIO_IRQ_ENABLED;

	bcm2835_mmc_enable_sdio_irq_nolock(host, enable);
	spin_unlock_irqrestore(&host->lock, flags);
}

static void bcm2835_mmc_cmd_irq(struct bcm2835_host *host, u32 intmask)
{

	BUG_ON(intmask == 0);

	if (!host->cmd) {
		pr_err("%s: Got command interrupt 0x%08x even "
			"though no command operation was in progress.\n",
			mmc_hostname(host->mmc), (unsigned)intmask);
		bcm2835_mmc_dumpregs(host);
		return;
	}

	if (intmask & SDHCI_INT_TIMEOUT)
		host->cmd->error = -ETIMEDOUT;
	else if (intmask & (SDHCI_INT_CRC | SDHCI_INT_END_BIT |
			SDHCI_INT_INDEX)) {
			host->cmd->error = -EILSEQ;
	}

	if (host->cmd->error) {
		tasklet_schedule(&host->finish_tasklet);
		return;
	}

	if (intmask & SDHCI_INT_RESPONSE)
		bcm2835_mmc_finish_command(host);

}

static void bcm2835_mmc_data_irq(struct bcm2835_host *host, u32 intmask)
{
	struct dma_chan *dma_chan;
	u32 dir_data;

	BUG_ON(intmask == 0);

	if (!host->data) {
		/*
		 * The "data complete" interrupt is also used to
		 * indicate that a busy state has ended. See comment
		 * above in sdhci_cmd_irq().
		 */
		if (host->cmd && (host->cmd->flags & MMC_RSP_BUSY)) {
			if (intmask & SDHCI_INT_DATA_END) {
				bcm2835_mmc_finish_command(host);
				return;
			}
		}

		pr_debug("%s: Got data interrupt 0x%08x even "
			"though no data operation was in progress.\n",
			mmc_hostname(host->mmc), (unsigned)intmask);
		bcm2835_mmc_dumpregs(host);

		return;
	}

	if (intmask & SDHCI_INT_DATA_TIMEOUT)
		host->data->error = -ETIMEDOUT;
	else if (intmask & SDHCI_INT_DATA_END_BIT)
		host->data->error = -EILSEQ;
	else if ((intmask & SDHCI_INT_DATA_CRC) &&
		SDHCI_GET_CMD(bcm2835_mmc_readw(host, SDHCI_COMMAND))
			!= MMC_BUS_TEST_R)
		host->data->error = -EILSEQ;

	if (host->use_dma) {
		if  (host->data->flags & MMC_DATA_WRITE) {
			/* IRQ handled here */

			dma_chan = host->dma_chan_tx;
			dir_data = DMA_TO_DEVICE;
			dma_unmap_sg(dma_chan->device->dev,
				 host->data->sg, host->data->sg_len,
				 dir_data);

			bcm2835_mmc_finish_data(host);
		}

	} else {
		if (host->data->error)
			bcm2835_mmc_finish_data(host);
		else {
			if (intmask & (SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL))
				bcm2835_mmc_transfer_pio(host);

			if (intmask & SDHCI_INT_DATA_END) {
				if (host->cmd) {
					/*
					 * Data managed to finish before the
					 * command completed. Make sure we do
					 * things in the proper order.
					 */
					host->data_early = 1;
				} else {
					bcm2835_mmc_finish_data(host);
				}
			}
		}
	}
}


static irqreturn_t bcm2835_mmc_irq(int irq, void *dev_id)
{
	irqreturn_t result = IRQ_NONE;
	struct bcm2835_host *host = dev_id;
	u32 intmask, mask, unexpected = 0;
	int max_loops = 16;

	spin_lock(&host->lock);

	intmask = bcm2835_mmc_readl(host, SDHCI_INT_STATUS);

	if (!intmask || intmask == 0xffffffff) {
		result = IRQ_NONE;
		goto out;
	}

	do {
		/* Clear selected interrupts. */
		mask = intmask & (SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK |
				  SDHCI_INT_BUS_POWER);
		bcm2835_mmc_writel(host, mask, SDHCI_INT_STATUS, 8);


		if (intmask & SDHCI_INT_CMD_MASK)
			bcm2835_mmc_cmd_irq(host, intmask & SDHCI_INT_CMD_MASK);

		if (intmask & SDHCI_INT_DATA_MASK)
			bcm2835_mmc_data_irq(host, intmask & SDHCI_INT_DATA_MASK);

		if (intmask & SDHCI_INT_BUS_POWER)
			pr_err("%s: Card is consuming too much power!\n",
				mmc_hostname(host->mmc));

		if (intmask & SDHCI_INT_CARD_INT) {
			bcm2835_mmc_enable_sdio_irq_nolock(host, false);
			host->thread_isr |= SDHCI_INT_CARD_INT;
			result = IRQ_WAKE_THREAD;
		}

		intmask &= ~(SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE |
			     SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK |
			     SDHCI_INT_ERROR | SDHCI_INT_BUS_POWER |
			     SDHCI_INT_CARD_INT);

		if (intmask) {
			unexpected |= intmask;
			bcm2835_mmc_writel(host, intmask, SDHCI_INT_STATUS, 9);
		}

		if (result == IRQ_NONE)
			result = IRQ_HANDLED;

		intmask = bcm2835_mmc_readl(host, SDHCI_INT_STATUS);
	} while (intmask && --max_loops);
out:
	spin_unlock(&host->lock);

	if (unexpected) {
		pr_err("%s: Unexpected interrupt 0x%08x.\n",
			   mmc_hostname(host->mmc), unexpected);
		bcm2835_mmc_dumpregs(host);
	}

	return result;
}

static irqreturn_t bcm2835_mmc_thread_irq(int irq, void *dev_id)
{
	struct bcm2835_host *host = dev_id;
	unsigned long flags;
	u32 isr;

	spin_lock_irqsave(&host->lock, flags);
	isr = host->thread_isr;
	host->thread_isr = 0;
	spin_unlock_irqrestore(&host->lock, flags);

	if (isr & SDHCI_INT_CARD_INT) {
		sdio_run_irqs(host->mmc);

		spin_lock_irqsave(&host->lock, flags);
		if (host->flags & SDHCI_SDIO_IRQ_ENABLED)
			bcm2835_mmc_enable_sdio_irq_nolock(host, true);
		spin_unlock_irqrestore(&host->lock, flags);
	}

	return isr ? IRQ_HANDLED : IRQ_NONE;
}



void bcm2835_mmc_set_clock(struct bcm2835_host *host, unsigned int clock)
{
	int div = 0; /* Initialized for compiler warning */
	int real_div = div, clk_mul = 1;
	u16 clk = 0;
	unsigned long timeout;
	unsigned int input_clock = clock;

	if (host->overclock_50 && (clock == 50000000))
		clock = host->overclock_50 * 1000000 + 999999;

	host->mmc->actual_clock = 0;

	bcm2835_mmc_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		return;

	/* Version 3.00 divisors must be a multiple of 2. */
	if (host->max_clk <= clock)
		div = 1;
	else {
		for (div = 2; div < SDHCI_MAX_DIV_SPEC_300;
			 div += 2) {
			if ((host->max_clk / div) <= clock)
				break;
		}
	}

	real_div = div;
	div >>= 1;

	if (real_div)
		clock = (host->max_clk * clk_mul) / real_div;
	host->mmc->actual_clock = clock;

	if ((clock > input_clock) && (clock > host->max_overclock)) {
		pr_warn("%s: Overclocking to %dHz\n",
			mmc_hostname(host->mmc), clock);
		host->max_overclock = clock;
	}

	clk |= (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
	clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
		<< SDHCI_DIVIDER_HI_SHIFT;
	clk |= SDHCI_CLOCK_INT_EN;
	bcm2835_mmc_writew(host, clk, SDHCI_CLOCK_CONTROL);

	/* Wait max 20 ms */
	timeout = 20;
	while (!((clk = bcm2835_mmc_readw(host, SDHCI_CLOCK_CONTROL))
		& SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			pr_err("%s: Internal clock never "
				"stabilised.\n", mmc_hostname(host->mmc));
			bcm2835_mmc_dumpregs(host);
			return;
		}
		timeout--;
		mdelay(1);
	}

	if (20-timeout > 10 && 20-timeout > host->max_delay) {
		host->max_delay = 20-timeout;
		pr_warning("Warning: MMC controller hung for %d ms\n", host->max_delay);
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	bcm2835_mmc_writew(host, clk, SDHCI_CLOCK_CONTROL);
}

static void bcm2835_mmc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct bcm2835_host *host;
	unsigned long flags;

	host = mmc_priv(mmc);

	spin_lock_irqsave(&host->lock, flags);

	WARN_ON(host->mrq != NULL);

	host->mrq = mrq;

	if (mrq->sbc && !(host->flags & SDHCI_AUTO_CMD23))
		bcm2835_mmc_send_command(host, mrq->sbc);
	else
		bcm2835_mmc_send_command(host, mrq->cmd);

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);

	if (!(mrq->sbc && !(host->flags & SDHCI_AUTO_CMD23)) && mrq->cmd->data && host->use_dma) {
		/* DMA transfer starts now, PIO starts after interrupt */
		bcm2835_mmc_transfer_dma(host);
	}
}


static void bcm2835_mmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{

	struct bcm2835_host *host = mmc_priv(mmc);
	unsigned long flags;
	u8 ctrl;
	u16 clk, ctrl_2;

	pr_debug("bcm2835_mmc_set_ios: clock %d, pwr %d, bus_width %d, timing %d, vdd %d, drv_type %d\n",
		 ios->clock, ios->power_mode, ios->bus_width,
		 ios->timing, ios->signal_voltage, ios->drv_type);

	spin_lock_irqsave(&host->lock, flags);

	if (!ios->clock || ios->clock != host->clock) {
		bcm2835_mmc_set_clock(host, ios->clock);
		host->clock = ios->clock;
	}

	if (host->pwr != SDHCI_POWER_330) {
		host->pwr = SDHCI_POWER_330;
		bcm2835_mmc_writeb(host, SDHCI_POWER_330 | SDHCI_POWER_ON, SDHCI_POWER_CONTROL);
	}

	ctrl = bcm2835_mmc_readb(host, SDHCI_HOST_CONTROL);

	/* set bus width */
	ctrl &= ~SDHCI_CTRL_8BITBUS;
	if (ios->bus_width == MMC_BUS_WIDTH_4)
		ctrl |= SDHCI_CTRL_4BITBUS;
	else
		ctrl &= ~SDHCI_CTRL_4BITBUS;

	ctrl &= ~SDHCI_CTRL_HISPD; /* NO_HISPD_BIT */


	bcm2835_mmc_writeb(host, ctrl, SDHCI_HOST_CONTROL);
	/*
	 * We only need to set Driver Strength if the
	 * preset value enable is not set.
	 */
	ctrl_2 = bcm2835_mmc_readw(host, SDHCI_HOST_CONTROL2);
	ctrl_2 &= ~SDHCI_CTRL_DRV_TYPE_MASK;
	if (ios->drv_type == MMC_SET_DRIVER_TYPE_A)
		ctrl_2 |= SDHCI_CTRL_DRV_TYPE_A;
	else if (ios->drv_type == MMC_SET_DRIVER_TYPE_C)
		ctrl_2 |= SDHCI_CTRL_DRV_TYPE_C;

	bcm2835_mmc_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);

	/* Reset SD Clock Enable */
	clk = bcm2835_mmc_readw(host, SDHCI_CLOCK_CONTROL);
	clk &= ~SDHCI_CLOCK_CARD_EN;
	bcm2835_mmc_writew(host, clk, SDHCI_CLOCK_CONTROL);

	/* Re-enable SD Clock */
	bcm2835_mmc_set_clock(host, host->clock);
	bcm2835_mmc_writeb(host, ctrl, SDHCI_HOST_CONTROL);

	mmiowb();

	spin_unlock_irqrestore(&host->lock, flags);
}


static struct mmc_host_ops bcm2835_ops = {
	.request = bcm2835_mmc_request,
	.set_ios = bcm2835_mmc_set_ios,
	.enable_sdio_irq = bcm2835_mmc_enable_sdio_irq,
};


static void bcm2835_mmc_tasklet_finish(unsigned long param)
{
	struct bcm2835_host *host;
	unsigned long flags;
	struct mmc_request *mrq;

	host = (struct bcm2835_host *)param;

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * If this tasklet gets rescheduled while running, it will
	 * be run again afterwards but without any active request.
	 */
	if (!host->mrq) {
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	del_timer(&host->timer);

	mrq = host->mrq;

	/*
	 * The controller needs a reset of internal state machines
	 * upon error conditions.
	 */
	if (!(host->flags & SDHCI_DEVICE_DEAD) &&
	    ((mrq->cmd && mrq->cmd->error) ||
		 (mrq->data && (mrq->data->error ||
		  (mrq->data->stop && mrq->data->stop->error))))) {

		spin_unlock_irqrestore(&host->lock, flags);
		bcm2835_mmc_reset(host, SDHCI_RESET_CMD);
		bcm2835_mmc_reset(host, SDHCI_RESET_DATA);
		spin_lock_irqsave(&host->lock, flags);
	}

	host->mrq = NULL;
	host->cmd = NULL;
	host->data = NULL;

	mmiowb();

	spin_unlock_irqrestore(&host->lock, flags);
	mmc_request_done(host->mmc, mrq);
}



static int bcm2835_mmc_add_host(struct bcm2835_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct device *dev = mmc->parent;
#ifndef FORCE_PIO
	struct dma_slave_config cfg;
#endif
	int ret;

	bcm2835_mmc_reset(host, SDHCI_RESET_ALL);

	host->clk_mul = 0;

	mmc->f_max = host->max_clk;
	mmc->f_max = host->max_clk;
	mmc->f_min = host->max_clk / SDHCI_MAX_DIV_SPEC_300;

	/* SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK */
	host->timeout_clk = mmc->f_max / 1000;
	mmc->max_busy_timeout = (1 << 27) / host->timeout_clk;

	/* host controller capabilities */
	mmc->caps |= MMC_CAP_CMD23 | MMC_CAP_ERASE | MMC_CAP_NEEDS_POLL |
		MMC_CAP_SDIO_IRQ | MMC_CAP_SD_HIGHSPEED |
		MMC_CAP_MMC_HIGHSPEED | MMC_CAP_4_BIT_DATA;

	mmc->caps2 |= MMC_CAP2_SDIO_IRQ_NOTHREAD;

	host->flags = SDHCI_AUTO_CMD23;

	dev_info(dev, "mmc_debug:%x mmc_debug2:%x\n", mmc_debug, mmc_debug2);
#ifdef FORCE_PIO
	dev_info(dev, "Forcing PIO mode\n");
	host->have_dma = false;
#else
	if (IS_ERR_OR_NULL(host->dma_chan_tx) ||
	    IS_ERR_OR_NULL(host->dma_chan_rx)) {
		dev_err(dev, "%s: Unable to initialise DMA channels. Falling back to PIO\n",
			DRIVER_NAME);
		host->have_dma = false;
	} else {
		dev_info(dev, "DMA channels allocated");
		host->have_dma = true;

		cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		cfg.slave_id = 11;		/* DREQ channel */

		cfg.direction = DMA_MEM_TO_DEV;
		cfg.src_addr = 0;
		cfg.dst_addr = host->phys_addr + SDHCI_BUFFER;
		ret = dmaengine_slave_config(host->dma_chan_tx, &cfg);

		cfg.direction = DMA_DEV_TO_MEM;
		cfg.src_addr = host->phys_addr + SDHCI_BUFFER;
		cfg.dst_addr = 0;
		ret = dmaengine_slave_config(host->dma_chan_rx, &cfg);
	}
#endif
	mmc->max_segs = 128;
	mmc->max_req_size = 524288;
	mmc->max_seg_size = mmc->max_req_size;
	mmc->max_blk_size = 512;
	mmc->max_blk_count =  65535;

	/* report supported voltage ranges */
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	tasklet_init(&host->finish_tasklet,
		bcm2835_mmc_tasklet_finish, (unsigned long)host);

	setup_timer(&host->timer, bcm2835_mmc_timeout_timer, (unsigned long)host);
	init_waitqueue_head(&host->buf_ready_int);

	bcm2835_mmc_init(host, 0);
	ret = devm_request_threaded_irq(dev, host->irq, bcm2835_mmc_irq,
					bcm2835_mmc_thread_irq, IRQF_SHARED,
					mmc_hostname(mmc), host);
	if (ret) {
		dev_err(dev, "Failed to request IRQ %d: %d\n", host->irq, ret);
		goto untasklet;
	}

	mmiowb();
	mmc_add_host(mmc);

	return 0;

untasklet:
	tasklet_kill(&host->finish_tasklet);

	return ret;
}

static int bcm2835_mmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct clk *clk;
	struct resource *iomem;
	struct bcm2835_host *host;
	struct mmc_host *mmc;
	int ret;

	mmc = mmc_alloc_host(sizeof(*host), dev);
	if (!mmc)
		return -ENOMEM;

	mmc->ops = &bcm2835_ops;
	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->timeout = msecs_to_jiffies(1000);
	spin_lock_init(&host->lock);

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	host->ioaddr = devm_ioremap_resource(dev, iomem);
	if (IS_ERR(host->ioaddr)) {
		ret = PTR_ERR(host->ioaddr);
		goto err;
	}

	host->phys_addr = iomem->start + BCM2835_VCMMU_SHIFT;

#ifndef FORCE_PIO
	if (node) {
		host->dma_chan_tx = dma_request_slave_channel(dev, "tx");
		host->dma_chan_rx = dma_request_slave_channel(dev, "rx");
	} else {
		dma_cap_mask_t mask;

		dma_cap_zero(mask);
		/* we don't care about the channel, any would work */
		dma_cap_set(DMA_SLAVE, mask);
		host->dma_chan_tx = dma_request_channel(mask, NULL, NULL);
		host->dma_chan_rx = dma_request_channel(mask, NULL, NULL);
	}
#endif
	clk = devm_clk_get(dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(dev, "could not get clk\n");
		ret = PTR_ERR(clk);
		goto err;
	}

	host->max_clk = clk_get_rate(clk);

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq <= 0) {
		dev_err(dev, "get IRQ failed\n");
		ret = -EINVAL;
		goto err;
	}

	if (node) {
		mmc_of_parse(mmc);

		/* Read any custom properties */
		of_property_read_u32(node,
				     "brcm,overclock-50",
				     &host->overclock_50);
	} else {
		mmc->caps |= MMC_CAP_4_BIT_DATA;
	}

	ret = bcm2835_mmc_add_host(host);
	if (ret)
		goto err;

	platform_set_drvdata(pdev, host);

	return 0;
err:
	mmc_free_host(mmc);

	return ret;
}

static int bcm2835_mmc_remove(struct platform_device *pdev)
{
	struct bcm2835_host *host = platform_get_drvdata(pdev);
	unsigned long flags;
	int dead;
	u32 scratch;

	dead = 0;
	scratch = bcm2835_mmc_readl(host, SDHCI_INT_STATUS);
	if (scratch == (u32)-1)
		dead = 1;


	if (dead) {
		spin_lock_irqsave(&host->lock, flags);

		host->flags |= SDHCI_DEVICE_DEAD;

		if (host->mrq) {
			pr_err("%s: Controller removed during "
				" transfer!\n", mmc_hostname(host->mmc));

			host->mrq->cmd->error = -ENOMEDIUM;
			tasklet_schedule(&host->finish_tasklet);
		}

		spin_unlock_irqrestore(&host->lock, flags);
	}

	mmc_remove_host(host->mmc);

	if (!dead)
		bcm2835_mmc_reset(host, SDHCI_RESET_ALL);

	free_irq(host->irq, host);

	del_timer_sync(&host->timer);

	tasklet_kill(&host->finish_tasklet);

	mmc_free_host(host->mmc);
	platform_set_drvdata(pdev, NULL);

	return 0;
}


static const struct of_device_id bcm2835_mmc_match[] = {
	{ .compatible = "brcm,bcm2835-mmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm2835_mmc_match);



static struct platform_driver bcm2835_mmc_driver = {
	.probe      = bcm2835_mmc_probe,
	.remove     = bcm2835_mmc_remove,
	.driver     = {
		.name		= DRIVER_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= bcm2835_mmc_match,
	},
};
module_platform_driver(bcm2835_mmc_driver);

module_param(mmc_debug, uint, 0644);
module_param(mmc_debug2, uint, 0644);
MODULE_ALIAS("platform:mmc-bcm2835");
MODULE_DESCRIPTION("BCM2835 SDHCI driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Gellert Weisz");
