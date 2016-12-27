/*
 * Copyright (c) 2011-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/spinlock.h>
#include <linux/usb_bam.h>
#include <linux/module.h>

#include "u_rmnet.h"
#include "u_data_ipa.h"
#include "configfs.h"

#define RMNET_NOTIFY_INTERVAL	5
#define RMNET_MAX_NOTIFY_SIZE	sizeof(struct usb_cdc_notification)


#define ACM_CTRL_DTR	(1 << 0)

/* TODO: use separate structures for data and
 * control paths
 */
struct f_rmnet {
	struct usb_function             func;
	struct grmnet			port;
	int				ifc_id;
	atomic_t			online;
	atomic_t			ctrl_online;
	struct usb_composite_dev	*cdev;

	spinlock_t			lock;

	/* usb eps*/
	struct usb_ep			*notify;
	struct usb_request		*notify_req;

	/* control info */
	struct gadget_ipa_port	ipa_port;
	struct list_head		cpkt_resp_q;
	unsigned long			notify_count;
	unsigned long			cpkts_len;
} *rmnet_port;

static struct usb_interface_descriptor rmnet_interface_desc = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bNumEndpoints =	3,
	.bInterfaceClass =	USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass =	USB_CLASS_VENDOR_SPEC,
	.bInterfaceProtocol =	USB_CLASS_VENDOR_SPEC,
	/* .iInterface = DYNAMIC */
};

/* Full speed support */
static struct usb_endpoint_descriptor rmnet_fs_notify_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(RMNET_MAX_NOTIFY_SIZE),
	.bInterval =		1 << RMNET_NOTIFY_INTERVAL,
};

static struct usb_endpoint_descriptor rmnet_fs_in_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize   =	cpu_to_le16(64),
};

static struct usb_endpoint_descriptor rmnet_fs_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize   =	cpu_to_le16(64),
};

static struct usb_descriptor_header *rmnet_fs_function[] = {
	(struct usb_descriptor_header *) &rmnet_interface_desc,
	(struct usb_descriptor_header *) &rmnet_fs_notify_desc,
	(struct usb_descriptor_header *) &rmnet_fs_in_desc,
	(struct usb_descriptor_header *) &rmnet_fs_out_desc,
	NULL,
};

/* High speed support */
static struct usb_endpoint_descriptor rmnet_hs_notify_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(RMNET_MAX_NOTIFY_SIZE),
	.bInterval =		RMNET_NOTIFY_INTERVAL + 4,
};

static struct usb_endpoint_descriptor rmnet_hs_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor rmnet_hs_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

static struct usb_descriptor_header *rmnet_hs_function[] = {
	(struct usb_descriptor_header *) &rmnet_interface_desc,
	(struct usb_descriptor_header *) &rmnet_hs_notify_desc,
	(struct usb_descriptor_header *) &rmnet_hs_in_desc,
	(struct usb_descriptor_header *) &rmnet_hs_out_desc,
	NULL,
};

/* Super speed support */
static struct usb_endpoint_descriptor rmnet_ss_notify_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(RMNET_MAX_NOTIFY_SIZE),
	.bInterval =		RMNET_NOTIFY_INTERVAL + 4,
};

static struct usb_ss_ep_comp_descriptor rmnet_ss_notify_comp_desc = {
	.bLength =		sizeof(rmnet_ss_notify_comp_desc),
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,

	/* the following 3 values can be tweaked if necessary */
	/* .bMaxBurst =		0, */
	/* .bmAttributes =	0, */
	.wBytesPerInterval =	cpu_to_le16(RMNET_MAX_NOTIFY_SIZE),
};

static struct usb_endpoint_descriptor rmnet_ss_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor rmnet_ss_in_comp_desc = {
	.bLength =		sizeof(rmnet_ss_in_comp_desc),
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,

	/* the following 2 values can be tweaked if necessary */
	/* .bMaxBurst =		0, */
	/* .bmAttributes =	0, */
};

static struct usb_endpoint_descriptor rmnet_ss_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor rmnet_ss_out_comp_desc = {
	.bLength =		sizeof(rmnet_ss_out_comp_desc),
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,

	/* the following 2 values can be tweaked if necessary */
	/* .bMaxBurst =		0, */
	/* .bmAttributes =	0, */
};

