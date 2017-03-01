/*
 *
 * MNH State Manager HOST Driver
 * Copyright (c) 2016, Intel Corporation.
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
 */

#define DEBUG

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "hw-mnh-regs.h"
#include "mnh-clk.h"
#include "mnh-ddr.h"
#include "mnh-mipi.h"
#include "mnh-pcie.h"
#include "mnh-pwr.h"
#include "mnh-sm.h"

#define MAX_STR_COPY 32
#define SUCCESS 0
#define DEVICE_NAME "mnh_sm"

#define SGL_SIZE 64

#define INIT_DONE 0x1

/* Firmware download address */
/* #define HW_MNH_SBL_DOWNLOAD		0x40000000 */
#define HW_MNH_SBL_DOWNLOAD		0x00101000 /* push directly to sram */
#define HW_MNH_SBL_DOWNLOAD_EXE		0x00101000
#define HW_MNH_UBOOT_DOWNLOAD		0x40020000
#define HW_MNH_KERNEL_DOWNLOAD		0x40080000
#define HW_MNH_DT_DOWNLOAD		0x40880000
#define HW_MNH_RAMDISK_DOWNLOAD		0x40890000
#define IMG_DOWNLOAD_MAX_SIZE		(2000 * 1024)
#define FIP_IMG_SBL_SIZE_OFFSET		0x28
#define FIP_IMG_SBL_ADDR_OFFSET		0x20
#define FIP_IMG_UBOOT_SIZE_OFFSET	0x50
#define FIP_IMG_UBOOT_ADDR_OFFSET	0x48

/* PCIe */
#define MNH_PCIE_CHAN_0 0

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define MNH_UBOOT_ENABLE 1
#define MNH_UBOOT_DISABLE 0

enum fw_image_state {
	FW_IMAGE_NONE = 0,
	FW_IMAGE_DOWNLOADING,
	FW_IMAGE_DOWNLOAD_SUCCESS,
	FW_IMAGE_DOWNLOAD_FAIL
};

struct mnh_sm_device {
	struct platform_device *pdev;
	struct device *dev;
	struct cdev cdev;
	dev_t dev_num;
	struct device *chardev;
	struct class *dev_class;
	int open;
	enum fw_image_state image_loaded;

	/* kernel thread for state transitions from ioctls */
	struct work_struct set_state_work;
	int next_mnh_state;
};

static struct mnh_mipi_config mnh_mipi_configs[] = {
	{
		.txdev = MNH_MUX_DEVICE_TX0,
		.rxdev = MNH_MUX_DEVICE_RX0,
		.rx_rate = 1296,
		.tx_rate = 1296,
		.vc_en_mask = MNH_MIPI_VC_ALL_EN_MASK,
		.is_gen3 = 1,
	},
	{
		.txdev = MNH_MUX_DEVICE_TX1,
		.rxdev = MNH_MUX_DEVICE_RX1,
		.rx_rate = 648,
		.tx_rate = 1296,
		.vc_en_mask = MNH_MIPI_VC_ALL_EN_MASK,
		.is_gen3 = 1,
	},
};

static struct mnh_sm_device *mnh_sm_dev;
static int mnh_state;
static int mnh_sm_uboot = MNH_UBOOT_DISABLE;

/* callback when easel enters and leaves the active state */
static hotplug_cb_t mnh_hotplug_cb;

static int mnh_sm_get_val_from_buf(const char *buf, unsigned long *val)
{
	uint8_t *token;
	const char *delim = ";";

	token = strsep((char **)&buf, delim);
	if ((token) && (!(kstrtoul(token, 0, val))))
		return 0;
	else
		return -EINVAL;
}

static ssize_t mnh_sm_poweron_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;

	dev_info(dev, "Entering mnh_sm_poweron_show...\n");
	mnh_sm_set_state(MNH_STATE_INIT);
	return strlen;
}

static ssize_t mnh_sm_poweron_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	dev_info(dev, "Entering mnh_sm_poweron_store...\n");
	return -EINVAL;
}

static DEVICE_ATTR(poweron, S_IWUSR | S_IRUSR | S_IRGRP,
		mnh_sm_poweron_show, mnh_sm_poweron_store);

static ssize_t mnh_sm_poweroff_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;

	dev_info(dev, "Entering mnh_sm_poweroff_show...\n");
	mnh_sm_set_state(MNH_STATE_OFF);

	return strlen;
}

static ssize_t mnh_sm_poweroff_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	dev_info(dev, "Entering mnh_sm_poweroff_store...\n");

	return -EINVAL;
}

