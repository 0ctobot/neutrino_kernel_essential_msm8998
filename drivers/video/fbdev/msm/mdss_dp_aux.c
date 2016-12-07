/* Copyright (c) 2013, 2014, 2016 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/bug.h>
#include <linux/of_gpio.h>
#include <linux/clk/msm-clk.h>

#include "mdss_panel.h"
#include "mdss_dp.h"
#include "mdss_dp_util.h"

/*
 * edp buffer operation
 */
static char *dp_buf_init(struct edp_buf *eb, char *buf, int size)
{
	eb->start = buf;
	eb->size = size;
	eb->data = eb->start;
	eb->end = eb->start + eb->size;
	eb->len = 0;
	eb->trans_num = 0;
	eb->i2c = 0;
	return eb->data;
}

static char *dp_buf_reset(struct edp_buf *eb)
{
	eb->data = eb->start;
	eb->len = 0;
	eb->trans_num = 0;
	eb->i2c = 0;
	return eb->data;
}

static char *dp_buf_push(struct edp_buf *eb, int len)
{
	eb->data += len;
	eb->len += len;
	return eb->data;
}

static int dp_buf_trailing(struct edp_buf *eb)
{
	return (int)(eb->end - eb->data);
}

/*
 * edp aux dp_buf_add_cmd:
 * NO native and i2c command mix allowed
 */
static int dp_buf_add_cmd(struct edp_buf *eb, struct edp_cmd *cmd)
{
	char data;
	char *bp, *cp;
	int i, len;

	if (cmd->read)	/* read */
		len = 4;
	else
		len = cmd->len + 4;

	if (dp_buf_trailing(eb) < len)
		return 0;

	/*
	 * cmd fifo only has depth of 144 bytes
	 * limit buf length to 128 bytes here
	 */
	if ((eb->len + len) > 128)
		return 0;

	bp = eb->data;
	data = cmd->addr >> 16;
	data &=  0x0f;	/* 4 addr bits */
	if (cmd->read)
		data |=  BIT(4);
	*bp++ = data;
	*bp++ = cmd->addr >> 8;
	*bp++ = cmd->addr;
	*bp++ = cmd->len - 1;

	if (!cmd->read) { /* write */
		cp = cmd->datap;
		for (i = 0; i < cmd->len; i++)
			*bp++ = *cp++;
	}
	dp_buf_push(eb, len);

	if (cmd->i2c)
		eb->i2c++;

	eb->trans_num++;	/* Increase transaction number */

	return cmd->len - 1;
}

static int dp_cmd_fifo_tx(struct edp_buf *tp, unsigned char *base)
{
	u32 data;
	char *dp;
	int len, cnt;

	len = tp->len;	/* total byte to cmd fifo */
	if (len == 0)
		return 0;

	cnt = 0;
	dp = tp->start;

	while (cnt < len) {
		data = *dp; /* data byte */
		data <<= 8;
		data &= 0x00ff00; /* index = 0, write */
		if (cnt == 0)
			data |= BIT(31);  /* INDEX_WRITE */
		pr_debug("data=%x\n", data);
		dp_write(base + DP_AUX_DATA, data);
		cnt++;
		dp++;
	}

	data = (tp->trans_num - 1);
	if (tp->i2c)
		data |= BIT(8); /* I2C */

	data |= BIT(9); /* GO */
	pr_debug("data=%x\n", data);
	dp_write(base + DP_AUX_TRANS_CTRL, data);

	return tp->len;
}

static int dp_cmd_fifo_rx(struct edp_buf *rp, int len, unsigned char *base)
{
	u32 data;
	char *dp;
	int i;

	data = 0; /* index = 0 */
	data |= BIT(31);  /* INDEX_WRITE */
	data |= BIT(0);	/* read */
	dp_write(base + DP_AUX_DATA, data);

	dp = rp->data;

	/* discard first byte */
	data = dp_read(base + DP_AUX_DATA);
	for (i = 0; i < len; i++) {
		data = dp_read(base + DP_AUX_DATA);
		pr_debug("data=%x\n", data);
		*dp++ = (char)((data >> 8) & 0xff);
	}

	rp->len = len;
	return len;
}

static int dp_aux_write_cmds(struct mdss_dp_drv_pdata *ep,
					struct edp_cmd *cmd)
{
	struct edp_cmd *cm;
	struct edp_buf *tp;
	int len, ret;

	mutex_lock(&ep->aux_mutex);
	ep->aux_cmd_busy = 1;

	tp = &ep->txp;
	dp_buf_reset(tp);

	cm = cmd;
	while (cm) {
		pr_debug("i2c=%d read=%d addr=%x len=%d next=%d\n",
			cm->i2c, cm->read, cm->addr, cm->len,
			cm->next);
		ret = dp_buf_add_cmd(tp, cm);
		if (ret <= 0)
			break;
		if (cm->next == 0)
			break;
		cm++;
	}

	if (tp->i2c)
		ep->aux_cmd_i2c = 1;
	else
		ep->aux_cmd_i2c = 0;

	reinit_completion(&ep->aux_comp);

	len = dp_cmd_fifo_tx(&ep->txp, ep->base);

	wait_for_completion(&ep->aux_comp);

	if (ep->aux_error_num == EDP_AUX_ERR_NONE)
		ret = len;
	else
		ret = ep->aux_error_num;

	ep->aux_cmd_busy = 0;
	mutex_unlock(&ep->aux_mutex);
	return  ret;
}

int dp_aux_write(void *ep, struct edp_cmd *cmd)
{
	int rc = dp_aux_write_cmds(ep, cmd);

	return rc < 0 ? -EINVAL : 0;
}

static int dp_aux_read_cmds(struct mdss_dp_drv_pdata *ep,
				struct edp_cmd *cmds)
{
	struct edp_cmd *cm;
	struct edp_buf *tp;
	struct edp_buf *rp;
	int len, ret;

	mutex_lock(&ep->aux_mutex);
	ep->aux_cmd_busy = 1;

	tp = &ep->txp;
	rp = &ep->rxp;
	dp_buf_reset(tp);
	dp_buf_reset(rp);

	cm = cmds;
	len = 0;
	while (cm) {
		pr_debug("i2c=%d read=%d addr=%x len=%d next=%d\n",
			cm->i2c, cm->read, cm->addr, cm->len,
			cm->next);
		ret = dp_buf_add_cmd(tp, cm);
		len += cm->len;
		if (ret <= 0)
			break;
		if (cm->next == 0)
			break;
		cm++;
	}

	if (tp->i2c)
		ep->aux_cmd_i2c = 1;
	else
		ep->aux_cmd_i2c = 0;

	reinit_completion(&ep->aux_comp);

	dp_cmd_fifo_tx(tp, ep->base);

	wait_for_completion(&ep->aux_comp);