static struct usb_descriptor_header *rmnet_ss_function[] = {
	(struct usb_descriptor_header *) &rmnet_interface_desc,
	(struct usb_descriptor_header *) &rmnet_ss_notify_desc,
	(struct usb_descriptor_header *) &rmnet_ss_notify_comp_desc,
	(struct usb_descriptor_header *) &rmnet_ss_in_desc,
	(struct usb_descriptor_header *) &rmnet_ss_in_comp_desc,
	(struct usb_descriptor_header *) &rmnet_ss_out_desc,
	(struct usb_descriptor_header *) &rmnet_ss_out_comp_desc,
	NULL,
};

/* String descriptors */

static struct usb_string rmnet_string_defs[] = {
	[0].s = "RmNet",
	{  } /* end of list */
};

static struct usb_gadget_strings rmnet_string_table = {
	.language =		0x0409,	/* en-us */
	.strings =		rmnet_string_defs,
};

static struct usb_gadget_strings *rmnet_strings[] = {
	&rmnet_string_table,
	NULL,
};

static void frmnet_ctrl_response_available(struct f_rmnet *dev);

/* ------- misc functions --------------------*/

static inline struct f_rmnet *func_to_rmnet(struct usb_function *f)
{
	return container_of(f, struct f_rmnet, func);
}

static inline struct f_rmnet *port_to_rmnet(struct grmnet *r)
{
	return container_of(r, struct f_rmnet, port);
}

static struct usb_request *
frmnet_alloc_req(struct usb_ep *ep, unsigned len, gfp_t flags)
{
	struct usb_request *req;

	req = usb_ep_alloc_request(ep, flags);
	if (!req)
		return ERR_PTR(-ENOMEM);

	req->buf = kmalloc(len, flags);
	if (!req->buf) {
		usb_ep_free_request(ep, req);
		return ERR_PTR(-ENOMEM);
	}

	req->length = len;

	return req;
}

void frmnet_free_req(struct usb_ep *ep, struct usb_request *req)
{
	kfree(req->buf);
	usb_ep_free_request(ep, req);
}

static struct rmnet_ctrl_pkt *rmnet_alloc_ctrl_pkt(unsigned len, gfp_t flags)
{
	struct rmnet_ctrl_pkt *pkt;

	pkt = kzalloc(sizeof(struct rmnet_ctrl_pkt), flags);
	if (!pkt)
		return ERR_PTR(-ENOMEM);

	pkt->buf = kmalloc(len, flags);
	if (!pkt->buf) {
		kfree(pkt);
		return ERR_PTR(-ENOMEM);
	}
	pkt->len = len;

	return pkt;
}

static void rmnet_free_ctrl_pkt(struct rmnet_ctrl_pkt *pkt)
{
	kfree(pkt->buf);
	kfree(pkt);
}

/* -------------------------------------------*/

static int gport_rmnet_connect(struct f_rmnet *dev, unsigned intf)
{
	int			ret;
	int			src_connection_idx = 0, dst_connection_idx = 0;
	struct usb_gadget	*gadget = dev->cdev->gadget;
	enum usb_ctrl		usb_bam_type;

	ret = gqti_ctrl_connect(&dev->port, QTI_PORT_RMNET, dev->ifc_id);
	if (ret) {
		pr_err("%s: gqti_ctrl_connect failed: err:%d\n",
			__func__, ret);
		return ret;
	}

	dev->ipa_port.cdev = dev->cdev;
	ipa_data_port_select(USB_IPA_FUNC_RMNET);
	usb_bam_type = usb_bam_get_bam_type(gadget->name);
	src_connection_idx = usb_bam_get_connection_idx(usb_bam_type,
		IPA_P_BAM, USB_TO_PEER_PERIPHERAL, USB_BAM_DEVICE,
		QTI_PORT_RMNET);
	dst_connection_idx = usb_bam_get_connection_idx(usb_bam_type,
		IPA_P_BAM, PEER_PERIPHERAL_TO_USB, USB_BAM_DEVICE,
		QTI_PORT_RMNET);
	if (dst_connection_idx < 0 || src_connection_idx < 0) {
		pr_err("%s: usb_bam_get_connection_idx failed\n",
			__func__);
		gqti_ctrl_disconnect(&dev->port, QTI_PORT_RMNET);
		return -EINVAL;
	}
	ret = ipa_data_connect(&dev->ipa_port, USB_IPA_FUNC_RMNET,
			src_connection_idx, dst_connection_idx);
	if (ret) {
		pr_err("%s: ipa_data_connect failed: err:%d\n",
			__func__, ret);
		gqti_ctrl_disconnect(&dev->port, QTI_PORT_RMNET);
		return ret;
	}

	return 0;
}

