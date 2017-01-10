/*
 * Android coprocessor communication hardware access functions
 *
 * Copyright 2016 Google Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* #define DEBUG */

#ifdef CONFIG_GOOGLE_EASEL_AP
#define EASELCOMM_AP 1
#else
#define EASELCOMM_EASEL 1
#endif

#ifdef EASELCOMM_EASEL
#include <linux/mnh_pcie_ep.h>
#else
#include "mnh/mnh-pcie.h"
#include "mnh/mnh-sm.h"
#include "mnh/hw-mnh-regs.h"
#endif

#include "google-easel-comm-shared.h"
#include "google-easel-comm-private.h"

#include <linux/io.h>
#include <linux/compiler.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#ifdef EASELCOMM_AP
/* Use only one DMA channel for app requests */
#define APP_DMA_CHAN 1

/*
 * Mutex serializes thread context access to app_dma* data structures below.
 * Client thread performing the app DMA transfer holds this for the duration
 * of the h/w-layer transfer operation.
 * The DMA IRQ callback accesses these without locking.
 */
static struct mutex app_dma_mutex;
/* ISR signals app DMA done */
static DECLARE_COMPLETION(app_dma_done);
/* Status of last DMA transfer */
static enum mnh_dma_trans_status_t app_dma_status;

/* MNH BAR4 window size (and addressing granularity) 4MB */
#define MNH_BAR4_MASK ((4 * 1024 * 1024) - 1)

/* DMA address of Easel cmdchan buffer */
static uint64_t easel_cmdchan_dma_addr;
/* Offset between iATU inbound 4MB-aligned window and actual buffer start */
static uint32_t easel_cmdchan_pcie_offset;
#endif

/* Common AP/Easel defines */
/* Local cmdchan buffer cpu virtual address */
static void *local_cmdchan_cpu_addr;
/* Local cmdchan buffer DMA address */
static dma_addr_t local_cmdchan_dma_addr;

/* Command channel data ready, call up to generic layer. */
static void easelcomm_hw_cmdchan_data_ready(struct work_struct *work)
{
	easelcomm_cmd_channel_data_handler();
}
static DECLARE_WORK(cmdchan_data, easelcomm_hw_cmdchan_data_ready);

/* Command channel remote wrapped received, call up to generic layer. */
static void easelcomm_hw_cmdchan_wrap(struct work_struct *work)
{
	easelcomm_cmd_channel_wrap_handler();
}
static DECLARE_WORK(cmdchan_wrap, easelcomm_hw_cmdchan_wrap);

/* AP/client cmdchan setup. */
int easelcomm_hw_ap_setup_cmdchans(void)
{
#ifdef EASELCOMM_AP
	struct mnh_inb_window inbound;
	struct mnh_outb_region outbound;
	int ret;

	ret = mnh_get_rb_base(&easel_cmdchan_dma_addr);
	if (WARN_ON(ret))
		return ret;
	pr_debug("easelcomm: cmdchans: ap virt %p dma %pad easel dma %llx",
		 local_cmdchan_cpu_addr, &local_cmdchan_dma_addr,
		 easel_cmdchan_dma_addr);

	/* Inbound region 0 BAR4 match map to Easel cmdchan phys addr */
	inbound.region = 0;
	inbound.mode = BAR_MATCH;
	inbound.bar = 4;
	/* Align target address for 4MB BAR size */
	/*
	 * TODO: Want a carveout dedicated for this so no chance of 4MB
	 * boundary crossed.  b/32782918.
	 */
	inbound.target_mth_address =
		easel_cmdchan_dma_addr & ~(MNH_BAR4_MASK);
	easel_cmdchan_pcie_offset = easel_cmdchan_dma_addr & (MNH_BAR4_MASK);
	ret = mnh_set_inbound_iatu(&inbound);
	if (WARN_ON(ret))
		return ret;

	/* Outbound region 1 map to AP cmdchan DMA addr */
	outbound.region = 1;
	outbound.base_address = HW_MNH_PCIE_OUTBOUND_BASE;
	outbound.limit_address = HW_MNH_PCIE_OUTBOUND_BASE +
		EASELCOMM_CMD_CHANNEL_SIZE - 1;
	outbound.target_pcie_address = local_cmdchan_dma_addr;
	ret = mnh_set_outbound_iatu(&outbound);
	if (WARN_ON(ret))
		return ret;
	return 0;
#else
	return -EIO;
#endif
}
EXPORT_SYMBOL(easelcomm_hw_ap_setup_cmdchans);