	if (ep->aux_error_num == EDP_AUX_ERR_NONE) {
		ret = dp_cmd_fifo_rx(rp, len, ep->base);

		if (cmds->out_buf)
			memcpy(cmds->out_buf, rp->data, cmds->len);

	} else {
		ret = ep->aux_error_num;
	}

	ep->aux_cmd_busy = 0;
	mutex_unlock(&ep->aux_mutex);

	return ret;
}

int dp_aux_read(void *ep, struct edp_cmd *cmds)
{
	int rc = dp_aux_read_cmds(ep, cmds);

	return rc  < 0 ? -EINVAL : 0;
}

void dp_aux_native_handler(struct mdss_dp_drv_pdata *ep, u32 isr)
{

	pr_debug("isr=%x\n", isr);

	if (isr & EDP_INTR_AUX_I2C_DONE)
		ep->aux_error_num = EDP_AUX_ERR_NONE;
	else if (isr & EDP_INTR_WRONG_ADDR)
		ep->aux_error_num = EDP_AUX_ERR_ADDR;
	else if (isr & EDP_INTR_TIMEOUT)
		ep->aux_error_num = EDP_AUX_ERR_TOUT;
	if (isr & EDP_INTR_NACK_DEFER)
		ep->aux_error_num = EDP_AUX_ERR_NACK;

	complete(&ep->aux_comp);
}

void dp_aux_i2c_handler(struct mdss_dp_drv_pdata *ep, u32 isr)
{

	pr_debug("isr=%x\n", isr);

	if (isr & EDP_INTR_AUX_I2C_DONE) {
		if (isr & (EDP_INTR_I2C_NACK | EDP_INTR_I2C_DEFER))
			ep->aux_error_num = EDP_AUX_ERR_NACK;
		else
			ep->aux_error_num = EDP_AUX_ERR_NONE;
	} else {
		if (isr & EDP_INTR_WRONG_ADDR)
			ep->aux_error_num = EDP_AUX_ERR_ADDR;
		else if (isr & EDP_INTR_TIMEOUT)
			ep->aux_error_num = EDP_AUX_ERR_TOUT;
		if (isr & EDP_INTR_NACK_DEFER)
			ep->aux_error_num = EDP_AUX_ERR_NACK;
		if (isr & EDP_INTR_I2C_NACK)
			ep->aux_error_num = EDP_AUX_ERR_NACK;
		if (isr & EDP_INTR_I2C_DEFER)
			ep->aux_error_num = EDP_AUX_ERR_NACK;
	}

	complete(&ep->aux_comp);
}

static int dp_aux_write_buf(struct mdss_dp_drv_pdata *ep, u32 addr,
				char *buf, int len, int i2c)
{
	struct edp_cmd	cmd;

	cmd.read = 0;
	cmd.i2c = i2c;
	cmd.addr = addr;
	cmd.datap = buf;
	cmd.len = len & 0x0ff;
	cmd.next = 0;

	return dp_aux_write_cmds(ep, &cmd);
}

static int dp_aux_read_buf(struct mdss_dp_drv_pdata *ep, u32 addr,
				int len, int i2c)
{
	struct edp_cmd cmd = {0};

	cmd.read = 1;
	cmd.i2c = i2c;
	cmd.addr = addr;
	cmd.datap = NULL;
	cmd.len = len & 0x0ff;
	cmd.next = 0;

	return dp_aux_read_cmds(ep, &cmd);
}

/*
 * edid standard header bytes
 */
static u8 edid_hdr[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

static bool dp_edid_is_valid_header(u8 *buf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(edid_hdr); i++) {
		if (buf[i] != edid_hdr[i])
			return false;
	}

	return true;
}

int dp_edid_buf_error(char *buf, int len)
{
	char *bp;
	int i;
	char csum = 0;

	bp = buf;
	if (len < 128) {
		pr_err("Error: len=%x\n", len);
		return -EINVAL;
	}

	for (i = 0; i < 128; i++)
		csum += *bp++;

	if (csum != 0) {
		pr_err("Error: csum=%x\n", csum);
		return -EINVAL;
	}

	return 0;
}


void dp_extract_edid_manufacturer(struct edp_edid *edid, char *buf)
{
	char *bp;
	char data;

	bp = &buf[8];
	data = *bp & 0x7f;
	data >>= 2;
	edid->id_name[0] = 'A' + data - 1;
	data = *bp & 0x03;
	data <<= 3;
	bp++;
	data |= (*bp >> 5);
	edid->id_name[1] = 'A' + data - 1;
	data = *bp & 0x1f;
	edid->id_name[2] = 'A' + data - 1;
	edid->id_name[3] = 0;

	pr_debug("edid manufacturer = %s\n", edid->id_name);
}

void dp_extract_edid_product(struct edp_edid *edid, char *buf)
{
	char *bp;
	u32 data;

	bp = &buf[0x0a];
	data =  *bp;
	edid->id_product = *bp++;
	edid->id_product &= 0x0ff;
	data = *bp & 0x0ff;
	data <<= 8;
	edid->id_product |= data;

	pr_debug("edid product = 0x%x\n", edid->id_product);
};

void dp_extract_edid_version(struct edp_edid *edid, char *buf)
{
	edid->version = buf[0x12];
	edid->revision = buf[0x13];
	pr_debug("edid version = %d.%d\n", edid->version,
			edid->revision);
};

void dp_extract_edid_ext_block_cnt(struct edp_edid *edid, char *buf)
{
	edid->ext_block_cnt = buf[0x7e];
	pr_debug("edid extension = %d\n",
			edid->ext_block_cnt);
};

void dp_extract_edid_video_support(struct edp_edid *edid, char *buf)
{
	char *bp;

	bp = &buf[0x14];
	if (*bp & 0x80) {
		edid->video_intf = *bp & 0x0f;
		/* 6, 8, 10, 12, 14 and 16 bit per component */
		edid->color_depth = ((*bp & 0x70) >> 4); /* color bit depth */
		if (edid->color_depth) {
			edid->color_depth *= 2;
			edid->color_depth += 4;
		}
		pr_debug("Digital Video intf=%d color_depth=%d\n",
			 edid->video_intf, edid->color_depth);
	} else {
		pr_err("Error, Analog video interface\n");
	}
};

void dp_extract_edid_feature(struct edp_edid *edid, char *buf)
{
	char *bp;
	char data;

	bp = &buf[0x18];
	data = *bp;
	data &= 0xe0;
	data >>= 5;
	if (data == 0x01)
		edid->dpm = 1; /* display power management */

	if (edid->video_intf) {
		if (*bp & 0x80) {
			/* RGB 4:4:4, YcrCb 4:4:4 and YCrCb 4:2:2 */
			edid->color_format = *bp & 0x18;
			edid->color_format >>= 3;
		}
	}

	pr_debug("edid dpm=%d color_format=%d\n",
			edid->dpm, edid->color_format);
};