static int gport_rmnet_disconnect(struct f_rmnet *dev)
{
	gqti_ctrl_disconnect(&dev->port, QTI_PORT_RMNET);
	ipa_data_disconnect(&dev->ipa_port, USB_IPA_FUNC_RMNET);
	return 0;
}

static void frmnet_free(struct usb_function *f)
{
	struct f_rmnet_opts *opts;

	opts = container_of(f->fi, struct f_rmnet_opts, func_inst);
	opts->refcnt--;
	kfree(rmnet_port);
	rmnet_port = NULL;
}

static void frmnet_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_rmnet *dev = func_to_rmnet(f);

	pr_debug("%s: start unbinding\n", __func__);
	if (gadget_is_superspeed(c->cdev->gadget))
		usb_free_descriptors(f->ss_descriptors);
	if (gadget_is_dualspeed(c->cdev->gadget))
		usb_free_descriptors(f->hs_descriptors);
	usb_free_descriptors(f->fs_descriptors);

	frmnet_free_req(dev->notify, dev->notify_req);

	kfree(f->name);
}

static void frmnet_purge_responses(struct f_rmnet *dev)
{
	unsigned long flags;
	struct rmnet_ctrl_pkt *cpkt;

	pr_debug("%s: Purging responses\n", __func__);
	spin_lock_irqsave(&dev->lock, flags);
	while (!list_empty(&dev->cpkt_resp_q)) {
		cpkt = list_first_entry(&dev->cpkt_resp_q,
				struct rmnet_ctrl_pkt, list);

		list_del(&cpkt->list);
		rmnet_free_ctrl_pkt(cpkt);
	}
	dev->notify_count = 0;
	spin_unlock_irqrestore(&dev->lock, flags);
}

static void frmnet_suspend(struct usb_function *f)
{
	struct f_rmnet	*dev = func_to_rmnet(f);
	bool	remote_wakeup_allowed;

	if (f->config->cdev->gadget->speed == USB_SPEED_SUPER)
		remote_wakeup_allowed = f->func_wakeup_allowed;
	else
		remote_wakeup_allowed = f->config->cdev->gadget->remote_wakeup;

	pr_debug("%s: dev: %p remote_wakeup: %d\n",
		__func__, dev, remote_wakeup_allowed);

	usb_ep_fifo_flush(dev->notify);
	frmnet_purge_responses(dev);

	ipa_data_suspend(&dev->ipa_port, USB_IPA_FUNC_RMNET,
		remote_wakeup_allowed);
}

static void frmnet_resume(struct usb_function *f)
{
	struct f_rmnet	*dev = func_to_rmnet(f);
	bool	remote_wakeup_allowed;

	if (f->config->cdev->gadget->speed == USB_SPEED_SUPER)
		remote_wakeup_allowed = f->func_wakeup_allowed;
	else
		remote_wakeup_allowed = f->config->cdev->gadget->remote_wakeup;

	pr_debug("%s: dev: %p remote_wakeup: %d\n",
		__func__, dev, remote_wakeup_allowed);

	ipa_data_resume(&dev->ipa_port, USB_IPA_FUNC_RMNET,
			remote_wakeup_allowed);
}

static void frmnet_disable(struct usb_function *f)
{
	struct f_rmnet	*dev = func_to_rmnet(f);

	pr_debug("%s: Disabling\n", __func__);
	usb_ep_disable(dev->notify);
	dev->notify->driver_data = NULL;

	atomic_set(&dev->online, 0);

	frmnet_purge_responses(dev);

	msm_ep_unconfig(dev->ipa_port.out);
	msm_ep_unconfig(dev->ipa_port.in);
	gport_rmnet_disconnect(dev);
}