/*
 * Send "remote producer wrote new data to the local command channel ring
 * buffer" interrupt.
 */
int easelcomm_hw_send_data_interrupt(void)
{
	int ret;

#ifdef EASELCOMM_AP
	ret = mnh_send_irq(IRQ_MSG_SENT);
#else
	ret = mnh_send_msi(MSG_SEND_M);
#endif
	WARN_ON(ret);
	return ret;
}
EXPORT_SYMBOL(easelcomm_hw_send_data_interrupt);

/*
 * Send "remote consumer caught up with local producer and is ready to wrap the
 * remote command channel ring buffer" interrupt.
 */
int easelcomm_hw_send_wrap_interrupt(void)
{
	int ret;
#ifdef EASELCOMM_AP
	ret = mnh_send_irq(IRQ_APPDEFINED_1);
#else
	ret = mnh_send_msi(APPDEFINED_1_M);
#endif
	WARN_ON(ret);
	return ret;
}
EXPORT_SYMBOL(easelcomm_hw_send_wrap_interrupt);

#ifdef EASELCOMM_EASEL
/* EP/server MNH API IRQ callback */
static int easelcomm_hw_easel_irq_callback(struct mnh_pcie_irq *irq)
{
	if (irq->msi_irq == MSG_SEND_I)
		schedule_work(&cmdchan_data);
	else if (irq->msi_irq == APPDEFINED_1_I)
		schedule_work(&cmdchan_wrap);
	else
		pr_info("easelcomm: mnh msi %u ignored\n",
			irq->msi_irq);
	return 0;
}

/* EP/server MNH DMA IRQ callback, not currently used */
static int easelcomm_hw_easel_dma_callback(struct mnh_dma_irq *irq)
{
	return 0;
}

/*
 * Set the command channel ring buffer address in the MNH cluster register
 * that the AP/client reads from, and send the MSI informing the ap that
 * the value is setup.
 */
static int easelcomm_hw_easel_advertise_cmdchan(uint64_t buffer_dma_addr)
{
	int ret;

	pr_debug("easelcomm: advertising buffer dma=%llx\n", buffer_dma_addr);
	ret = mnh_set_rb_base(buffer_dma_addr);
	WARN_ON(ret);
	return ret;
}

/* Module init time actions for EP/server */
int easelcomm_hw_init(void)
{
	int ret;

	/* dma alloc a buffer for now, may want dedicated carveout */
	/* temporary: alloc twice as much, align to boundary */
	local_cmdchan_cpu_addr =
	    mnh_alloc_coherent(EASELCOMM_CMD_CHANNEL_SIZE,
			       &local_cmdchan_dma_addr);
	pr_debug("easelcomm: cmdchan v=%p d=%pad\n",
		 local_cmdchan_cpu_addr, &local_cmdchan_dma_addr);

	/* "PCIe" is immediately ready for server, no hotplug in/probe wait */
	ret = easelcomm_init_pcie_ready(local_cmdchan_cpu_addr);
	WARN_ON(ret);

	/* Register EP-specific IRQ callbacks */
	ret = mnh_reg_irq_callback(
		&easelcomm_hw_easel_irq_callback,
		&easelcomm_hw_easel_dma_callback);
	WARN_ON(ret);

	ret = easelcomm_hw_easel_advertise_cmdchan(
		(uint64_t)local_cmdchan_dma_addr);
	return ret;
}
EXPORT_SYMBOL(easelcomm_hw_init);
#endif