char mdss_dp_gen_link_clk(struct mdss_panel_info *pinfo, char lane_cnt)
{
	const u32 encoding_factx10 = 8;
	const u32 ln_to_link_ratio = 10;
	u32 min_link_rate, reminder = 0;
	char calc_link_rate = 0;

	pr_debug("clk_rate=%llu, bpp= %d, lane_cnt=%d\n",
	       pinfo->clk_rate, pinfo->bpp, lane_cnt);

	/*
	 * The max pixel clock supported is 675Mhz. The
	 * current calculations below will make sure
	 * the min_link_rate is within 32 bit limits.
	 * Any changes in the section of code should
	 * consider this limitation.
	 */
	min_link_rate = (u32)div_u64(pinfo->clk_rate,
				(lane_cnt * encoding_factx10));
	min_link_rate /= ln_to_link_ratio;
	min_link_rate = (min_link_rate * pinfo->bpp);
	min_link_rate = (u32)div_u64_rem(min_link_rate * 10,
				DP_LINK_RATE_MULTIPLIER, &reminder);

	/*
	 * To avoid any fractional values,
	 * increment the min_link_rate
	 */
	if (reminder)
		min_link_rate += 1;
	pr_debug("min_link_rate = %d\n", min_link_rate);

	if (min_link_rate <= DP_LINK_RATE_162)
		calc_link_rate = DP_LINK_RATE_162;
	else if (min_link_rate <= DP_LINK_RATE_270)
		calc_link_rate = DP_LINK_RATE_270;
	else if (min_link_rate <= DP_LINK_RATE_540)
		calc_link_rate = DP_LINK_RATE_540;
	else {
		pr_err("link_rate = %d is unsupported\n", min_link_rate);
		calc_link_rate = 0;
	}

	return calc_link_rate;
}

void dp_extract_edid_detailed_timing_description(struct edp_edid *edid,
		char *buf)
{
	char *bp;
	u32 data;
	struct display_timing_desc *dp;

	dp = &edid->timing[0];

	bp = &buf[0x36];
	dp->pclk = 0;
	dp->pclk = *bp++; /* byte 0x36 */
	dp->pclk |= (*bp++ << 8); /* byte 0x37 */

	dp->h_addressable = *bp++; /* byte 0x38 */

	if (dp->pclk == 0 && dp->h_addressable == 0)
		return;	/* Not detailed timing definition */

	dp->pclk *= 10000;

	dp->h_blank = *bp++;/* byte 0x39 */
	data = *bp & 0xf0; /* byte 0x3A */
	data  <<= 4;
	dp->h_addressable |= data;

	data = *bp++ & 0x0f;
	data <<= 8;
	dp->h_blank |= data;

	dp->v_addressable = *bp++; /* byte 0x3B */
	dp->v_blank = *bp++; /* byte 0x3C */
	data = *bp & 0xf0; /* byte 0x3D */
	data  <<= 4;
	dp->v_addressable |= data;

	data = *bp++ & 0x0f;
	data <<= 8;
	dp->v_blank |= data;

	dp->h_fporch = *bp++; /* byte 0x3E */
	dp->h_sync_pulse = *bp++; /* byte 0x3F */

	dp->v_fporch = *bp & 0x0f0; /* byte 0x40 */
	dp->v_fporch  >>= 4;
	dp->v_sync_pulse = *bp & 0x0f;

	bp++;
	data = *bp & 0xc0; /* byte 0x41 */
	data <<= 2;
	dp->h_fporch |= data;

	data = *bp & 0x30;
	data <<= 4;
	dp->h_sync_pulse |= data;

	data = *bp & 0x0c;
	data <<= 2;
	dp->v_fporch |= data;

	data = *bp & 0x03;
	data <<= 4;
	dp->v_sync_pulse |= data;

	bp++;
	dp->width_mm = *bp++; /* byte 0x42 */
	dp->height_mm = *bp++; /* byte 0x43 */
	data = *bp & 0x0f0; /* byte 0x44 */
	data <<= 4;
	dp->width_mm |= data;
	data = *bp & 0x0f;
	data <<= 8;
	dp->height_mm |= data;

	bp++;
	dp->h_border = *bp++; /* byte 0x45 */
	dp->v_border = *bp++; /* byte 0x46 */

	/* progressive or interlaved */
	dp->interlaced = *bp & 0x80; /* byte 0x47 */

	dp->stereo = *bp & 0x60;
	dp->stereo >>= 5;

	data = *bp & 0x1e; /* bit 4,3,2 1*/
	data >>= 1;
	dp->sync_type = data & 0x08;
	dp->sync_type >>= 3;	/* analog or digital */
	if (dp->sync_type) {
		dp->sync_separate = data & 0x04;
		dp->sync_separate >>= 2;
		if (dp->sync_separate) {
			if (data & 0x02)
				dp->vsync_pol = 1; /* positive */
			else
				dp->vsync_pol = 0;/* negative */

			if (data & 0x01)
				dp->hsync_pol = 1; /* positive */
			else
				dp->hsync_pol = 0; /* negative */
		}
	}

	pr_debug("pixel_clock = %d\n", dp->pclk);

	pr_debug("horizontal=%d, blank=%d, porch=%d, sync=%d\n",
			dp->h_addressable, dp->h_blank,
			dp->h_fporch, dp->h_sync_pulse);
	pr_debug("vertical=%d, blank=%d, porch=%d, vsync=%d\n",
			dp->v_addressable, dp->v_blank,
			dp->v_fporch, dp->v_sync_pulse);
	pr_debug("panel size in mm, width=%d height=%d\n",
			dp->width_mm, dp->height_mm);
	pr_debug("panel border horizontal=%d vertical=%d\n",
				dp->h_border, dp->v_border);
	pr_debug("flags: interlaced=%d stereo=%d sync_type=%d sync_sep=%d\n",
			dp->interlaced, dp->stereo,
			dp->sync_type, dp->sync_separate);
	pr_debug("polarity vsync=%d, hsync=%d",
			dp->vsync_pol, dp->hsync_pol);
}


/*
 * EDID structure can be found in VESA standart here:
 * http://read.pudn.com/downloads110/ebook/456020/E-EDID%20Standard.pdf
 *
 * following table contains default edid
 * static char edid_raw_data[128] = {
 * 0, 255, 255, 255, 255, 255, 255, 0,
 * 6, 175, 93, 48, 0, 0, 0, 0, 0, 22,
 * 1, 4,
 * 149, 26, 14, 120, 2,
 * 164, 21,158, 85, 78, 155, 38, 15, 80, 84,
 * 0, 0, 0,
 * 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
 * 29, 54, 128, 160, 112, 56, 30, 64, 48, 32, 142, 0, 0, 144, 16,0,0,24,
 * 19, 36, 128, 160, 112, 56, 30, 64, 48, 32, 142, 0, 0, 144, 16,0,0,24,
 * 0, 0, 0, 254, 0, 65, 85, 79, 10, 32, 32, 32, 32, 32, 32, 32, 32, 32,
 * 0, 0, 0, 254, 0, 66, 49, 49, 54, 72, 65, 78, 48, 51, 46, 48, 32, 10,
 * 0, 75 };
 */