static int
frmnet_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_rmnet			*dev = func_to_rmnet(f);
	struct usb_composite_dev	*cdev = f->config->cdev;
	int				ret;
	struct list_head		*cpkt;

	pr_debug("%s: dev: %p\n", __func__, dev);
	dev->cdev = cdev;
	if (dev->notify->driver_data) {
		pr_debug("%s: reset port\n", __func__);
		usb_ep_disable(dev->notify);
	}

	ret = config_ep_by_speed(cdev->gadget, f, dev->notify);
	if (ret) {
		dev->notify->desc = NULL;
		ERROR(cdev, "config_ep_by_speed failes for ep %s, result %d\n",
					dev->notify->name, ret);
		return ret;
	}
	ret = usb_ep_enable(dev->notify);

	if (ret) {
		pr_err("%s: usb ep#%s enable failed, err#%d\n",
				__func__, dev->notify->name, ret);
		dev->notify->desc = NULL;
		return ret;
	}
	dev->notify->driver_data = dev;

	if (!dev->ipa_port.in->desc || !dev->ipa_port.out->desc) {
		if (config_ep_by_speed(cdev->gadget, f, dev->ipa_port.in) ||
		config_ep_by_speed(cdev->gadget, f, dev->ipa_port.out)) {
			pr_err("%s(): config_ep_by_speed failed.\n", __func__);
			ret = -EINVAL;
			goto err_disable_ep;
		}
		dev->ipa_port.cdev = dev->cdev;
	}

	ret = gport_rmnet_connect(dev, intf);
	if (ret) {
		pr_err("%s(): gport_rmnet_connect fail with err:%d\n",
							__func__, ret);
		goto err_disable_ep;
	}

	atomic_set(&dev->online, 1);

	/*
	 * In case notifications were aborted, but there are pending control
	 * packets in the response queue, re-add the notifications.
	 */
	list_for_each(cpkt, &dev->cpkt_resp_q)
		frmnet_ctrl_response_available(dev);

	return ret;
err_disable_ep:
	dev->ipa_port.in->desc = NULL;
	dev->ipa_port.out->desc = NULL;
	usb_ep_disable(dev->notify);

	return ret;
}

static void frmnet_ctrl_response_available(struct f_rmnet *dev)
{
	struct usb_request		*req = dev->notify_req;
	struct usb_cdc_notification	*event;
	unsigned long			flags;
	int				ret;
	struct rmnet_ctrl_pkt		*cpkt;

	pr_debug("%s: dev: %p\n", __func__, dev);
	spin_lock_irqsave(&dev->lock, flags);
	if (!atomic_read(&dev->online) || !req || !req->buf) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return;
	}

	if (++dev->notify_count != 1) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return;
	}

	event = req->buf;
	event->bmRequestType = USB_DIR_IN | USB_TYPE_CLASS
			| USB_RECIP_INTERFACE;
	event->bNotificationType = USB_CDC_NOTIFY_RESPONSE_AVAILABLE;
	event->wValue = cpu_to_le16(0);
	event->wIndex = cpu_to_le16(dev->ifc_id);
	event->wLength = cpu_to_le16(0);
	spin_unlock_irqrestore(&dev->lock, flags);

	ret = usb_ep_queue(dev->notify, dev->notify_req, GFP_ATOMIC);
	if (ret) {
		spin_lock_irqsave(&dev->lock, flags);
		if (!list_empty(&dev->cpkt_resp_q)) {
			if (dev->notify_count > 0)
				dev->notify_count--;
			else {
				pr_debug("%s: Invalid notify_count=%lu to decrement\n",
					 __func__, dev->notify_count);
				spin_unlock_irqrestore(&dev->lock, flags);
				return;
			}
			cpkt = list_first_entry(&dev->cpkt_resp_q,
					struct rmnet_ctrl_pkt, list);
			list_del(&cpkt->list);
			rmnet_free_ctrl_pkt(cpkt);
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		pr_debug("ep enqueue error %d\n", ret);
	}
}

static void frmnet_connect(struct grmnet *gr)
{
	struct f_rmnet			*dev;

	if (!gr) {
		pr_err("%s: Invalid grmnet:%p\n", __func__, gr);
		return;
	}

	dev = port_to_rmnet(gr);

	atomic_set(&dev->ctrl_online, 1);
}