#ifdef EASELCOMM_AP
/* Called on workqueue when server sends bootstrap set MSI. */
static void easelcomm_hw_ap_server_ready(struct work_struct *work)
{
	int ret = easelcomm_client_remote_cmdchan_ready_handler();

	if (ret)
		pr_debug("easelcomm: remote_cmdchan_ready_handler returns %d\n",
			ret);
}
static DECLARE_WORK(server_cmdchan_ready, easelcomm_hw_ap_server_ready);

/* AP/client MNH API MSI callback */
static int easelcomm_hw_ap_msi_callback(uint32_t msi)
{
	pr_debug("easelcomm: msi %u\n", msi);

	switch (msi) {
	case MSI_BOOTSTRAP_SET:
		schedule_work(&server_cmdchan_ready);
		break;
	case MSI_MSG_SEND:
		schedule_work(&cmdchan_data);
		break;
	case MSI_APPDEFINED_1:
		schedule_work(&cmdchan_wrap);
		break;
	default:
		pr_warn("easelcomm: MSI %u ignored\n", msi);
	}

	return 0;
}

/* AP/client MNH API DMA IRQ callback */
static int easelcomm_hw_ap_dma_callback(
	uint8_t chan, enum mnh_dma_chan_dir_t dir,
	enum mnh_dma_trans_status_t status)
{
	pr_debug("easelcomm: DMA done chan=%u dir=%u status=%u\n",
		chan, dir, status);
	if (chan == APP_DMA_CHAN) {
		app_dma_status = status;
		complete(&app_dma_done);
	}
	return 0;
}

/* AP/client PCIe ready, EP enumerated, can now use MNH host driver. */
static int easelcomm_hw_ap_pcie_ready(void)
{
	int ret;

	local_cmdchan_cpu_addr = mnh_alloc_coherent(
		EASELCOMM_CMD_CHANNEL_SIZE, &local_cmdchan_dma_addr);

	ret = easelcomm_init_pcie_ready(local_cmdchan_cpu_addr);
	WARN_ON(ret);

	/* Register AP-specific IRQ callbacks */
	ret = mnh_reg_irq_callback(
		&easelcomm_hw_ap_msi_callback, NULL,
		&easelcomm_hw_ap_dma_callback);
	WARN_ON(ret);
	/*
	 * TODO: Easel is already booted, need to poll whether it already sent
	 * bootstrap done MSI.
	 */
	return ret;
}

/* Callback on MNH host driver hotplug in/out events. */
static int easelcomm_hw_ap_hotplug_callback(enum mnh_hotplug_event_t event)
{
	int ret = 0;

	switch (event) {
	case MNH_HOTPLUG_IN:
		pr_debug("easelcomm: mnh hotplug in\n");
		ret = easelcomm_hw_ap_pcie_ready();
	 break;
	case MNH_HOTPLUG_OUT:
		pr_debug("easelcomm: mnh hotplug out\n");
		ret = mnh_reg_irq_callback(NULL, NULL, NULL);
		WARN_ON(ret);
		break;
	default:
		break;
	}
	return ret;
}

/* Module init time actions for AP/client */
int easelcomm_hw_init(void)
{
	int ret;

	ret = mnh_sm_reg_hotplug_callback(&easelcomm_hw_ap_hotplug_callback);
	if (WARN_ON(ret))
		return ret;

	return 0;
}
EXPORT_SYMBOL(easelcomm_hw_init);
#endif

/* Read remote ringbuffer memory */
int easelcomm_hw_remote_read(
	void *local_addr, size_t len, uint64_t remote_offset)
{
	int ret;

#ifdef EASELCOMM_AP
	ret = mnh_ddr_read(easel_cmdchan_pcie_offset + (uint32_t)remote_offset,
			   len, local_addr);
#else
	ret = mnh_pcie_read(local_addr, len, remote_offset);
#endif
	WARN_ON(ret);
	return ret;
}
EXPORT_SYMBOL(easelcomm_hw_remote_read);