static int dp_aux_chan_ready(struct mdss_dp_drv_pdata *ep)
{
	int cnt, ret;
	char data = 0;

	for (cnt = 5; cnt; cnt--) {
		ret = dp_aux_write_buf(ep, EDID_START_ADDRESS, &data, 1, 1);
		pr_debug("ret=%d\n", ret);
		if (ret >= 0)
			break;
		msleep(100);
	}

	if (cnt <= 0) {
		pr_err("aux chan NOT ready\n");
		return -EIO;
	}

	return 0;
}

int mdss_dp_edid_read(struct mdss_dp_drv_pdata *dp)
{
	struct edp_buf *rp = &dp->rxp;
	int rlen, ret = 0;
	int edid_blk = 0, blk_num = 0, retries = 10;
	bool edid_parsing_done = false;
	const u8 cea_tag = 0x02;

	ret = dp_aux_chan_ready(dp);
	if (ret) {
		pr_err("aux chan NOT ready\n");
		return ret;
	}

	do {
		rlen = dp_aux_read_buf(dp, EDID_START_ADDRESS +
				(blk_num * EDID_BLOCK_SIZE),
				EDID_BLOCK_SIZE, 1);
		if (rlen != EDID_BLOCK_SIZE) {
			pr_err("Read failed. rlen=%d\n", rlen);
			continue;
		}

		pr_debug("blk_num=%d, rlen=%d\n", blk_num, rlen);

		if (dp_edid_is_valid_header(rp->data)) {
			if (dp_edid_buf_error(rp->data, rp->len))
				continue;

			if (edid_parsing_done) {
				blk_num++;
				continue;
			}

			dp_extract_edid_manufacturer(&dp->edid, rp->data);
			dp_extract_edid_product(&dp->edid, rp->data);
			dp_extract_edid_version(&dp->edid, rp->data);
			dp_extract_edid_ext_block_cnt(&dp->edid, rp->data);
			dp_extract_edid_video_support(&dp->edid, rp->data);
			dp_extract_edid_feature(&dp->edid, rp->data);
			dp_extract_edid_detailed_timing_description(&dp->edid,
				rp->data);

			edid_parsing_done = true;
		} else {
			edid_blk++;
			blk_num++;

			/* fix dongle byte shift issue */
			if (edid_blk == 1 && rp->data[0] != cea_tag) {
				u8 tmp[EDID_BLOCK_SIZE - 1];

				memcpy(tmp, rp->data, EDID_BLOCK_SIZE - 1);
				rp->data[0] = cea_tag;
				memcpy(rp->data + 1, tmp, EDID_BLOCK_SIZE - 1);
			}
		}

		memcpy(dp->edid_buf + (edid_blk * EDID_BLOCK_SIZE),
			rp->data, EDID_BLOCK_SIZE);

		if (edid_blk == dp->edid.ext_block_cnt)
			return 0;
	} while (retries--);

	return 0;
}

static void dp_sink_capability_read(struct mdss_dp_drv_pdata *ep,
				int len)
{
	char *bp;
	char data;
	struct dpcd_cap *cap;
	struct edp_buf *rp;
	int rlen;

	rlen = dp_aux_read_buf(ep, 0, len, 0);
	if (rlen <= 0) {
		pr_err("edp aux read failed\n");
		return;
	}
	rp = &ep->rxp;
	cap = &ep->dpcd;
	bp = rp->data;

	memset(cap, 0, sizeof(*cap));

	data = *bp++; /* byte 0 */
	cap->major = (data >> 4) & 0x0f;
	cap->minor = data & 0x0f;
	if (--rlen <= 0)
		return;
	pr_debug("version: %d.%d\n", cap->major, cap->minor);

	data = *bp++; /* byte 1 */
	/* 162, 270 and 540 MB, symbol rate, NOT bit rate */
	cap->max_link_rate = data;
	if (--rlen <= 0)
		return;
	pr_debug("link_rate=%d\n", cap->max_link_rate);

	data = *bp++; /* byte 2 */
	if (data & BIT(7))
		cap->enhanced_frame++;

	if (data & 0x40) {
		cap->flags |=  DPCD_TPS3;
		pr_debug("pattern 3 supported\n");
	} else {
		pr_debug("pattern 3 not supported\n");
	}

	data &= 0x0f;
	cap->max_lane_count = data;
	if (--rlen <= 0)
		return;
	pr_debug("lane_count=%d\n", cap->max_lane_count);

	data = *bp++; /* byte 3 */
	if (data & BIT(0)) {
		cap->flags |= DPCD_MAX_DOWNSPREAD_0_5;
		pr_debug("max_downspread\n");
	}

	if (data & BIT(6)) {
		cap->flags |= DPCD_NO_AUX_HANDSHAKE;
		pr_debug("NO Link Training\n");
	}
	if (--rlen <= 0)
		return;

	data = *bp++; /* byte 4 */
	cap->num_rx_port = (data & BIT(0)) + 1;
	pr_debug("rx_ports=%d", cap->num_rx_port);
	if (--rlen <= 0)
		return;

	data = *bp++; /* Byte 5: DOWN_STREAM_PORT_PRESENT */
	cap->downstream_port.dfp_present = data & BIT(0);
	cap->downstream_port.dfp_type = data & 0x6;
	cap->downstream_port.format_conversion = data & BIT(3);
	cap->downstream_port.detailed_cap_info_available = data & BIT(4);
	pr_debug("dfp_present = %d, dfp_type = %d\n",
			cap->downstream_port.dfp_present,
			cap->downstream_port.dfp_type);
	pr_debug("format_conversion = %d, detailed_cap_info_available = %d\n",
			cap->downstream_port.format_conversion,
			cap->downstream_port.detailed_cap_info_available);
	if (--rlen <= 0)
		return;

	bp += 1;	/* Skip Byte 6 */
	rlen -= 1;
	if (rlen <= 0)
		return;

	data = *bp++; /* Byte 7: DOWN_STREAM_PORT_COUNT */
	cap->downstream_port.dfp_count = data & 0x7;
	cap->downstream_port.msa_timing_par_ignored = data & BIT(6);
	cap->downstream_port.oui_support = data & BIT(7);
	pr_debug("dfp_count = %d, msa_timing_par_ignored = %d\n",
			cap->downstream_port.dfp_count,
			cap->downstream_port.msa_timing_par_ignored);
	pr_debug("oui_support = %d\n", cap->downstream_port.oui_support);
	if (--rlen <= 0)
		return;

	data = *bp++; /* byte 8 */
	if (data & BIT(1)) {
		cap->flags |= DPCD_PORT_0_EDID_PRESENTED;
		pr_debug("edid presented\n");
	}
	if (--rlen <= 0)
		return;

	data = *bp++; /* byte 9 */
	cap->rx_port0_buf_size = (data + 1) * 32;
	pr_debug("lane_buf_size=%d\n", cap->rx_port0_buf_size);
	if (--rlen <= 0)
		return;

	bp += 2; /* skip 10, 11 port1 capability */
	rlen -= 2;
	if (rlen <= 0)
		return;

	data = *bp++;	/* byte 12 */
	cap->i2c_speed_ctrl = data;
	if (cap->i2c_speed_ctrl > 0)
		pr_debug("i2c_rate=%d", cap->i2c_speed_ctrl);
	if (--rlen <= 0)
		return;

	data = *bp++;	/* byte 13 */
	cap->scrambler_reset = data & BIT(0);
	pr_debug("scrambler_reset=%d\n",
					cap->scrambler_reset);

	if (data & BIT(1))
		cap->enhanced_frame++;

	pr_debug("enhanced_framing=%d\n",
					cap->enhanced_frame);
	if (--rlen <= 0)
		return;

	data = *bp++; /* byte 14 */
	if (data == 0)
		cap->training_read_interval = 4000; /* us */
	else
		cap->training_read_interval = 4000 * data; /* us */
	pr_debug("training_interval=%d\n",
			 cap->training_read_interval);
}