static void frmnet_disconnect(struct grmnet *gr)
{
	struct f_rmnet			*dev;
	struct usb_cdc_notification	*event;
	int				status;

	if (!gr) {
		pr_err("%s: Invalid grmnet:%p\n", __func__, gr);
		return;
	}

	dev = port_to_rmnet(gr);

	atomic_set(&dev->ctrl_online, 0);

	if (!atomic_read(&dev->online)) {
		pr_debug("%s: nothing to do\n", __func__);
		return;
	}

	usb_ep_fifo_flush(dev->notify);

	event = dev->notify_req->buf;
	event->bmRequestType = USB_DIR_IN | USB_TYPE_CLASS
			| USB_RECIP_INTERFACE;
	event->bNotificationType = USB_CDC_NOTIFY_NETWORK_CONNECTION;
	event->wValue = cpu_to_le16(0);
	event->wIndex = cpu_to_le16(dev->ifc_id);
	event->wLength = cpu_to_le16(0);

	status = usb_ep_queue(dev->notify, dev->notify_req, GFP_ATOMIC);
	if (status < 0) {
		if (!atomic_read(&dev->online))
			return;
		pr_err("%s: rmnet notify ep enqueue error %d\n",
				__func__, status);
	}

	frmnet_purge_responses(dev);
}

static int
frmnet_send_cpkt_response(void *gr, void *buf, size_t len)
{
	struct f_rmnet		*dev;
	struct rmnet_ctrl_pkt	*cpkt;
	unsigned long		flags;

	if (!gr || !buf) {
		pr_err("%s: Invalid grmnet/buf, grmnet:%p buf:%p\n",
				__func__, gr, buf);
		return -ENODEV;
	}
	cpkt = rmnet_alloc_ctrl_pkt(len, GFP_ATOMIC);
	if (IS_ERR(cpkt)) {
		pr_err("%s: Unable to allocate ctrl pkt\n", __func__);
		return -ENOMEM;
	}
	memcpy(cpkt->buf, buf, len);
	cpkt->len = len;

	dev = port_to_rmnet(gr);

	pr_debug("%s: dev: %p\n", __func__, dev);
	if (!atomic_read(&dev->online) || !atomic_read(&dev->ctrl_online)) {
		rmnet_free_ctrl_pkt(cpkt);
		return 0;
	}

	spin_lock_irqsave(&dev->lock, flags);
	list_add_tail(&cpkt->list, &dev->cpkt_resp_q);
	spin_unlock_irqrestore(&dev->lock, flags);

	frmnet_ctrl_response_available(dev);

	return 0;
}

static void
frmnet_cmd_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_rmnet			*dev = req->context;
	struct usb_composite_dev	*cdev;

	if (!dev) {
		pr_err("%s: rmnet dev is null\n", __func__);
		return;
	}
	pr_debug("%s: dev: %p\n", __func__, dev);
	cdev = dev->cdev;

	if (dev->port.send_encap_cmd) {
		dev->port.send_encap_cmd(QTI_PORT_RMNET, req->buf, req->actual);
	}
}

static void frmnet_notify_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_rmnet	*dev = req->context;
	int	status = req->status;
	unsigned long		flags;
	struct rmnet_ctrl_pkt	*cpkt;

	pr_debug("%s: dev: %p\n", __func__, dev);
	switch (status) {
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* connection gone */
		spin_lock_irqsave(&dev->lock, flags);
		dev->notify_count = 0;
		spin_unlock_irqrestore(&dev->lock, flags);
		break;
	default:
		pr_err("rmnet notify ep error %d\n", status);
		/* FALLTHROUGH */
	case 0:
		if (!atomic_read(&dev->ctrl_online))
			break;

		spin_lock_irqsave(&dev->lock, flags);
		if (dev->notify_count > 0) {
			dev->notify_count--;
			if (dev->notify_count == 0) {
				spin_unlock_irqrestore(&dev->lock, flags);
				break;
			}
		} else {
			pr_debug("%s: Invalid notify_count=%lu to decrement\n",
					__func__, dev->notify_count);
			spin_unlock_irqrestore(&dev->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&dev->lock, flags);

		status = usb_ep_queue(dev->notify, req, GFP_ATOMIC);
		if (status) {
			spin_lock_irqsave(&dev->lock, flags);
			if (!list_empty(&dev->cpkt_resp_q)) {
				if (dev->notify_count > 0)
					dev->notify_count--;
				else {
					pr_err("%s: Invalid notify_count=%lu to decrement\n",
						__func__, dev->notify_count);
					spin_unlock_irqrestore(&dev->lock,
								flags);
					break;
				}
				cpkt = list_first_entry(&dev->cpkt_resp_q,
						struct rmnet_ctrl_pkt, list);
				list_del(&cpkt->list);
				rmnet_free_ctrl_pkt(cpkt);
			}
			spin_unlock_irqrestore(&dev->lock, flags);
			pr_debug("ep enqueue error %d\n", status);
		}
		break;
	}
}