/* Write remote ringbuffer memory. */
int easelcomm_hw_remote_write(
	void *local_addr, size_t len, uint64_t remote_offset)
{
	int ret;

#ifdef EASELCOMM_AP
	ret = mnh_ddr_write(easel_cmdchan_pcie_offset + (uint32_t)remote_offset,
			    len, local_addr);
#else
	ret = mnh_pcie_write(local_addr, len, remote_offset);
#endif
	WARN_ON(ret);
	return ret;
}
EXPORT_SYMBOL(easelcomm_hw_remote_write);

/* Build an MNH scatter-gather list */
void *easelcomm_hw_build_scatterlist(
	void __user *buf, uint32_t buf_size, uint32_t *scatterlist_size,
	void **sglocaldata, enum easelcomm_dma_direction dma_dir)
{
	/*
	 * Allocate enough for one entry per page, perhaps needing 1 more due
	 * to crossing a page boundary, plus end of list.
	 */
	int n_ents = (buf_size / PAGE_SIZE) + 3;
	int n_ents_used = 0;
	struct mnh_sg_entry *sg_ents =
	       kcalloc(n_ents, sizeof(struct mnh_sg_entry), GFP_KERNEL);
	struct mnh_sg_list *local_sg_info;
	int ret;

	if (!sg_ents) {
		*scatterlist_size = 0;
		return NULL;
	}

	local_sg_info = kmalloc(sizeof(struct mnh_sg_list), GFP_KERNEL);
	*sglocaldata = local_sg_info;
	if (!local_sg_info) {
		kfree(sg_ents);
		*scatterlist_size = 0;
		return NULL;
	}
	ret = mnh_sg_build(buf, buf_size, sg_ents, local_sg_info, n_ents);
	if (ret < 0) {
		kfree(sg_ents);
		kfree(local_sg_info);
		*sglocaldata = NULL;
		*scatterlist_size = 0;
		return NULL;
	}

	n_ents_used = local_sg_info->length;
	*scatterlist_size = sizeof(struct mnh_sg_entry) * n_ents_used;
	return sg_ents;
}
EXPORT_SYMBOL(easelcomm_hw_build_scatterlist);

/*
 * Return the number of scatter-gather entries in the MNH SG list.  Used to
 * determine whether both sides require only 1 block and can use single-block
 * DMA for the transfer.
 */
int easelcomm_hw_scatterlist_block_count(uint32_t scatterlist_size)
{
	return scatterlist_size / sizeof(struct mnh_sg_entry);
}
EXPORT_SYMBOL(easelcomm_hw_scatterlist_block_count);

/*
 * Return the physical/DMA address of the (server-side) single entry in the
 * SG list.  Used to determine the address needed by the client side for the
 * single-block DMA transfer.  It must alreayd have been determined that the
 * SG list has only one entry via easelcomm_hw_scatterlist_block_count()
 * above.
 */
uint64_t easelcomm_hw_scatterlist_sblk_addr(void *sgent)
{
	return (uint64_t)(((struct mnh_sg_entry *)sgent)->paddr);
}
EXPORT_SYMBOL(easelcomm_hw_scatterlist_sblk_addr);

/* Destroy the MNH SG local mapping data */
void easelcomm_hw_destroy_scatterlist(void *sglocaldata)
{
	if (sglocaldata)
		mnh_sg_destroy((struct mnh_sg_list *)sglocaldata);
}
EXPORT_SYMBOL(easelcomm_hw_destroy_scatterlist);

/* Server builds an MNH DMA linked list for a multi-block transfer */
int easelcomm_hw_easel_build_ll(
	void *src_sg, void *dest_sg, void **ll_data)
{
#ifdef EASELCOMM_EASEL
	struct mnh_dma_ll *mnh_ll;
	int ret;

	*ll_data = NULL;
	mnh_ll = kmalloc(sizeof(struct mnh_dma_ll), GFP_KERNEL);
	if (!mnh_ll)
		return -ENOMEM;
	ret = mnh_ll_build(src_sg, dest_sg, mnh_ll);
	if (ret) {
		kfree(mnh_ll);
		return ret;
	}

	*ll_data = mnh_ll;
	return 0;
#else
	return -EIO;
#endif
}
EXPORT_SYMBOL(easelcomm_hw_easel_build_ll);