static DEVICE_ATTR(poweroff, S_IWUSR | S_IRUSR | S_IRGRP,
		mnh_sm_poweroff_show, mnh_sm_poweroff_store);


static ssize_t mnh_sm_config_mipi_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;

	dev_info(dev, "Entering mnh_sm_config_mipi_show...\n");
	mnh_sm_set_state(MNH_STATE_CONFIG_MIPI);
	return strlen;
}

static DEVICE_ATTR(config_mipi, S_IRUGO,
		mnh_sm_config_mipi_show, NULL);

static ssize_t mnh_sm_config_ddr_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;

	dev_info(dev, "Entering mnh_sm_config_ddr_show...\n");
	mnh_sm_set_state(MNH_STATE_CONFIG_DDR);
	return strlen;
}

static DEVICE_ATTR(config_ddr, S_IRUGO,
		mnh_sm_config_ddr_show, NULL);


static ssize_t mnh_sm_config_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;

	dev_info(dev, "Entering mnh_sm_config_show...\n");
	mnh_sm_set_state(MNH_STATE_CONFIG_MIPI);
	mnh_sm_set_state(MNH_STATE_CONFIG_DDR);
	return strlen;
}

static DEVICE_ATTR(config, S_IRUGO,
		mnh_sm_config_show, NULL);


static ssize_t mnh_sm_state_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	dev_info(dev, "Entering mnh_sm_state_show...\n");

	return scnprintf(buf, MAX_STR_COPY, "0x%x\n", mnh_state);
}

static ssize_t mnh_sm_state_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	unsigned long val = 0;
	int ret;

	dev_info(dev, "Entering mnh_sm_state_store...\n");

	ret = mnh_sm_get_val_from_buf(buf, &val);
	return mnh_sm_set_state((int)val);
}

static DEVICE_ATTR(state, S_IWUSR | S_IRUGO,
		mnh_sm_state_show, mnh_sm_state_store);

static int dma_callback(uint8_t chan, enum mnh_dma_chan_dir_t dir,
		enum mnh_dma_trans_status_t status)
{
	dev_info(mnh_sm_dev->dev, "DMA_CALLBACK: ch:%d, dir:%s, status:%s\n",
		 chan, (dir == DMA_AP2EP)?"READ(AP2EP)":"WRITE(EP2AP)",
		 (status == DMA_DONE)?"DONE":"ABORT");


	if (chan == MNH_PCIE_CHAN_0 && dir == DMA_AP2EP) {
		if (mnh_sm_dev->image_loaded == FW_IMAGE_DOWNLOADING) {
			if (status == DMA_DONE)
				mnh_sm_dev->image_loaded =
				    FW_IMAGE_DOWNLOAD_SUCCESS;
			else if (status == DMA_ABORT)
				mnh_sm_dev->image_loaded =
				    FW_IMAGE_DOWNLOAD_FAIL;
		}
	} else {
		dev_err(mnh_sm_dev->dev, "DMA_CALLBACK: incorrect channel and direction");
		return -EINVAL;
	}

	return 0;
}

static int mnh_firmware_waitdownloaded(void)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(5000);

	do {
		if (mnh_sm_dev->image_loaded == FW_IMAGE_DOWNLOAD_SUCCESS) {
			dev_info(mnh_sm_dev->dev, "Firmware loaded!\n");
			return 0;
		} else if (mnh_sm_dev->image_loaded == FW_IMAGE_DOWNLOAD_FAIL) {
			dev_err(mnh_sm_dev->dev, "Firmware load fail!\n");
			return -EIO;
		}
		msleep(20);
	} while (time_before(jiffies, timeout));

	dev_err(mnh_sm_dev->dev, "Fail to Download Firmware, timeout!!\n");
	return -EIO;
}