static int dp_link_status_read(struct mdss_dp_drv_pdata *ep, int len)
{
	char *bp;
	char data;
	struct dpcd_link_status *sp;
	struct edp_buf *rp;
	int rlen;

	pr_debug("len=%d", len);
	/* skip byte 0x200 and 0x201 */
	rlen = dp_aux_read_buf(ep, 0x202, len, 0);
	if (rlen < len) {
		pr_err("edp aux read failed\n");
		return 0;
	}
	rp = &ep->rxp;
	bp = rp->data;
	sp = &ep->link_status;

	data = *bp++; /* byte 0x202 */
	sp->lane_01_status = data; /* lane 0, 1 */

	data = *bp++; /* byte 0x203 */
	sp->lane_23_status = data; /* lane 2, 3 */

	data = *bp++; /* byte 0x204 */
	sp->interlane_align_done = (data & BIT(0));
	sp->downstream_port_status_changed = (data & BIT(6));
	sp->link_status_updated = (data & BIT(7));

	data = *bp++; /* byte 0x205 */
	sp->port_0_in_sync = (data & BIT(0));
	sp->port_1_in_sync = (data & BIT(1));

	data = *bp++; /* byte 0x206 */
	sp->req_voltage_swing[0] = data & 0x03;
	data >>= 2;
	sp->req_pre_emphasis[0] = data & 0x03;
	data >>= 2;
	sp->req_voltage_swing[1] = data & 0x03;
	data >>= 2;
	sp->req_pre_emphasis[1] = data & 0x03;

	data = *bp++; /* byte 0x207 */
	sp->req_voltage_swing[2] = data & 0x03;
	data >>= 2;
	sp->req_pre_emphasis[2] = data & 0x03;
	data >>= 2;
	sp->req_voltage_swing[3] = data & 0x03;
	data >>= 2;
	sp->req_pre_emphasis[3] = data & 0x03;

	return len;
}

/**
 * mdss_dp_aux_send_test_response() - sends a test response to the sink
 * @dp: Display Port Driver data
 */
void mdss_dp_aux_send_test_response(struct mdss_dp_drv_pdata *dp)
{
	char test_response[4];

	test_response[0] = dp->test_data.response;

	pr_debug("sending test response %s",
			mdss_dp_get_test_response(test_response[0]));
	dp_aux_write_buf(dp, 0x260, test_response, 1, 0);
}

/**
 * dp_is_link_rate_valid() - validates the link rate
 * @lane_rate: link rate requested by the sink
 *
 * Returns true if the requested link rate is supported.
 */
static bool dp_is_link_rate_valid(u32 link_rate)
{
	return (link_rate == DP_LINK_RATE_162) ||
		(link_rate == DP_LINK_RATE_270) ||
		(link_rate == DP_LINK_RATE_540);
}

/**
 * dp_is_lane_count_valid() - validates the lane count
 * @lane_count: lane count requested by the sink
 *
 * Returns true if the requested lane count is supported.
 */
static bool dp_is_lane_count_valid(u32 lane_count)
{
	return (lane_count == DP_LANE_COUNT_1) ||
		(lane_count == DP_LANE_COUNT_2) ||
		(lane_count == DP_LANE_COUNT_4);
}

/**
 * dp_parse_link_training_params() - parses link training parameters from DPCD
 * @ep: Display Port Driver data
 *
 * Returns 0 if it successfully parses the link rate (Byte 0x219) and lane
 * count (Byte 0x220), and if these values parse are valid.
 */
static int dp_parse_link_training_params(struct mdss_dp_drv_pdata *ep)
{
	int ret = 0;
	char *bp;
	char data;
	struct edp_buf *rp;
	int rlen;
	int const test_parameter_len = 0x1;
	int const test_link_rate_addr = 0x219;
	int const test_lane_count_addr = 0x220;

	/* Read the requested link rate (Byte 0x219). */
	rlen = dp_aux_read_buf(ep, test_link_rate_addr,
			test_parameter_len, 0);
	if (rlen < test_parameter_len) {
		pr_err("failed to read link rate\n");
		ret = -EINVAL;
		goto exit;
	}
	rp = &ep->rxp;
	bp = rp->data;
	data = *bp++;

	if (!dp_is_link_rate_valid(data)) {
		pr_err("invalid link rate = 0x%x\n", data);
		ret = -EINVAL;
		goto exit;
	}

	ep->test_data.test_link_rate = data;
	pr_debug("link rate = 0x%x\n", ep->test_data.test_link_rate);

	/* Read the requested lane count (Byte 0x220). */
	rlen = dp_aux_read_buf(ep, test_lane_count_addr,
			test_parameter_len, 0);
	if (rlen < test_parameter_len) {
		pr_err("failed to read lane count\n");
		ret = -EINVAL;
		goto exit;
	}
	rp = &ep->rxp;
	bp = rp->data;
	data = *bp++;
	data &= 0x1F;

	if (!dp_is_lane_count_valid(data)) {
		pr_err("invalid lane count = 0x%x\n", data);
		ret = -EINVAL;
		goto exit;
	}

	ep->test_data.test_lane_count = data;
	pr_debug("lane count = 0x%x\n", ep->test_data.test_lane_count);

exit:
	return ret;
}