static int
frmnet_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct f_rmnet			*dev = func_to_rmnet(f);
	struct usb_composite_dev	*cdev = dev->cdev;
	struct usb_request		*req = cdev->req;
	u16				w_index = le16_to_cpu(ctrl->wIndex);
	u16				w_value = le16_to_cpu(ctrl->wValue);
	u16				w_length = le16_to_cpu(ctrl->wLength);
	int				ret = -EOPNOTSUPP;

	pr_debug("%s: dev: %p\n", __func__, dev);
	if (!atomic_read(&dev->online)) {
		pr_warn("%s: usb cable is not connected\n", __func__);
		return -ENOTCONN;
	}

	switch ((ctrl->bRequestType << 8) | ctrl->bRequest) {

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
			| USB_CDC_SEND_ENCAPSULATED_COMMAND:
		pr_debug("%s: USB_CDC_SEND_ENCAPSULATED_COMMAND\n"
				 , __func__);
		ret = w_length;
		req->complete = frmnet_cmd_complete;
		req->context = dev;
		break;


	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
			| USB_CDC_GET_ENCAPSULATED_RESPONSE:
		pr_debug("%s: USB_CDC_GET_ENCAPSULATED_RESPONSE\n", __func__);
		if (w_value) {
			pr_err("%s: invalid w_value = %04x\n",
				   __func__, w_value);
			goto invalid;
		} else {
			unsigned len;
			struct rmnet_ctrl_pkt *cpkt;

			spin_lock(&dev->lock);
			if (list_empty(&dev->cpkt_resp_q)) {
				pr_err("ctrl resp queue empty: ");
				pr_err("req%02x.%02x v%04x i%04x l%d\n",
					ctrl->bRequestType, ctrl->bRequest,
					w_value, w_index, w_length);
				ret = 0;
				spin_unlock(&dev->lock);
				goto invalid;
			}

			cpkt = list_first_entry(&dev->cpkt_resp_q,
					struct rmnet_ctrl_pkt, list);
			list_del(&cpkt->list);
			spin_unlock(&dev->lock);

			len = min_t(unsigned, w_length, cpkt->len);
			memcpy(req->buf, cpkt->buf, len);
			ret = len;

			rmnet_free_ctrl_pkt(cpkt);
		}
		break;
	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
			| USB_CDC_REQ_SET_CONTROL_LINE_STATE:
		pr_debug("%s: USB_CDC_REQ_SET_CONTROL_LINE_STATE: DTR:%d\n",
				__func__, w_value & ACM_CTRL_DTR ? 1 : 0);
		if (dev->port.notify_modem) {
			dev->port.notify_modem(&dev->port,
				QTI_PORT_RMNET, w_value);
		}
		ret = 0;

		break;
	default:

invalid:
		DBG(cdev, "invalid control req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
	}

	/* respond with data transfer or status phase? */
	if (ret >= 0) {
		VDBG(cdev, "rmnet req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
		req->zero = (ret < w_length);
		req->length = ret;
		ret = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (ret < 0)
			ERROR(cdev, "rmnet ep0 enqueue err %d\n", ret);
	}

	return ret;
}

static int frmnet_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_rmnet			*dev = func_to_rmnet(f);
	struct usb_ep			*ep;
	struct usb_composite_dev	*cdev = c->cdev;
	int				ret = -ENODEV;

	if (rmnet_string_defs[0].id == 0) {
		ret = usb_string_id(c->cdev);
		if (ret < 0) {
			pr_err("%s: failed to get string id, err:%d\n",
					__func__, ret);
			return ret;
		}
		rmnet_string_defs[0].id = ret;
	}

	pr_debug("%s: start binding\n", __func__);
	dev->ifc_id = usb_interface_id(c, f);
	if (dev->ifc_id < 0) {
		pr_err("%s: unable to allocate ifc id, err:%d\n",
				__func__, dev->ifc_id);
		return dev->ifc_id;
	}
	rmnet_interface_desc.bInterfaceNumber = dev->ifc_id;

	ep = usb_ep_autoconfig(cdev->gadget, &rmnet_fs_in_desc);
	if (!ep) {
		pr_err("%s: usb epin autoconfig failed\n", __func__);
		return -ENODEV;
	}
	dev->ipa_port.in = ep;
	ep->driver_data = cdev;

	ep = usb_ep_autoconfig(cdev->gadget, &rmnet_fs_out_desc);
	if (!ep) {
		pr_err("%s: usb epout autoconfig failed\n", __func__);
		ret = -ENODEV;
		goto ep_auto_out_fail;
	}
	dev->ipa_port.out = ep;
	ep->driver_data = cdev;

	ep = usb_ep_autoconfig(cdev->gadget, &rmnet_fs_notify_desc);
	if (!ep) {
		pr_err("%s: usb epnotify autoconfig failed\n", __func__);
		ret = -ENODEV;
		goto ep_auto_notify_fail;
	}
	dev->notify = ep;
	ep->driver_data = cdev;

	dev->notify_req = frmnet_alloc_req(ep,
				sizeof(struct usb_cdc_notification),
				GFP_KERNEL);
	if (IS_ERR(dev->notify_req)) {
		pr_err("%s: unable to allocate memory for notify req\n",
				__func__);
		ret = -ENOMEM;
		goto ep_notify_alloc_fail;
	}

	dev->notify_req->complete = frmnet_notify_complete;
	dev->notify_req->context = dev;

	ret = -ENOMEM;
	f->fs_descriptors = usb_copy_descriptors(rmnet_fs_function);

	if (!f->fs_descriptors) {
		pr_err("%s: no descriptors,usb_copy descriptors(fs)failed\n",
			__func__);
		goto fail;
	}
	if (gadget_is_dualspeed(cdev->gadget)) {
		rmnet_hs_in_desc.bEndpointAddress =
				rmnet_fs_in_desc.bEndpointAddress;
		rmnet_hs_out_desc.bEndpointAddress =
				rmnet_fs_out_desc.bEndpointAddress;
		rmnet_hs_notify_desc.bEndpointAddress =
				rmnet_fs_notify_desc.bEndpointAddress;

		/* copy descriptors, and track endpoint copies */
		f->hs_descriptors = usb_copy_descriptors(rmnet_hs_function);

		if (!f->hs_descriptors) {
			pr_err("%s: no hs_descriptors,usb_copy descriptors(hs)failed\n",
			__func__);
			goto fail;
		}
	}

	if (gadget_is_superspeed(cdev->gadget)) {
		rmnet_ss_in_desc.bEndpointAddress =
				rmnet_fs_in_desc.bEndpointAddress;
		rmnet_ss_out_desc.bEndpointAddress =
				rmnet_fs_out_desc.bEndpointAddress;
		rmnet_ss_notify_desc.bEndpointAddress =
				rmnet_fs_notify_desc.bEndpointAddress;

		/* copy descriptors, and track endpoint copies */
		f->ss_descriptors = usb_copy_descriptors(rmnet_ss_function);

		if (!f->ss_descriptors) {
			pr_err("%s: no ss_descriptors,usb_copy descriptors(ss)failed\n",
			__func__);
			goto fail;
		}
	}

	pr_debug("%s: RmNet %s Speed, IN:%s OUT:%s\n",
		__func__, gadget_is_dualspeed(cdev->gadget) ? "dual" : "full",
		dev->ipa_port.in->name, dev->ipa_port.out->name);

	return 0;

fail:
	if (f->ss_descriptors)
		usb_free_descriptors(f->ss_descriptors);
	if (f->hs_descriptors)
		usb_free_descriptors(f->hs_descriptors);
	if (f->fs_descriptors)
		usb_free_descriptors(f->fs_descriptors);
	if (dev->notify_req)
		frmnet_free_req(dev->notify, dev->notify_req);
ep_notify_alloc_fail:
	dev->notify->driver_data = NULL;
	dev->notify = NULL;
ep_auto_notify_fail:
	dev->ipa_port.out->driver_data = NULL;
	dev->ipa_port.out = NULL;
ep_auto_out_fail:
	dev->ipa_port.in->driver_data = NULL;
	dev->ipa_port.in = NULL;

	return ret;
}