int mnh_transfer_firmware(size_t fw_size, const uint8_t *fw_data,
	uint64_t dst_addr)
{
	uint32_t *buf = NULL;
	struct mnh_dma_element_t dma_blk;
	int err = -EINVAL;
	size_t sent = 0, size, remaining;

	remaining = fw_size;

	while (remaining > 0) {
		size = MIN(remaining, IMG_DOWNLOAD_MAX_SIZE);

		buf = kmalloc(size, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		memcpy(buf, fw_data + sent, size);

		dma_blk.dst_addr = dst_addr + sent;
		dma_blk.len = size;
		dma_blk.src_addr = mnh_map_mem(buf, size, DMA_TO_DEVICE);

		if (!dma_blk.src_addr) {
			dev_err(mnh_sm_dev->dev,
				"Could not map dma buffer for FW download\n");
			kfree(buf);
			return -ENOMEM;
		}

		dev_info(mnh_sm_dev->dev, "FW download - AP(:0x%llx) to EP(:0x%llx), size(%d)\n",
			 dma_blk.src_addr, dma_blk.dst_addr, dma_blk.len);

		mnh_sm_dev->image_loaded = FW_IMAGE_DOWNLOADING;
		mnh_dma_sblk_start(MNH_PCIE_CHAN_0, DMA_AP2EP, &dma_blk);

		sent += size;
		remaining -= size;
		dev_info(mnh_sm_dev->dev, "Sent:%zd, Remaining:%zd\n",
			 sent, remaining);

		err = mnh_firmware_waitdownloaded();
		mnh_unmap_mem(dma_blk.src_addr, size, DMA_TO_DEVICE);
		kfree(buf);

		if (err)
			break;
	}

	return err;

}

int mnh_download_firmware(void)
{
	const struct firmware *dt_img, *kernel_img, *ram_img;
	const struct firmware *fip_img;
	int err;
	uint32_t size, addr;

	mnh_sm_dev->image_loaded = FW_IMAGE_NONE;

	/* Register DMA callback */
	err = mnh_reg_irq_callback(0, 0, dma_callback);
	if (err) {
		dev_err(mnh_sm_dev->dev, "register irq callback failed - %d\n",
			err);
		return err;
	}

	err = request_firmware(&fip_img, "easel/fip.bin", mnh_sm_dev->dev);
	if (err) {
		dev_err(mnh_sm_dev->dev, "request fip_image failed - %d\n",
			err);
		return -EIO;
	}

	err = request_firmware(&kernel_img, "easel/Image", mnh_sm_dev->dev);
	if (err) {
		dev_err(mnh_sm_dev->dev, "request kernel failed - %d\n", err);
		goto free_uboot;
	}

	err = request_firmware(&dt_img, "easel/mnh.dtb", mnh_sm_dev->dev);
	if (err) {
		dev_err(mnh_sm_dev->dev, "request kernel failed - %d\n", err);
		goto free_kernel;
	}

	err = request_firmware(&ram_img, "easel/ramdisk.img", mnh_sm_dev->dev);
	if (err) {
		dev_err(mnh_sm_dev->dev, "request kernel failed - %d\n", err);
		goto free_dt;
	}

	/* DMA transfer for SBL */
	memcpy(&size, (uint8_t *)(fip_img->data + FIP_IMG_SBL_SIZE_OFFSET), 4);
	memcpy(&addr, (uint8_t *)(fip_img->data + FIP_IMG_SBL_ADDR_OFFSET), 4);
	dev_info(mnh_sm_dev->dev, "sbl size :0x%x", size);
	dev_info(mnh_sm_dev->dev, "sbl data addr :0x%x", addr);

	dev_info(mnh_sm_dev->dev, "DOWNLOADING SBL...size:0x%x\n", size);
	if (mnh_transfer_firmware(size, fip_img->data + addr,
			HW_MNH_SBL_DOWNLOAD))
		goto fail_downloading;
	mnh_config_write(HW_MNH_PCIE_CLUSTER_ADDR_OFFSET + HW_MNH_PCIE_GP_4, 4,
		HW_MNH_SBL_DOWNLOAD);
	mnh_config_write(HW_MNH_PCIE_CLUSTER_ADDR_OFFSET + HW_MNH_PCIE_GP_5, 4,
		HW_MNH_SBL_DOWNLOAD_EXE);
	mnh_config_write(HW_MNH_PCIE_CLUSTER_ADDR_OFFSET + HW_MNH_PCIE_GP_6, 4,
		size);

	/* DMA transfer for UBOOT */
	memcpy(&size,
		(uint8_t *)(fip_img->data + FIP_IMG_UBOOT_SIZE_OFFSET), 4);
	memcpy(&addr,
		(uint8_t *)(fip_img->data + FIP_IMG_UBOOT_ADDR_OFFSET), 4);
	dev_info(mnh_sm_dev->dev, "uboot size :0x%x", size);
	dev_info(mnh_sm_dev->dev, "uboot data addr:0x%x", addr);

	/* DMA transfer for UBOOT */
	dev_info(mnh_sm_dev->dev, "DOWNLOADING UBOOT...size:0x%x\n", size);
	if (mnh_transfer_firmware(size, fip_img->data + addr,
			HW_MNH_UBOOT_DOWNLOAD))
		goto fail_downloading;

	/* DMA transfer for device tree */
	dev_info(mnh_sm_dev->dev, "DOWNLOADING DT...size:%zd\n", dt_img->size);
	if (mnh_transfer_firmware(dt_img->size, dt_img->data,
			HW_MNH_DT_DOWNLOAD))
		goto fail_downloading;

	/* DMA transfer for ramdisk */
	dev_info(mnh_sm_dev->dev, "DOWNLOADING RAMDISK...size:%zd\n",
		 ram_img->size);
	if (mnh_transfer_firmware(ram_img->size, ram_img->data,
			HW_MNH_RAMDISK_DOWNLOAD))
		goto fail_downloading;

	/* DMA transfer for Kernel image */
	dev_info(mnh_sm_dev->dev, "DOWNLOADING KERNEL...size:%zd\n",
		 kernel_img->size);
	if (mnh_transfer_firmware(kernel_img->size, kernel_img->data,
			HW_MNH_KERNEL_DOWNLOAD))
		goto fail_downloading;

	/* PC */
	if (mnh_sm_uboot)
		mnh_config_write(
			HW_MNH_PCIE_CLUSTER_ADDR_OFFSET + HW_MNH_PCIE_GP_7, 4,
			HW_MNH_UBOOT_DOWNLOAD);
	else
		mnh_config_write(
			HW_MNH_PCIE_CLUSTER_ADDR_OFFSET + HW_MNH_PCIE_GP_7, 4,
			HW_MNH_KERNEL_DOWNLOAD);

	/* sbl needs this for its own operation and arg0 for kernel */
	mnh_config_write(HW_MNH_PCIE_CLUSTER_ADDR_OFFSET + HW_MNH_PCIE_GP_3, 4,
			 HW_MNH_DT_DOWNLOAD);

	release_firmware(fip_img);
	release_firmware(kernel_img);
	release_firmware(dt_img);
	release_firmware(ram_img);

	/* Unregister DMA callback */
	mnh_reg_irq_callback(NULL, NULL, NULL);

	return 0;

fail_downloading:
	dev_err(mnh_sm_dev->dev, "FW downloading fails\n");
	mnh_sm_dev->image_loaded = FW_IMAGE_DOWNLOAD_FAIL;
	release_firmware(ram_img);
free_dt:
	release_firmware(dt_img);
free_kernel:
	release_firmware(kernel_img);
free_uboot:
	release_firmware(fip_img);

	/* Unregister DMA callback */
	mnh_reg_irq_callback(NULL, NULL, NULL);
	return -EIO;
}

static ssize_t mnh_sm_download_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;

	dev_info(mnh_sm_dev->dev, "MNH PM mnh_pm_download_show...\n");

	mnh_sm_set_state(MNH_STATE_ACTIVE);

	return strlen;
}