/**
 * dp_sink_parse_sink_count() - parses the sink count
 * @ep: Display Port Driver data
 *
 * Parses the DPCD to check if there is an update to the sink count
 * (Byte 0x200), and whether all the sink devices connected have Content
 * Protection enabled.
 */
static void dp_sink_parse_sink_count(struct mdss_dp_drv_pdata *ep)
{
	char *bp;
	char data;
	struct edp_buf *rp;
	int rlen;
	int const param_len = 0x1;
	int const sink_count_addr = 0x200;

	rlen = dp_aux_read_buf(ep, sink_count_addr, param_len, 0);
	if (rlen < param_len) {
		pr_err("failed to read sink count\n");
		return;
	}
	rp = &ep->rxp;
	bp = rp->data;
	data = *bp++;

	/* BIT 7, BIT 5:0 */
	ep->sink_count.count = (data & BIT(7)) << 6 | (data & 0x63);
	/* BIT 6*/
	ep->sink_count.cp_ready = data & BIT(6);

	pr_debug("sink_count = 0x%x, cp_ready = 0x%x\n",
			ep->sink_count.count, ep->sink_count.cp_ready);
}

/**
 * dp_is_test_supported() - checks if test requested by sink is supported
 * @test_requested: test requested by the sink
 *
 * Returns true if the requested test is supported.
 */
static bool dp_is_test_supported(u32 test_requested)
{
	return test_requested == TEST_LINK_TRAINING;
}

/**
 * dp_sink_parse_test_request() - parses test request parameters from sink
 * @ep: Display Port Driver data
 *
 * Parses the DPCD to check if an automated test is requested (Byte 0x201),
 * and what type of test automation is being requested (Byte 0x218).
 */
static void dp_sink_parse_test_request(struct mdss_dp_drv_pdata *ep)
{
	int ret = 0;
	char *bp;
	char data;
	struct edp_buf *rp;
	int rlen;
	int const test_parameter_len = 0x1;
	int const device_service_irq_addr = 0x201;
	int const test_request_addr = 0x218;

	/**
	 * Read the device service IRQ vector (Byte 0x201) to determine
	 * whether an automated test has been requested by the sink.
	 */
	rlen = dp_aux_read_buf(ep, device_service_irq_addr,
			test_parameter_len, 0);
	if (rlen < test_parameter_len) {
		pr_err("failed to read device service IRQ vector\n");
		return;
	}
	rp = &ep->rxp;
	bp = rp->data;
	data = *bp++;

	pr_debug("device service irq vector = 0x%x\n", data);

	if (!(data & BIT(1))) {
		pr_debug("no test requested\n");
		return;
	}

	/**
	 * Read the test request byte (Byte 0x218) to determine what type
	 * of automated test has been requested by the sink.
	 */
	rlen = dp_aux_read_buf(ep, test_request_addr,
			test_parameter_len, 0);
	if (rlen < test_parameter_len) {
		pr_err("failed to read test_requested\n");
		return;
	}
	rp = &ep->rxp;
	bp = rp->data;
	data = *bp++;

	if (!dp_is_test_supported(data)) {
		pr_debug("test 0x%x not supported\n", data);
		return;
	}

	pr_debug("%s requested\n", mdss_dp_get_test_name(data));
	ep->test_data.test_requested = data;

	if (ep->test_data.test_requested == TEST_LINK_TRAINING)
		ret = dp_parse_link_training_params(ep);

	/**
	 * Send a TEST_ACK if all test parameters are valid, otherwise send
	 * a TEST_NACK.
	 */
	if (ret)
		ep->test_data.response = TEST_NACK;
	else
		ep->test_data.response = TEST_ACK;
}

static int dp_cap_lane_rate_set(struct mdss_dp_drv_pdata *ep)
{
	char buf[4];
	int len = 0;
	struct dpcd_cap *cap;

	cap = &ep->dpcd;

	pr_debug("bw=%x lane=%d\n", ep->link_rate, ep->lane_cnt);
	buf[0] = ep->link_rate;
	buf[1] = ep->lane_cnt;
	if (cap->enhanced_frame)
		buf[1] |= 0x80;
	len = dp_aux_write_buf(ep, 0x100, buf, 2, 0);

	return len;
}

static int dp_lane_set_write(struct mdss_dp_drv_pdata *ep, int voltage_level,
		int pre_emphasis_level)
{
	int i;
	char buf[4];
	u32 max_level_reached = 0;

	if (voltage_level == DPCD_LINK_VOLTAGE_MAX) {
		pr_debug("max. voltage swing level reached %d\n",
				voltage_level);
		max_level_reached |= BIT(2);
	}

	if (pre_emphasis_level == DPCD_LINK_PRE_EMPHASIS_MAX) {
		pr_debug("max. pre-emphasis level reached %d\n",
				pre_emphasis_level);
		max_level_reached  |= BIT(5);
	}

	pr_debug("max_level_reached = 0x%x\n", max_level_reached);

	pre_emphasis_level <<= 3;

	for (i = 0; i < 4; i++)
		buf[i] = voltage_level | pre_emphasis_level | max_level_reached;

	pr_debug("p|v=0x%x", voltage_level | pre_emphasis_level);
	return dp_aux_write_buf(ep, 0x103, buf, 4, 0);
}

static int dp_train_pattern_set_write(struct mdss_dp_drv_pdata *ep,
						int pattern)
{
	char buf[4];

	pr_debug("pattern=%x\n", pattern);
	buf[0] = pattern;
	return dp_aux_write_buf(ep, 0x102, buf, 1, 0);
}

bool mdss_dp_aux_clock_recovery_done(struct mdss_dp_drv_pdata *ep)
{
	u32 mask;
	u32 data;

	if (ep->lane_cnt == 1) {
		mask = 0x01;	/* lane 0 */
		data = ep->link_status.lane_01_status;
	} else if (ep->lane_cnt == 2) {
		mask = 0x011; /*B lane 0, 1 */
		data = ep->link_status.lane_01_status;
	} else {
		mask = 0x01111; /*B lane 0, 1 */
		data = ep->link_status.lane_23_status;
		data <<= 8;
		data |= ep->link_status.lane_01_status;
	}

	pr_debug("data=%x mask=%x\n", data, mask);
	data &= mask;
	if (data == mask) /* all done */
		return true;

	return false;
}

bool mdss_dp_aux_channel_eq_done(struct mdss_dp_drv_pdata *ep)
{
	u32 mask;
	u32 data;

	pr_debug("Entered++\n");

	if (!ep->link_status.interlane_align_done) { /* not align */
		pr_err("interlane align failed\n");
		return 0;
	}

	if (ep->lane_cnt == 1) {
		mask = 0x7;
		data = ep->link_status.lane_01_status;
	} else if (ep->lane_cnt == 2) {
		mask = 0x77;
		data = ep->link_status.lane_01_status;
	} else {
		mask = 0x7777;
		data = ep->link_status.lane_23_status;
		data <<= 8;
		data |= ep->link_status.lane_01_status;
	}

	pr_debug("data=%x mask=%x\n", data, mask);

	data &= mask;
	if (data == mask)/* all done */
		return true;

	return false;
}

