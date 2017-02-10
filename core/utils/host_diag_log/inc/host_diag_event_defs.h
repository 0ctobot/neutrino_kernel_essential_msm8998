/*
 * Copyright (c) 2014-2017 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

#ifndef EVENT_DEFS_H
#define EVENT_DEFS_H

typedef enum {
	EVENT_DROP_ID = 0,

	/* Events between 0x1 to 0x674 are not used */

	EVENT_WLAN_SECURITY = 0x675, /* 13 byte payload */
	EVENT_WLAN_STATUS, /* 15 byte payload */

	/* Events 0x677 and 0x678 are not used */

	EVENT_WLAN_QOS = 0x679, /* 2 byte payload */
	EVENT_WLAN_PE, /* 16 byte payload */

	/* Events between 0x67b to 0x67f are not used */

	EVENT_WLAN_BRINGUP_STATUS = 0x680, /* 12 byte payload */
	EVENT_WLAN_POWERSAVE_GENERIC, /* 16 byte payload */
	EVENT_WLAN_POWERSAVE_WOW, /* 11 byte payload */

	/* Events between 0x683 to 0x690 are not used */

	EVENT_WLAN_BTC = 0x691, /* 15 byte payload */
	EVENT_WLAN_EAPOL = 0xA8D,/* 18 bytes payload */
	EVENT_WLAN_WAKE_LOCK = 0xAA2, /* 96 bytes payload */
	EVENT_WLAN_BEACON_RECEIVED = 0xAA6, /* FW event: 2726 */
	EVENT_WLAN_LOG_COMPLETE = 0xAA7, /* 16 bytes payload */
	EVENT_WLAN_STATUS_V2 = 0xAB3,

	/*
	 * <diag_event>
	 * EVENT_WLAN_TDLS_TEARDOWN
	 * @ reason: reason for tear down.
	 * @peer_mac: Peer mac address
	 *
	 *
	 * This event is sent when TDLS tear down happens.
	 *
	 * Supported Feature: TDLS
	 *
	 * </diag_event>
	 */
	EVENT_WLAN_TDLS_TEARDOWN = 0xAB5,

	/*
	 * <diag_event>
	 * EVENT_WLAN_TDLS_ENABLE_LINK
	 * @peer_mac: peer mac
	 * @is_off_chan_supported: If peer supports off channel
	 * @is_off_chan_configured: If off channel is configured
	 * @is_off_chan_established: If off channel is established
	 *
	 *
	 * This event is sent when TDLS enable link happens.
	 *
	 * Supported Feature: TDLS
	 *
	 * </diag_event>
	 */
	EVENT_WLAN_TDLS_ENABLE_LINK = 0XAB6,
	EVENT_WLAN_SUSPEND_RESUME = 0xAB7,
	EVENT_WLAN_OFFLOAD_REQ = 0xAB8,

	/*
	 * <diag_event>
	 * EVENT_TDLS_SCAN_BLOCK
	 * @status: rejected status
	 *
	 *
	 * This event is sent when scan is rejected due to TDLS.
	 *
	 * Supported Feature: TDLS
	 *
	 * </diag_event>
	 */
	EVENT_TDLS_SCAN_BLOCK = 0xAB9,

	/*
	 * <diag_event>
	 * EVENT_WLAN_TDLS_TX_RX_MGMT
	 * @event_id: event id
	 * @tx_rx: tx or rx
	 * @type: type of frame
	 * @action_sub_type: action frame type
	 * @peer_mac: peer mac
	 *
	 *
	 * This event is sent when TDLS mgmt rx tx happens.
	 *
	 * Supported Feature: TDLS
	 *
	 * </diag_event>
	 */
	EVENT_WLAN_TDLS_TX_RX_MGMT = 0xABA,
	EVENT_WLAN_LOW_RESOURCE_FAILURE = 0xABB,
	EVENT_WLAN_POWERSAVE_WOW_STATS = 0xB33,

	EVENT_MAX_ID = 0x0FFF
} event_id_enum_type;

#endif /* EVENT_DEFS_H */