static DEVICE_ATTR(download, S_IRUSR | S_IRGRP,
		mnh_sm_download_show, NULL);


static ssize_t mnh_sm_suspend_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;

	mnh_sm_set_state(MNH_STATE_SUSPEND_SELF_REFRESH);

	return strlen;
}

static ssize_t mnh_sm_suspend_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	return -EINVAL;
}

static DEVICE_ATTR(suspend, S_IWUSR | S_IRUGO,
		mnh_sm_suspend_show, mnh_sm_suspend_store);

static ssize_t mnh_sm_resume_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;

	mnh_sm_set_state(MNH_STATE_ACTIVE);

	return strlen;
}

static ssize_t mnh_sm_resume_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	return -EINVAL;
}

static DEVICE_ATTR(resume, S_IRUGO | S_IWUSR | S_IWGRP,
		mnh_sm_resume_show, mnh_sm_resume_store);

static ssize_t mnh_sm_reset_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	ssize_t strlen = 0;
	return strlen;
}

static ssize_t mnh_sm_reset_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	return -EINVAL;
}

static DEVICE_ATTR(reset, S_IRUGO | S_IWUSR | S_IWGRP,
		mnh_sm_reset_show, mnh_sm_reset_store);

static ssize_t mnh_sm_cpu_clk_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	unsigned long val = 0;
	int ret;

	dev_dbg(mnh_sm_dev->dev, "Entering mnh_sm_cpu_clk_store...\n");

	if (count > 10)
		return -EINVAL;

	ret = mnh_sm_get_val_from_buf(buf, &val);
	if (!ret && (val >= CPU_FREQ_MIN) && (val <= CPU_FREQ_MAX)) {
		mnh_sm_cpu_set_clk_freq(mnh_sm_dev->dev, val);
		return val;
	}

	return -EINVAL;
}

static DEVICE_ATTR(cpu_clk, S_IWUSR,
		NULL, mnh_sm_cpu_clk_store);

static ssize_t mnh_sm_uboot_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	dev_dbg(mnh_sm_dev->dev, "Entering mnh_sm_uboot_show...\n");

	return scnprintf(buf, MAX_STR_COPY, "uboot flag: 0x%x\n", mnh_sm_uboot);
}