static struct usb_function *frmnet_bind_config(struct usb_function_instance *fi)
{
	struct f_rmnet_opts *opts;
	int			status;
	struct f_rmnet		*dev;
	struct usb_function	*f;
	unsigned long		flags;

	/* allocate and initialize one new instance */
	status = -ENOMEM;
	opts = container_of(fi, struct f_rmnet_opts, func_inst);
	opts->refcnt++;
	dev = opts->dev;
	spin_lock_irqsave(&dev->lock, flags);
	f = &dev->func;
	f->name = kasprintf(GFP_ATOMIC, "rmnet%d", 0);
	spin_unlock_irqrestore(&dev->lock, flags);
	if (!f->name) {
		pr_err("%s: cannot allocate memory for name\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	f->strings = rmnet_strings;
	f->bind = frmnet_bind;
	f->unbind = frmnet_unbind;
	f->disable = frmnet_disable;
	f->set_alt = frmnet_set_alt;
	f->setup = frmnet_setup;
	f->suspend = frmnet_suspend;
	f->resume = frmnet_resume;
	f->free_func = frmnet_free;
	dev->port.send_cpkt_response = frmnet_send_cpkt_response;
	dev->port.disconnect = frmnet_disconnect;
	dev->port.connect = frmnet_connect;

	pr_debug("%s: complete\n", __func__);

	return f;
}

static int rmnet_init(void)
{
	return gqti_ctrl_init();
}

static void frmnet_cleanup(void)
{
	gqti_ctrl_cleanup();
}

static void rmnet_free_inst(struct usb_function_instance *f)
{
	struct f_rmnet_opts *opts = container_of(f, struct f_rmnet_opts,
						func_inst);
	ipa_data_free(USB_IPA_FUNC_RMNET);
	kfree(opts);
}

static int rmnet_set_inst_name(struct usb_function_instance *fi,
		const char *name)
{
	int name_len;
	int ret;

	name_len = strlen(name) + 1;
	if (name_len > MAX_INST_NAME_LEN)
		return -ENAMETOOLONG;

	ret = ipa_data_setup(USB_IPA_FUNC_RMNET);
	return ret;
}

static inline struct f_rmnet_opts *to_f_rmnet_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_rmnet_opts,
				func_inst.group);
}