void dp_sink_train_set_adjust(struct mdss_dp_drv_pdata *ep)
{
	int i;
	int max = 0;


	/* use the max level across lanes */
	for (i = 0; i < ep->lane_cnt; i++) {
		pr_debug("lane=%d req_voltage_swing=%d",
			i, ep->link_status.req_voltage_swing[i]);
		if (max < ep->link_status.req_voltage_swing[i])
			max = ep->link_status.req_voltage_swing[i];
	}

	ep->v_level = max;

	/* use the max level across lanes */
	max = 0;
	for (i = 0; i < ep->lane_cnt; i++) {
		pr_debug("lane=%d req_pre_emphasis=%d",
			i, ep->link_status.req_pre_emphasis[i]);
		if (max < ep->link_status.req_pre_emphasis[i])
			max = ep->link_status.req_pre_emphasis[i];
	}

	ep->p_level = max;

	/**
	 * Adjust the voltage swing and pre-emphasis level combination to within
	 * the allowable range.
	 */
	if (ep->v_level > DPCD_LINK_VOLTAGE_MAX) {
		pr_debug("Requested vSwingLevel=%d, change to %d\n",
				ep->v_level, DPCD_LINK_VOLTAGE_MAX);
		ep->v_level = DPCD_LINK_VOLTAGE_MAX;
	}

	if (ep->p_level > DPCD_LINK_PRE_EMPHASIS_MAX) {
		pr_debug("Requested preEmphasisLevel=%d, change to %d\n",
				ep->p_level, DPCD_LINK_PRE_EMPHASIS_MAX);
		ep->p_level = DPCD_LINK_PRE_EMPHASIS_MAX;
	}

	if ((ep->p_level > DPCD_LINK_PRE_EMPHASIS_LEVEL_1)
			&& (ep->v_level == DPCD_LINK_VOLTAGE_LEVEL_2)) {
		pr_debug("Requested preEmphasisLevel=%d, change to %d\n",
				ep->p_level, DPCD_LINK_PRE_EMPHASIS_LEVEL_1);
		ep->p_level = DPCD_LINK_PRE_EMPHASIS_LEVEL_1;
	}

	pr_debug("v_level=%d, p_level=%d",
					ep->v_level, ep->p_level);
}

static void dp_host_train_set(struct mdss_dp_drv_pdata *ep, int train)
{
	int bit, cnt;
	u32 data;


	bit = 1;
	bit  <<=  (train - 1);
	pr_debug("bit=%d train=%d\n", bit, train);
	dp_write(ep->base + DP_STATE_CTRL, bit);

	bit = 8;
	bit <<= (train - 1);
	cnt = 10;
	while (cnt--) {
		data = dp_read(ep->base + DP_MAINLINK_READY);
		if (data & bit)
			break;
	}

	if (cnt == 0)
		pr_err("set link_train=%d failed\n", train);
}

char vm_pre_emphasis[4][4] = {
	{0x00, 0x09, 0x11, 0x0C},	/* pe0, 0 db */
	{0x00, 0x0A, 0x10, 0xFF},	/* pe1, 3.5 db */
	{0x00, 0x0C, 0xFF, 0xFF},	/* pe2, 6.0 db */
	{0x00, 0xFF, 0xFF, 0xFF}	/* pe3, 9.5 db */
};

/* voltage swing, 0.2v and 1.0v are not support */
char vm_voltage_swing[4][4] = {
	{0x07, 0x0f, 0x12, 0x1E}, /* sw0, 0.4v  */
	{0x11, 0x1D, 0x1F, 0xFF}, /* sw1, 0.6 v */
	{0x18, 0x1F, 0xFF, 0xFF}, /* sw1, 0.8 v */
	{0x1E, 0xFF, 0xFF, 0xFF}  /* sw1, 1.2 v, optional */
};

static void dp_voltage_pre_emphasise_set(struct mdss_dp_drv_pdata *dp)
{
	u32 value0 = 0;
	u32 value1 = 0;

	pr_debug("v=%d p=%d\n", dp->v_level, dp->p_level);

	value0 = vm_voltage_swing[(int)(dp->v_level)][(int)(dp->p_level)];
	value1 = vm_pre_emphasis[(int)(dp->v_level)][(int)(dp->p_level)];

	/* Enable MUX to use Cursor values from these registers */
	value0 |= BIT(5);
	value1 |= BIT(5);
	/* Configure host and panel only if both values are allowed */
	if (value0 != 0xFF && value1 != 0xFF) {
		dp_write(dp->phy_io.base +
			QSERDES_TX0_OFFSET + TXn_TX_DRV_LVL,
			value0);
		dp_write(dp->phy_io.base +
			QSERDES_TX1_OFFSET + TXn_TX_DRV_LVL,
			value0);
		dp_write(dp->phy_io.base +
			QSERDES_TX0_OFFSET + TXn_TX_EMP_POST1_LVL,
			value1);
		dp_write(dp->phy_io.base +
			QSERDES_TX1_OFFSET + TXn_TX_EMP_POST1_LVL,
			value1);

		pr_debug("value0=0x%x value1=0x%x",
						value0, value1);
		dp_lane_set_write(dp, dp->v_level, dp->p_level);
	}

}

static int dp_start_link_train_1(struct mdss_dp_drv_pdata *ep)
{
	int tries, old_v_level;
	int ret = 0;
	int usleep_time;
	int const maximum_retries = 5;

	pr_debug("Entered++");

	dp_host_train_set(ep, 0x01); /* train_1 */
	dp_cap_lane_rate_set(ep);
	dp_train_pattern_set_write(ep, 0x21); /* train_1 */
	dp_voltage_pre_emphasise_set(ep);

	tries = 0;
	old_v_level = ep->v_level;
	while (1) {
		usleep_time = ep->dpcd.training_read_interval;
		usleep_range(usleep_time, usleep_time);

		dp_link_status_read(ep, 6);
		if (mdss_dp_aux_clock_recovery_done(ep)) {
			ret = 0;
			break;
		}

		if (ep->v_level == DPCD_LINK_VOLTAGE_MAX) {
			ret = -1;
			break;	/* quit */
		}

		if (old_v_level == ep->v_level) {
			tries++;
			if (tries >= maximum_retries) {
				ret = -1;
				break;	/* quit */
			}
		} else {
			tries = 0;
			old_v_level = ep->v_level;
		}

		dp_sink_train_set_adjust(ep);
		dp_voltage_pre_emphasise_set(ep);
	}

	return ret;
}