static ssize_t mnh_sm_uboot_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t count)
{
	unsigned long val = 0;
	int ret;

	dev_dbg(mnh_sm_dev->dev, "Entering mnh_sm_uboot_show...\n");

	ret = mnh_sm_get_val_from_buf(buf, &val);
	if (!ret) {
		mnh_sm_uboot = val;
		return mnh_sm_uboot;
	}

	return -EINVAL;
}

static DEVICE_ATTR(uboot, S_IWUSR | S_IRUGO,
		mnh_sm_uboot_show, mnh_sm_uboot_store);

static struct attribute *mnh_sm_dev_attributes[] = {
	&dev_attr_poweron.attr,
	&dev_attr_poweroff.attr,
	&dev_attr_config_mipi.attr,
	&dev_attr_config_ddr.attr,
	&dev_attr_config.attr,
	&dev_attr_state.attr,
	&dev_attr_download.attr,
	&dev_attr_suspend.attr,
	&dev_attr_resume.attr,
	&dev_attr_reset.attr,
	&dev_attr_cpu_clk.attr,
	&dev_attr_uboot.attr,
	NULL
};

static struct attribute_group mnh_sm_group = {
	.attrs = mnh_sm_dev_attributes
};

/*******************************************************************************
 *
 *      APIs
 *
 ******************************************************************************/

/**
 * API to register hotplug callback to receive MNH up/down notifications
 * @param[in] hotplug_cb  handler for hotplug in/out events
 * @return 0
 */
int mnh_sm_reg_hotplug_callback(hotplug_cb_t hotplug_cb)
{
	mnh_hotplug_cb = hotplug_cb;
	return 0;
}
EXPORT_SYMBOL(mnh_sm_reg_hotplug_callback);

/**
 * API to initialize Power and clocks to MNH.
 * @param[in] Structure argument to configure power and clock component.
 *            This structure will be populated within the kernel module.
 * @return 0 if success or -EINVAL or -EFATAL on failure
 */