static void rmnet_opts_release(struct config_item *item)
{
	struct f_rmnet_opts *opts = to_f_rmnet_opts(item);

	usb_put_function_instance(&opts->func_inst);
};

static struct configfs_item_operations rmnet_item_ops = {
	.release = rmnet_opts_release,
};

static struct config_item_type rmnet_func_type = {
	.ct_item_ops    = &rmnet_item_ops,
	.ct_owner       = THIS_MODULE,
};

static struct usb_function_instance *rmnet_alloc_inst(void)
{
	struct f_rmnet_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.set_inst_name = rmnet_set_inst_name;
	opts->func_inst.free_func_inst = rmnet_free_inst;

	config_group_init_type_name(&opts->func_inst.group, "",
				&rmnet_func_type);
	return &opts->func_inst;
}

static struct usb_function *rmnet_alloc(struct usb_function_instance *fi)
{
	struct f_rmnet_opts *opts = container_of(fi,
					struct f_rmnet_opts, func_inst);
	rmnet_port = kzalloc(sizeof(struct f_rmnet), GFP_KERNEL);
	if (!rmnet_port)
		return ERR_PTR(-ENOMEM);
	opts->dev = rmnet_port;
	spin_lock_init(&rmnet_port->lock);
	INIT_LIST_HEAD(&rmnet_port->cpkt_resp_q);
	return frmnet_bind_config(fi);
}

DECLARE_USB_FUNCTION(rmnet_bam, rmnet_alloc_inst, rmnet_alloc);

static int __init usb_rmnet_init(void)
{
	int ret;

	ret = rmnet_init();
	if (!ret) {
		ret = usb_function_register(&rmnet_bamusb_func);
		if (ret) {
			pr_err("%s: failed to register rmnet %d\n",
					__func__, ret);
			return ret;
		}
	}
	return ret;
}

static void __exit usb_rmnet_exit(void)
{
	usb_function_unregister(&rmnet_bamusb_func);
	frmnet_cleanup();
}

module_init(usb_rmnet_init);
module_exit(usb_rmnet_exit);
MODULE_DESCRIPTION("USB RMNET Function Driver");