static int dp_start_link_train_2(struct mdss_dp_drv_pdata *ep)
{
	int tries = 0;
	int ret = 0;
	int usleep_time;
	char pattern;
	int const maximum_retries = 5;

	pr_debug("Entered++");

	if (ep->dpcd.flags & DPCD_TPS3)
		pattern = 0x03;
	else
		pattern = 0x02;

	dp_train_pattern_set_write(ep, pattern | 0x20);/* train_2 */

	do  {
		dp_voltage_pre_emphasise_set(ep);
		dp_host_train_set(ep, pattern);

		usleep_time = ep->dpcd.training_read_interval;
		usleep_range(usleep_time, usleep_time);

		dp_link_status_read(ep, 6);

		if (mdss_dp_aux_channel_eq_done(ep)) {
			ret = 0;
			break;
		}

		tries++;
		if (tries > maximum_retries) {
			ret = -1;
			break;
		}

		dp_sink_train_set_adjust(ep);
	} while (1);

	return ret;
}

static int dp_link_rate_down_shift(struct mdss_dp_drv_pdata *ep)
{
	int ret = 0;

	if (!ep)
		return -EINVAL;

	switch (ep->link_rate) {
	case DP_LINK_RATE_540:
		ep->link_rate = DP_LINK_RATE_270;
		break;
	case DP_LINK_RATE_270:
		ep->link_rate = DP_LINK_RATE_162;
		break;
	case DP_LINK_RATE_162:
	default:
		ret = -EINVAL;
		break;
	};

	pr_debug("new rate=%d\n", ep->link_rate);

	return ret;
}

int mdss_dp_aux_set_sink_power_state(struct mdss_dp_drv_pdata *ep, char state)
{
	int ret;

	ret = dp_aux_write_buf(ep, 0x600, &state, 1, 0);
	pr_debug("state=%d ret=%d\n", state, ret);
	return ret;
}

static void dp_clear_training_pattern(struct mdss_dp_drv_pdata *ep)
{
	int usleep_time;

	pr_debug("Entered++\n");
	dp_train_pattern_set_write(ep, 0);
	usleep_time = ep->dpcd.training_read_interval;
	usleep_range(usleep_time, usleep_time);
}

int mdss_dp_link_train(struct mdss_dp_drv_pdata *dp)
{
	int ret = 0;

	ret = dp_aux_chan_ready(dp);
	if (ret) {
		pr_err("LINK Train failed: aux chan NOT ready\n");
		complete(&dp->train_comp);
		return ret;
	}

	dp_write(dp->base + DP_MAINLINK_CTRL, 0x1);

	mdss_dp_aux_set_sink_power_state(dp, SINK_POWER_ON);

	dp->v_level = 0; /* start from default level */
	dp->p_level = 0;
	mdss_dp_config_ctrl(dp);

	mdss_dp_state_ctrl(&dp->ctrl_io, 0);
	dp_clear_training_pattern(dp);

	ret = dp_start_link_train_1(dp);
	if (ret < 0) {
		if (!dp_link_rate_down_shift(dp)) {
			pr_debug("retry with lower rate\n");
			return -EAGAIN;
		} else {
			pr_err("Training 1 failed\n");
			ret = -EINVAL;
			goto clear;
		}
	}

	pr_debug("Training 1 completed successfully\n");

	ret = dp_start_link_train_2(dp);
	if (ret < 0) {
		if (!dp_link_rate_down_shift(dp)) {
			pr_debug("retry with lower rate\n");
			return -EAGAIN;
		} else {
			pr_err("Training 2 failed\n");
			ret = -EINVAL;
			goto clear;
		}
	}

	pr_debug("Training 2 completed successfully\n");

clear:
	dp_clear_training_pattern(dp);
	if (ret != -EINVAL) {
		mdss_dp_config_misc_settings(&dp->ctrl_io,
				&dp->panel_data.panel_info);
		mdss_dp_setup_tr_unit(&dp->ctrl_io, dp->link_rate,
					dp->lane_cnt, dp->vic);
		mdss_dp_state_ctrl(&dp->ctrl_io, ST_SEND_VIDEO);
		pr_debug("State_ctrl set to SEND_VIDEO\n");
	}

	complete(&dp->train_comp);
	return ret;
}

void mdss_dp_dpcd_cap_read(struct mdss_dp_drv_pdata *ep)
{
	dp_sink_capability_read(ep, 16);
}

void mdss_dp_aux_parse_sink_status_field(struct mdss_dp_drv_pdata *ep)
{
	dp_sink_parse_sink_count(ep);
	dp_sink_parse_test_request(ep);
	dp_link_status_read(ep, 6);
}

int mdss_dp_dpcd_status_read(struct mdss_dp_drv_pdata *ep)
{
	struct dpcd_link_status *sp;
	int ret = 0; /* not sync */

	ret = dp_link_status_read(ep, 6);

	if (ret) {
		sp = &ep->link_status;
		ret = sp->port_0_in_sync; /* 1 == sync */
	}

	return ret;
}

void mdss_dp_fill_link_cfg(struct mdss_dp_drv_pdata *ep)
{
	struct display_timing_desc *dp;

	dp = &ep->edid.timing[0];
	ep->lane_cnt = ep->dpcd.max_lane_count;

	pr_debug("pclk=%d rate=%d lane=%d\n",
		ep->pixel_rate, ep->link_rate, ep->lane_cnt);

}

void mdss_dp_aux_init(struct mdss_dp_drv_pdata *ep)
{
	mutex_init(&ep->aux_mutex);
	mutex_init(&ep->train_mutex);
	init_completion(&ep->aux_comp);
	init_completion(&ep->train_comp);
	init_completion(&ep->idle_comp);
	init_completion(&ep->video_comp);
	complete(&ep->train_comp); /* make non block at first time */
	complete(&ep->video_comp); /* make non block at first time */

	dp_buf_init(&ep->txp, ep->txbuf, sizeof(ep->txbuf));
	dp_buf_init(&ep->rxp, ep->rxbuf, sizeof(ep->rxbuf));
}

int mdss_dp_aux_read_rx_status(struct mdss_dp_drv_pdata *dp, u8 *rx_status)
{
	bool cp_irq;
	int rc = 0;

	if (!dp) {
		pr_err("%s Invalid input\n", __func__);
		return -EINVAL;
	}

	*rx_status = 0;

	rc = dp_aux_read_buf(dp, DP_DPCD_CP_IRQ, 1, 0);
	if (!rc) {
		pr_err("Error reading CP_IRQ\n");
		return -EINVAL;
	}

	cp_irq = *dp->rxp.data & BIT(2);

	if (cp_irq) {
		rc = dp_aux_read_buf(dp, DP_DPCD_RXSTATUS, 1, 0);
		if (!rc) {
			pr_err("Error reading RxStatus\n");
			return -EINVAL;
		}

		*rx_status = *dp->rxp.data;
	}

	return 0;
}