static int mnh_sm_poweron(void)
{
	int ret;

	/* Initialize MNH Power */
	ret = mnh_pwr_set_state(MNH_PWR_S0);
	if (ret) {
		dev_err(mnh_sm_dev->dev,
			"%s: failed to power on device (%d)\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

/**
 * API to power monette hill.
 * @return 0 if success or -EINVAL or -EFATAL on failure
 */
static int mnh_sm_poweroff(void)
{
	/* Power down MNH */
	mnh_pwr_set_state(MNH_PWR_S4);
	return 0;
}

/**
 * API to initialize MIPI, DDR, DDR training,
 * and PCIE.
 * @param[in] Structure argument to configure each boot component.
 *            This structure will be populated within the kernel module.
 * @return 0 if success or -EINVAL or -EFATAL on failure
 */
static int mnh_sm_config_mipi(void)
{
	/* Initialze MIPI bypass */
	/* TODO hardcode the to use the first config */
	mnh_sm_mipi_bypass_init(mnh_mipi_configs[0]);
	mnh_sm_mipi_bypass_init(mnh_mipi_configs[1]);

	return 0;
}

static int mnh_sm_config_ddr(void)
{
	/* Initialize DDR */
	mnh_ddr_po_init(mnh_sm_dev->dev);

	return 0;
}

/**
 * API to download the binary images(SBL, UBoot, Kernel, Ramdisk) for mnh.
 * The location of the binaries will be located in the AP file system.
 * @return 0 if success or -EINVAL or -EFATAL on failure
 */
static int mnh_sm_download(void)
{
	uint32_t  magic;

	if (mnh_download_firmware() == 0) {
		/*
		 * Magic number setting to notify MNH that PCIE initialization
		 * is done on Host side
		 */
		if (mnh_config_read(HW_MNH_PCIE_CLUSTER_ADDR_OFFSET +
				    HW_MNH_PCIE_GP_0,
				    sizeof(uint32_t), &magic) == SUCCESS &&
				    magic == 0) {
			mnh_config_write(HW_MNH_PCIE_CLUSTER_ADDR_OFFSET +
					 HW_MNH_PCIE_GP_0,
					 sizeof(uint32_t), INIT_DONE);

			/* Set the state to configured */
			mnh_state = MNH_STATE_ACTIVE;
		} else {
			dev_err(mnh_sm_dev->dev,
				"Read GP0 register fail or GP0 is not 0:%d",
				magic);
		}
	} else {
		dev_err(mnh_sm_dev->dev,
			"%s: firmware download failed\n", __func__);
	}

	return 0;
}

/**
 * API to put MNH in suspend state.  In suspend mode the DDR will be isolated
 * and put in self refresh while the CPU is powered down.
 * @return 0 if success or -EINVAL or -EFATAL on failure
 */
static int mnh_sm_suspend(void)
{
	/* Suspend MNH power */
	mnh_pwr_set_state(MNH_PWR_S3);
	return 0;
}

/**
 * API to put MNH into active state.
 * The resume call flow should be similar to normal bootflow except for DDR
 * initializations. Since the binaries are already saved on the DDR while MNH
 * is in suspend, ESM will not need to download the binaries again during
 * resume.
 * @return 0 if success or -EINVAL or -EFATAL on failure
 */
static int mnh_sm_resume(void)
{
	/* Initialize MNH Power */
	mnh_pwr_set_state(MNH_PWR_S0);

	return 0;
}

/**
 * API to obtain the state of monette hill.
 * @return the power states of mnh
 */
int mnh_sm_get_state(void)
{
	return mnh_state;
}
EXPORT_SYMBOL(mnh_sm_get_state);

/**
 * API to set the state of monette hill.
 * @param[in] Set the power states of mnh
 */
int mnh_sm_set_state(int state)
{
	if (!mnh_sm_dev)
		return -ENODEV;

	dev_info(mnh_sm_dev->dev,
		 "%s: request state %d\n", __func__, state);

	if (state == mnh_state) {
		dev_info(mnh_sm_dev->dev,
			 "%s: already in state %d\n", __func__, state);
		return 0;
	}

	switch (state) {
	case MNH_STATE_OFF:
		mnh_sm_poweroff();
		break;
	case MNH_STATE_INIT:
		mnh_sm_set_state(MNH_STATE_OFF);
		mnh_sm_poweron();
		break;
	case MNH_STATE_CONFIG_MIPI:
		if (mnh_state != MNH_STATE_CONFIG_DDR)
			mnh_sm_set_state(MNH_STATE_INIT);
		mnh_sm_config_mipi();
		break;
	case MNH_STATE_CONFIG_DDR:
		if (mnh_state != MNH_STATE_CONFIG_MIPI)
			mnh_sm_set_state(MNH_STATE_INIT);
		mnh_sm_config_ddr();
		break;
	case MNH_STATE_ACTIVE:
		if (mnh_state == MNH_STATE_SUSPEND_SELF_REFRESH) {
			mnh_sm_resume();
		} else {
			mnh_sm_set_state(MNH_STATE_CONFIG_DDR);
			mnh_sm_download();
		}
		break;
	case MNH_STATE_SUSPEND_SELF_REFRESH:
		mnh_sm_set_state(MNH_STATE_ACTIVE);
		mnh_sm_suspend();
		break;
	case MNH_STATE_SUSPEND_HIBERNATE:
		dev_err(mnh_sm_dev->dev,
			 "%s: TODO unsupported state %d\n", __func__, state);
		break;
	default:
		dev_err(mnh_sm_dev->dev,
			 "%s: invalid state %d\n", __func__, state);
		return -EINVAL;
	}

	mnh_state = state;

	/* check for hotplug conditions */
	if (mnh_hotplug_cb && (state == MNH_STATE_ACTIVE))
		mnh_hotplug_cb(MNH_HOTPLUG_IN);
	else if (mnh_hotplug_cb && (mnh_state == MNH_STATE_ACTIVE))
		mnh_hotplug_cb(MNH_HOTPLUG_OUT);

	return 0;
}
EXPORT_SYMBOL(mnh_sm_set_state);

int mnh_sm_is_present(void)
{
	return (mnh_sm_dev != NULL) ? 1 : 0;
}
EXPORT_SYMBOL(mnh_sm_is_present);

static int mnh_sm_open(struct inode *inode, struct file *filp)
{
	struct mnh_sm_device *data = container_of(inode->i_cdev,
						  struct mnh_sm_device, cdev);

	dev_dbg(data->dev, "%s: opening mnh_sm\n", __func__);

	filp->private_data = data; /* for other methods */

	if (data->open)
		return -EBUSY;
	data->open++;

	dev_dbg(data->dev, "%s: open count %d\n", __func__, data->open);

	return 0;
}

static int mnh_sm_close(struct inode *inode, struct file *filp)
{
	struct mnh_sm_device *data = container_of(inode->i_cdev,
						  struct mnh_sm_device, cdev);

	dev_dbg(data->dev, "%s: closing mnh_sm\n", __func__);

	filp->private_data = data; /* for other methods */

	data->open--;

	dev_dbg(data->dev, "%s: open count %d\n", __func__, data->open);

	return 0;
}

static void mnh_sm_set_state_work(struct work_struct *data)
{
	mnh_sm_set_state(mnh_sm_dev->next_mnh_state);
}

static long mnh_sm_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	int err = 0;
	struct mnh_mipi_config mipi_config;

	/*
	 * the direction is a bitmask, and VERIFY_WRITE catches R/W
	 * transfers. `Type' is user-oriented, while
	 * access_ok is kernel-oriented, so the concept of "read" and
	 * "write" is reversed
	 */
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg,
				  _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err =  !access_ok(VERIFY_READ, (void __user *)arg,
				  _IOC_SIZE(cmd));
	if (err) {
		dev_err(mnh_sm_dev->dev, "%s: access error!\n", __func__);
		return -EFAULT;
	}

	/*
	 * extract the type and number bitfields, and don't decode
	 * wrong cmds: return ENOTTY (inappropriate ioctl) before access_ok(  )
	 */

	if (_IOC_TYPE(cmd) != MNH_SM_IOC_MAGIC) {
		dev_err(mnh_sm_dev->dev,
			"%s: invalid ioctl type %c (0x%08x)\n",
			__func__, _IOC_TYPE(cmd), cmd);
		return -ENOTTY;
	}

	switch (cmd) {
	case MNH_SM_IOC_POWERON:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_POWERON\n", __func__);
		mnh_sm_dev->next_mnh_state = MNH_STATE_INIT;
		schedule_work(&mnh_sm_dev->set_state_work);
		break;
	case MNH_SM_IOC_POWEROFF:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_POWEROFF\n", __func__);
		mnh_sm_dev->next_mnh_state = MNH_STATE_OFF;
		schedule_work(&mnh_sm_dev->set_state_work);
		break;
	case MNH_SM_IOC_CONFIG_MIPI:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_CONFIG_MIPI\n",
			__func__);
		err = copy_from_user(&mipi_config, (void __user *)arg,
				     sizeof(struct mnh_mipi_config));
		if (err) {
			dev_err(mnh_sm_dev->dev,
				"%s: failed to copy mipi config from userspace (%d)\n",
				__func__, err);
			return err;
		}
		mnh_sm_mipi_bypass_init(mipi_config);
		/* TODO */
		mnh_state = MNH_STATE_CONFIG_MIPI;
		break;
	case MNH_SM_IOC_CONFIG_DDR:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_CONFIG_DDR\n",
			__func__);
		mnh_sm_dev->next_mnh_state = MNH_STATE_CONFIG_DDR;
		schedule_work(&mnh_sm_dev->set_state_work);
		break;
	case MNH_SM_IOC_GET_STATE:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_GET_STATE\n",
			__func__);
		err = copy_to_user((void __user *)arg, &mnh_state,
				   sizeof(mnh_state));
		if (err) {
			dev_err(mnh_sm_dev->dev,
				"%s: failed to copy mnh_state to userspace (%d)\n",
				__func__, err);
			return err;
		}
		break;
	case MNH_SM_IOC_SET_STATE:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_SET_STATE\n",
			__func__);
		mnh_sm_dev->next_mnh_state = (int)arg;
		schedule_work(&mnh_sm_dev->set_state_work);
		break;
	case MNH_SM_IOC_DOWNLOAD:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_DOWNLOAD\n", __func__);
		mnh_sm_dev->next_mnh_state = MNH_STATE_ACTIVE;
		schedule_work(&mnh_sm_dev->set_state_work);
		break;
	case MNH_SM_IOC_SUSPEND:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_SUSPEND\n", __func__);
		mnh_sm_dev->next_mnh_state = MNH_STATE_SUSPEND_SELF_REFRESH;
		schedule_work(&mnh_sm_dev->set_state_work);
		break;
	case MNH_SM_IOC_RESUME:
		dev_dbg(mnh_sm_dev->dev, "%s: MNH_SM_IOC_RESUME\n", __func__);
		mnh_sm_dev->next_mnh_state = MNH_STATE_ACTIVE;
		schedule_work(&mnh_sm_dev->set_state_work);
		break;
	default:
		dev_err(mnh_sm_dev->dev,
			"%s: unknown ioctl %c, dir=%d, #%d (0x%08x)\n",
			__func__, _IOC_TYPE(cmd), _IOC_DIR(cmd), _IOC_NR(cmd),
			cmd);
		break;
	}

	return err;
}