/*
 * Server returns Linked List DMA start address to send to client to
 * initiate a multi-block DMA transfer.
 */
uint64_t easelcomm_hw_easel_ll_addr(void *ll_data)
{
#ifdef EASELCOMM_EASEL
	return (uint64_t)((struct mnh_dma_ll *)ll_data)->dma[0];
#else
	return 0;
#endif
}
EXPORT_SYMBOL(easelcomm_hw_easel_ll_addr);

/* Server destroys an MNH DMA Linked List. */
int easelcomm_hw_easel_destroy_ll(void *ll_data)
{
#ifdef EASELCOMM_EASEL
	return mnh_ll_destroy((struct mnh_dma_ll *)ll_data);
#else
	return -EIO;
#endif
}
EXPORT_SYMBOL(easelcomm_hw_easel_destroy_ll);

/* Client performs a single-block DMA transfer */
int easelcomm_hw_ap_dma_sblk_transfer(
	uint64_t ap_daddr, uint64_t easel_daddr, uint32_t xfer_len,
	bool to_easel)
{
#ifdef EASELCOMM_AP
	enum mnh_dma_chan_dir_t dir = to_easel ? DMA_AP2EP : DMA_EP2AP;
	struct mnh_dma_element_t blk;
	int ret;

	blk.src_addr = to_easel ? ap_daddr : easel_daddr;
	blk.dst_addr = to_easel ? easel_daddr : ap_daddr;
	blk.len = xfer_len;

	mutex_lock(&app_dma_mutex);
	reinit_completion(&app_dma_done);
	ret = mnh_dma_sblk_start(APP_DMA_CHAN, dir, &blk);
	if (WARN_ON(ret)) {
		mutex_unlock(&app_dma_mutex);
		return -EIO;
	}

	ret = wait_for_completion_interruptible(&app_dma_done);
	if (WARN_ON(ret)) {
		/* Ensure DMA aborted before returning */
		ret = mnh_dma_abort(APP_DMA_CHAN, dir);
		WARN_ON(ret);
		mutex_unlock(&app_dma_mutex);
		return ret;
	}

	mutex_unlock(&app_dma_mutex);
	return app_dma_status == DMA_DONE ? 0 : -EIO;
#else
	return -EIO;
#endif
}
EXPORT_SYMBOL(easelcomm_hw_ap_dma_sblk_transfer);

/* Client performs a multi-block DMA transfer */
int easelcomm_hw_ap_dma_mblk_transfer(uint64_t ll_paddr, bool to_easel)
{
#ifdef EASELCOMM_AP
	enum mnh_dma_chan_dir_t dir = to_easel ? DMA_AP2EP : DMA_EP2AP;
	int ret;

	mutex_lock(&app_dma_mutex);
	reinit_completion(&app_dma_done);
	ret = mnh_dma_mblk_start(APP_DMA_CHAN, dir, &ll_paddr);
	if (WARN_ON(ret)) {
		mutex_unlock(&app_dma_mutex);
		return -EIO;
	}
	ret = wait_for_completion_interruptible(&app_dma_done);
	if (WARN_ON(ret)) {
		/* Ensure DMA aborted before returning */
		ret = mnh_dma_abort(APP_DMA_CHAN, dir);
		WARN_ON(ret);
		mutex_unlock(&app_dma_mutex);
		return ret;
	}
	mutex_unlock(&app_dma_mutex);
	return app_dma_status == DMA_DONE ? 0 : -EIO;
#else
	return -EIO;
#endif
}
EXPORT_SYMBOL(easelcomm_hw_ap_dma_mblk_transfer);