static long mnh_sm_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	int ret;

	switch (_IOC_NR(cmd)) {
	case _IOC_NR(MNH_SM_IOC_CONFIG_MIPI):
	case _IOC_NR(MNH_SM_IOC_GET_STATE):
		cmd &= ~(_IOC_SIZEMASK << _IOC_SIZESHIFT);
		cmd |= sizeof(void *) << _IOC_SIZESHIFT;
		ret = mnh_sm_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
		break;
	default:
		ret = mnh_sm_ioctl(file, cmd, arg);
		break;
	}

	return ret;
}

const struct file_operations mnh_sm_fops = {
	.open = mnh_sm_open,
	.unlocked_ioctl = mnh_sm_ioctl,
	.compat_ioctl = mnh_sm_compat_ioctl,
	.release = mnh_sm_close
};

static int mnh_sm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int error = 0;

	dev_dbg(dev, "MNH SM initializing...\n");

	/* allocate device memory */
	mnh_sm_dev = devm_kzalloc(dev, sizeof(struct mnh_sm_device),
				  GFP_KERNEL);
	if (mnh_sm_dev == NULL)
		return -ENOMEM;

	/* add device data to platform device */
	mnh_sm_dev->pdev = pdev;
	mnh_sm_dev->dev = dev;
	dev_set_drvdata(dev, mnh_sm_dev);

	INIT_WORK(&mnh_sm_dev->set_state_work, mnh_sm_set_state_work);

	/* allocate character device region */
	error = alloc_chrdev_region(&mnh_sm_dev->dev_num, 0, 1, DEVICE_NAME);
	if (error < 0) {
		dev_err(dev, "alloc_chrdev_region failed (%d)\n", error);
		return error;
	}

	/* initialize cdev */
	cdev_init(&mnh_sm_dev->cdev, &mnh_sm_fops);

	/* add cdev to kernel */
	error = cdev_add(&mnh_sm_dev->cdev, mnh_sm_dev->dev_num, 1);
	if (error) {
		dev_err(dev, "cdev_add failed (%d)\n", error);
		goto fail_cdev_add;
	}

	/* Register the device class */
	mnh_sm_dev->dev_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(mnh_sm_dev->dev_class)) {
		dev_err(mnh_sm_dev->dev,
			"%s: class_create failed (%ld)\n",
			__func__, PTR_ERR(mnh_sm_dev->dev_class));
		error = PTR_ERR(mnh_sm_dev->dev_class);
		goto fail_create_class;
	}

	mnh_sm_dev->chardev =
		device_create(mnh_sm_dev->dev_class, NULL,
			      MKDEV(MAJOR(mnh_sm_dev->dev_num), 0), NULL,
			      DEVICE_NAME);
	if (IS_ERR(mnh_sm_dev->chardev)) {
		dev_err(mnh_sm_dev->dev,
			"%s: device_create failed (%ld)\n",
			__func__, PTR_ERR(mnh_sm_dev->chardev));
		error = PTR_ERR(mnh_sm_dev->chardev);
		goto fail_device_create;
	}

	dev_dbg(dev, "char driver %s added", DEVICE_NAME);

	/* create sysfs group */
	error = sysfs_create_group(&dev->kobj, &mnh_sm_group);
	if (error) {
		dev_err(dev, "failed to create sysfs group\n");
		goto fail_sysfs_create_group;
	}

	/* initialize mnh-pwr and get resources there */
	error = mnh_pwr_init(dev);
	if (error) {
		dev_err(dev, "failed to initialize mnh-pwr (%d)\n", error);
		goto fail_mnh_pwr_init;
	}

	dev_info(dev, "MNH SM initialized successfully\n");

	return 0;

fail_mnh_pwr_init:
	sysfs_remove_group(&dev->kobj, &mnh_sm_group);
fail_sysfs_create_group:
	device_destroy(mnh_sm_dev->dev_class,
		       MKDEV(MAJOR(mnh_sm_dev->dev_num), 0));
fail_device_create:
	class_destroy(mnh_sm_dev->dev_class);
fail_create_class:
	cdev_del(&mnh_sm_dev->cdev);
fail_cdev_add:
	unregister_chrdev_region(mnh_sm_dev->dev_num, 1);
	devm_kfree(dev, mnh_sm_dev);
	mnh_sm_dev = NULL;

	return error;
}

static const struct of_device_id mnh_sm_ids[] = {
	{ .compatible = "intel,mnh-sm", },
	{ },
};

MODULE_DEVICE_TABLE(of, esm_dt_ids);
static struct platform_driver mnh_sm = {
	.probe = mnh_sm_probe,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = mnh_sm_ids,
	},
};

module_platform_driver(mnh_sm);

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("MNH State Manager HOST DRIVER");
MODULE_LICENSE("GPL v2");
