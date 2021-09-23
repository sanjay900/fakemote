#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "conf.h"
#include "fakedev.h"
#include "ipc.h"
#include "hci_state.h"
#include "mem.h"
#include "syscalls.h"
#include "tools.h"
#include "types.h"
#include "hci.h"
#include "l2cap.h"
#include "utils.h"

/* OH1 module hook information */
#define OH1_IOS_ReceiveMessage_ADDR1 0x138b365c
#define OH1_IOS_ResourceReply_ADDR1  0x138b36f4
#define OH1_IOS_ReceiveMessage_ADDR2 0x138b366c
#define OH1_IOS_ResourceReply_ADDR2  0x138b3704
#define OH1_DEV_OH1_QUEUEID_ADDR     0x138b5004

#define USBV0_IOCTLV_CTRLMSG		 0
#define USBV0_IOCTLV_BLKMSG		 1
#define USBV0_IOCTLV_INTRMSG		 2
#define USBV0_IOCTL_SUSPENDDEV		 5
#define USBV0_IOCTL_RESUMEDEV		 6
#define USBV0_IOCTLV_ISOMSG		 9
#define USBV0_IOCTLV_LBLKMSG		10
#define USBV0_IOCTLV_GETDEVLIST		12
#define USBV0_IOCTL_GETRHDESCA		15
#define USBV0_IOCTLV_GETRHPORTSTATUS	20
#define USBV0_IOCTLV_SETRHPORTSTATUS	25
#define USBV0_IOCTL_DEVREMOVALHOOK	26
#define USBV0_IOCTLV_DEVINSERTHOOK	27
#define USBV0_IOCTLV_DEVICECLASSCHANGE	28
#define USBV0_IOCTL_RESET_DEVICE	29
#define USBV0_IOCTLV_DEVINSERTHOOKID	30
#define USBV0_IOCTL_CANCEL_INSERT_HOOK	31
#define USBV0_IOCTLV_UNKNOWN_32		32

/* Wii's BT USB dongle configuration */
#define EP_HCI_CTRL	0x00
#define EP_HCI_EVENT	0x81
#define EP_ACL_DATA_IN	0x82
#define EP_ACL_DATA_OUT	0x02

/* Private definitions */

#define PERIODC_TIMER_PERIOD		500 * 1000
#define THREAD_STACK_SIZE		4096
#define INJECTED_IPCMESSAGE_COOKIE	0xFEEDCAFE

/* Global heap for dynamic memory */
static u8 heapspace[16 * 1024] ATTRIBUTE_ALIGN(32);

/* Global state */
char *moduleName = "TST";
static int orig_msg_queueid;
static int periodic_timer_id;
static int periodic_timer_cookie __attribute__((used));

static u8 usb_intr_msg_handed_to_oh1_ioctlv_0_data = EP_HCI_EVENT;
static u16 usb_intr_msg_handed_to_oh1_ioctlv_1_data;
static ioctlv usb_intr_msg_handed_to_oh1_ioctlvs[3] = {
	{&usb_intr_msg_handed_to_oh1_ioctlv_0_data, sizeof(usb_intr_msg_handed_to_oh1_ioctlv_0_data)},
	{&usb_intr_msg_handed_to_oh1_ioctlv_1_data, sizeof(usb_intr_msg_handed_to_oh1_ioctlv_1_data)},
	{NULL, 0} /* Filled dynamically */
};
static ipcmessage usb_intr_msg_handed_to_oh1 = {
	.command = IOS_IOCTLV,
	.result = IOS_OK,
	.fd = 0, /* Filled dynamically */
	.ioctlv = {
		.command = USBV0_IOCTLV_INTRMSG,
		.num_in = 2,
		.num_io = 1,
		.vector = usb_intr_msg_handed_to_oh1_ioctlvs
	}
};

static u8 usb_bulk_in_msg_handed_to_oh1_ioctlv_0_data = EP_ACL_DATA_IN;
static u16 usb_bulk_in_msg_handed_to_oh1_ioctlv_1_data;
static ioctlv usb_bulk_in_msg_handed_to_oh1_ioctlvs[3] = {
	{&usb_bulk_in_msg_handed_to_oh1_ioctlv_0_data, sizeof(usb_bulk_in_msg_handed_to_oh1_ioctlv_0_data)},
	{&usb_bulk_in_msg_handed_to_oh1_ioctlv_1_data, sizeof(usb_bulk_in_msg_handed_to_oh1_ioctlv_1_data)},
	{NULL, 0} /* Filled dynamically */
};
static ipcmessage usb_bulk_in_msg_handed_to_oh1 = {
	.command = IOS_IOCTLV,
	.result = IOS_OK,
	.fd = 0, /* Filled dynamically */
	.ioctlv = {
		.command = USBV0_IOCTLV_BLKMSG,
		.num_in = 2,
		.num_io = 1,
		.vector = usb_bulk_in_msg_handed_to_oh1_ioctlvs
	}
};

static u8 ipcmessages_heap_data[4 * 1024] ATTRIBUTE_ALIGN(32);
static int ipcmessages_heap_id;

static ipcmessage *ready_usb_intr_msg_queue_data[8] ATTRIBUTE_ALIGN(32);
static int ready_usb_intr_msg_queue_id;
static ipcmessage *pending_usb_intr_msg_queue_data[8] ATTRIBUTE_ALIGN(32);
static int pending_usb_intr_msg_queue_id;

static ipcmessage *ready_usb_bulk_in_msg_queue_data[8] ATTRIBUTE_ALIGN(32);
static int ready_usb_bulk_in_msg_queue_id;
static ipcmessage *pending_usb_bulk_in_msg_queue_data[8] ATTRIBUTE_ALIGN(32);
static int pending_usb_bulk_in_msg_queue_id;

/* Function prototypes */

static int ensure_init_oh1_context(void);
static int handle_bulk_intr_pending_message(ipcmessage *recv_msg, u16 size, ipcmessage **ret_msg,
					    int ready_queue_id, int pending_queue_id,
					    ipcmessage *hand_down_msg, int *fwd_to_usb);
static int handle_bulk_intr_ready_message(ipcmessage *ready_msg, int retval,
					  int pending_queue_id, int ready_queue_id);

/* Message allocation and enqueuing helpers */

/* Used to allocate ipcmessages (bulk in/interrupt) to inject back to the BT SW stack */
static ipcmessage *alloc_inject_message(void **data, u32 size)
{
	u32 alloc_size = sizeof(ipcmessage) + 3 * sizeof(ioctlv) + size;
	ipcmessage *msg = os_heap_alloc(ipcmessages_heap_id, alloc_size);
	if (!msg)
		return NULL;

	/* Fill ipcmessage data */
	msg->command = INJECTED_IPCMESSAGE_COOKIE;
	msg->ioctlv.vector = (void *)((u8 *)msg + sizeof(ipcmessage));
	msg->ioctlv.vector[2].data = (void *)((u8 *)msg->ioctlv.vector + 3 * sizeof(ioctlv));
	msg->ioctlv.vector[2].len = size;
	*data = msg->ioctlv.vector[2].data;

	return msg;
}

/* ACL/L2CAP message enqueue (injection) helpers */

static inline int enqueue_to_bulk_in_msg_ready_queue(ipcmessage *msg)
{
	return handle_bulk_intr_ready_message(msg, msg->ioctlv.vector[2].len,
					      pending_usb_bulk_in_msg_queue_id,
					      ready_usb_bulk_in_msg_queue_id);
}

static ipcmessage *alloc_hci_acl_msg(void **acl_payload, u16 hci_con_handle, u16 acl_payload_size)
{
	hci_acldata_hdr_t *hdr;
	ipcmessage *msg = alloc_inject_message((void **)&hdr, sizeof(*hdr) + acl_payload_size);
	if (!msg)
		return NULL;

	/* Fill message data */
	hdr->con_handle = htole16(HCI_MK_CON_HANDLE(hci_con_handle, HCI_PACKET_START,
						    HCI_POINT2POINT));
	hdr->length = htole16(acl_payload_size);
	*acl_payload = (u8 *)hdr + sizeof(*hdr);

	return msg;
}

static ipcmessage *alloc_l2cap_msg(void **l2cap_payload, u16 hci_con_handle, u16 dcid, u16 size)
{
	ipcmessage *msg;
	l2cap_hdr_t *hdr;

	msg = alloc_hci_acl_msg((void **)&hdr, hci_con_handle, sizeof(l2cap_hdr_t) + size);
	if (!msg)
		return NULL;

	/* Fill message data */
	hdr->length = htole16(size);
	hdr->dcid = htole16(dcid);
	*l2cap_payload = (u8 *)hdr + sizeof(l2cap_hdr_t);

	return msg;
}

static ipcmessage *alloc_l2cap_cmd_msg(void **l2cap_cmd_payload, u16 hci_con_handle,
				       u8 code, u8 ident, u16 size)
{
	ipcmessage *msg;
	l2cap_cmd_hdr_t *hdr;

	msg = alloc_l2cap_msg((void **)&hdr, hci_con_handle, L2CAP_SIGNAL_CID,
			      sizeof(l2cap_cmd_hdr_t) + size);
	if (!msg)
		return NULL;

	/* Fill message data */
	hdr->code = code;
	hdr->ident = ident;
	hdr->length = htole16(size);
	*l2cap_cmd_payload = (u8 *)hdr + sizeof(l2cap_cmd_hdr_t);

	return msg;
}

int l2cap_send_msg(u16 hci_con_handle, u16 dcid, const void *data, u16 size)
{
	void *payload;
	ipcmessage *msg = alloc_l2cap_msg(&payload, hci_con_handle, dcid, size);
	if (!msg)
		return IOS_ENOMEM;

	/* Fill message data */
	memcpy(payload, data, size);

	return enqueue_to_bulk_in_msg_ready_queue(msg);
}

int l2cap_send_connect_req(u16 hci_con_handle, u16 psm, u16 scid)
{
	ipcmessage *msg;
	l2cap_con_req_cp *req;

	msg = alloc_l2cap_cmd_msg((void **)&req, hci_con_handle, L2CAP_CONNECT_REQ,
				  L2CAP_CONNECT_REQ, sizeof(*req));
	if (!msg)
		return IOS_ENOMEM;

	/* Fill message data */
	req->psm = htole16(psm);
	req->scid = htole16(scid);

	return enqueue_to_bulk_in_msg_ready_queue(msg);
}

int l2cap_send_config_req(u16 hci_con_handle, u16 remote_cid, u16 mtu, u16 flush_time_out)
{
	ipcmessage *msg;
	l2cap_cfg_req_cp *req;
	l2cap_cfg_opt_t *opt;
	u32 size = sizeof(l2cap_cfg_req_cp);
	u32 offset = size;

	if (mtu != L2CAP_MTU_DEFAULT)
		size += sizeof(l2cap_cfg_opt_t) + L2CAP_OPT_MTU_SIZE;
	if (flush_time_out != L2CAP_FLUSH_TIMO_DEFAULT)
		size += sizeof(l2cap_cfg_opt_t) + L2CAP_OPT_FLUSH_TIMO_SIZE;

	msg = alloc_l2cap_cmd_msg((void **)&req, hci_con_handle, L2CAP_CONFIG_REQ,
				  L2CAP_CONFIG_REQ, size);
	if (!msg)
		return IOS_ENOMEM;

	/* Fill message data */
	req->dcid = htole16(remote_cid);
	req->flags = htole16(0);

	if (mtu != L2CAP_MTU_DEFAULT) {
		opt = (void *)((u8 *)req + offset);
		offset += sizeof(l2cap_cfg_opt_t);
		opt->type = L2CAP_OPT_MTU;
		opt->length = L2CAP_OPT_MTU_SIZE;
		*(u16 *)(void *)((u8 *)req + offset) = htole16(mtu);
		offset += L2CAP_OPT_MTU_SIZE;
	}

	if (flush_time_out != L2CAP_FLUSH_TIMO_DEFAULT) {
		opt = (void *)((u8 *)req + offset);
		offset += sizeof(l2cap_cfg_opt_t);
		opt->type = L2CAP_OPT_FLUSH_TIMO;
		opt->length = L2CAP_OPT_FLUSH_TIMO_SIZE;
		*(u16 *)(void *)((u8 *)req + offset) = htole16(flush_time_out);
		offset += L2CAP_OPT_FLUSH_TIMO_SIZE;
	}

	return enqueue_to_bulk_in_msg_ready_queue(msg);
}

int l2cap_send_config_rsp(u16 hci_con_handle, u16 remote_cid, u8 ident, const u8 *options, u32 options_len)
{
	ipcmessage *msg;
	l2cap_cfg_rsp_cp *req;

	msg = alloc_l2cap_cmd_msg((void **)&req, hci_con_handle, L2CAP_CONFIG_RSP,
				  ident, sizeof(*req) + options_len);
	if (!msg)
		return IOS_ENOMEM;

	/* Fill message data */
	req->scid = htole16(remote_cid);
	req->flags = htole16(0);
	req->result = htole16(L2CAP_SUCCESS);
	if (options && options_len > 0)
		memcpy((u8 *)req + sizeof(*req), options, options_len);

	return enqueue_to_bulk_in_msg_ready_queue(msg);
}

/* HCI event enqueue (injection) helpers */

static inline int enqueue_to_usb_intr_msg_ready_queue(ipcmessage *msg)
{
	return handle_bulk_intr_ready_message(msg, msg->ioctlv.vector[2].len,
					      pending_usb_intr_msg_queue_id,
					      ready_usb_intr_msg_queue_id);
}

static ipcmessage *alloc_hci_event_msg(void **event_payload, u8 event, u8 event_size)
{
	hci_event_hdr_t *hdr;
	ipcmessage *msg = alloc_inject_message((void **)&hdr, sizeof(*hdr) + event_size);
	if (!msg)
		return NULL;

	/* Fill the header */
	hdr->event = event;
	hdr->length = event_size;
	*event_payload = (u8 *)hdr + sizeof(*hdr);

	return msg;
}

int enqueue_hci_event_command_status(u16 opcode)
{
	hci_command_status_ep *ep;
	ipcmessage *msg = alloc_hci_event_msg((void *)&ep, HCI_EVENT_COMMAND_STATUS, sizeof(*ep));
	if (!msg)
		return IOS_ENOMEM;

	/* Fill event data */
	ep->status = 0;
	ep->num_cmd_pkts = 1;
	ep->opcode = htole16(opcode);

	return enqueue_to_usb_intr_msg_ready_queue(msg);
}

int enqueue_hci_event_command_compl(u16 opcode, const void *payload, u32 payload_size)
{
	hci_command_compl_ep *ep;
	ipcmessage *msg = alloc_hci_event_msg((void *)&ep, HCI_EVENT_COMMAND_COMPL,
					      sizeof(*ep) + payload_size);
	if (!msg)
		return IOS_ENOMEM;

	/* Fill event data */
	ep->num_cmd_pkts = 1;
	ep->opcode = htole16(opcode);
	if (payload && payload_size > 0)
		memcpy((u8 *)ep + sizeof(*ep), payload, payload_size);

	return enqueue_to_usb_intr_msg_ready_queue(msg);
}

int enqueue_hci_event_con_req(const bdaddr_t *bdaddr, u8 uclass0, u8 uclass1, u8 uclass2, u8 link_type)
{
	hci_con_req_ep *ep;
	ipcmessage *msg = alloc_hci_event_msg((void *)&ep, HCI_EVENT_CON_REQ, sizeof(*ep));
	if (!msg)
		return IOS_ENOMEM;

	/* Fill event data */
	ep->bdaddr = *bdaddr;
	ep->uclass[0] = uclass0;
	ep->uclass[1] = uclass1;
	ep->uclass[2] = uclass2;
	ep->link_type = link_type;

	return enqueue_to_usb_intr_msg_ready_queue(msg);
}

int enqueue_hci_event_con_compl(const bdaddr_t *bdaddr, u16 con_handle, u8 status)
{
	hci_con_compl_ep *ep;
	ipcmessage *msg = alloc_hci_event_msg((void *)&ep, HCI_EVENT_CON_COMPL, sizeof(*ep));
	if (!msg)
		return IOS_ENOMEM;

	/* Fill event data */
	ep->status = status;
	ep->con_handle = htole16(con_handle);
	ep->bdaddr = *bdaddr;
	ep->link_type = HCI_LINK_ACL;
	ep->encryption_mode = HCI_ENCRYPTION_MODE_NONE;

	return enqueue_to_usb_intr_msg_ready_queue(msg);
}

int enqueue_hci_event_role_change(const bdaddr_t *bdaddr, u8 role)
{
	hci_role_change_ep *ep;
	ipcmessage *msg = alloc_hci_event_msg((void *)&ep, HCI_EVENT_ROLE_CHANGE, sizeof(*ep));
	if (!msg)
		return IOS_ENOMEM;

	/* Fill event data */
	ep->status = 0;
	ep->bdaddr = *bdaddr;
	ep->role = role;

	return enqueue_to_usb_intr_msg_ready_queue(msg);
}

/* Main IOCTLV handler */

static int handle_oh1_dev_ioctlv(ipcmessage *recv_msg, ipcmessage **ret_msg, u32 cmd,
				 ioctlv *vector, u32 inlen, u32 iolen, int *fwd_to_usb)
{
	int ret = 0;
	void *data;
	u16 wLength;
	u8 bEndpoint, bRequest;

	/* Invalidate cache */
	InvalidateVector(vector, inlen, iolen);

	switch (cmd) {
	case USBV0_IOCTLV_CTRLMSG: {
		bRequest = *(u8 *)vector[1].data;
		if (bRequest == EP_HCI_CTRL) {
			wLength = le16toh(*(u16 *)vector[4].data);
			data    = vector[6].data;
			hci_state_handle_hci_cmd_from_host(data, wLength, fwd_to_usb);
			/* If we don't have to hand it down, we can already ACK it */
			if (!*fwd_to_usb)
				ret = os_message_queue_ack(recv_msg, wLength);
		}
		break;
	}
	case USBV0_IOCTLV_BLKMSG: {
		bEndpoint = *(u8 *)vector[0].data;
		if (bEndpoint == EP_ACL_DATA_OUT) {
			/* This is the ACL datapath from CPU to device (Wiimote) */
			wLength = *(u16 *)vector[1].data;
			data    = vector[2].data;
			hci_state_handle_acl_data_out_request_from_host(data, wLength, fwd_to_usb);
			/* If we don't have to hand it down, we can already ACK it */
			if (!*fwd_to_usb) {
				ret = os_message_queue_ack(recv_msg, wLength);
			}
		} else if (bEndpoint == EP_ACL_DATA_IN) {
			/* We are given an ACL buffer to fill */
			wLength = *(u16 *)vector[1].data;
			hci_state_handle_acl_data_in_request_from_host(wLength);
			ret = handle_bulk_intr_pending_message(recv_msg, wLength, ret_msg,
							       ready_usb_bulk_in_msg_queue_id,
							       pending_usb_bulk_in_msg_queue_id,
							       &usb_bulk_in_msg_handed_to_oh1,
							       fwd_to_usb);
		}
		break;
	}
	case USBV0_IOCTLV_INTRMSG: {
		bEndpoint = *(u8 *)vector[0].data;
		if (bEndpoint == EP_HCI_EVENT) {
			wLength = *(u16 *)vector[1].data;
			/* We are given a HCI buffer to fill */
			hci_state_handle_hci_event_request_from_host(wLength);
			ret = handle_bulk_intr_pending_message(recv_msg, wLength, ret_msg,
							       ready_usb_intr_msg_queue_id,
							       pending_usb_intr_msg_queue_id,
							       &usb_intr_msg_handed_to_oh1,
							       fwd_to_usb);
		}
		break;
	}
	default:
		/* Unhandled/unknown ioctls are forwarded to the OH1 module */
		printf("Unhandled IOCTL: 0x%x\n", cmd);
		break;
	}

	return ret;
}

/* PendingQ / ReadyQ helpers */

static inline void copy_ipcmessage_bulk_intr_io_data(ipcmessage *dst, const ipcmessage *src, u32 len)
{
	void *src_data = src->ioctlv.vector[2].data;
	void *dst_data = dst->ioctlv.vector[2].data;

	memcpy(dst_data, src_data, len);
	os_sync_after_write(dst_data, len);
}

static int configure_usb_bulk_intr_msg_handed_to_oh1(ipcmessage *handed_down_msg, u32 fd,
						     u16 min_wLength)
{
	u16 *p_wLength = handed_down_msg->ioctlv.vector[1].data;
	u32 *p_len = &handed_down_msg->ioctlv.vector[2].len;
	void **p_data = &handed_down_msg->ioctlv.vector[2].data;

	/* If the message has no data allocated, or it's smaller than the required... */
	if (*p_data == NULL || *p_wLength < min_wLength) {
		if (*p_data != NULL)
			Mem_Free(*p_data);

		*p_data = Mem_Alloc(min_wLength);
		if (*p_data == NULL)
			return IOS_ENOMEM;
		*p_wLength = min_wLength;
		*p_len = *p_wLength;
	}

	handed_down_msg->fd = fd;

	os_sync_before_read(*p_data, *p_len);

	return IOS_OK;
}

#if 0
/* Used to allocate ipcmessages to hand down to OH1 */
static ipcmessage *clone_bulk_in_intr_ipcmessage(const ipcmessage *src)
{
	ipcmessage *dst;
	u16 data_size = *(u16 *)src->ioctlv.vector[1].data;
	u32 data_offset = ROUNDUP32(sizeof(*dst) + 3 * sizeof(ioctlv) + sizeof(u8) + sizeof(u16));
	u32 alloc_size = data_offset + data_size;

	dst = os_heap_alloc(ipcmessages_heap_id, alloc_size);
	assert(dst != NULL);
	if (!dst)
		return NULL;

	dst->command               = src->command;
	dst->result                = src->result;
	dst->fd                    = src->fd;
	dst->ioctlv.command        = src->ioctlv.command;
	dst->ioctlv.num_in         = src->ioctlv.num_in;
	dst->ioctlv.num_io         = src->ioctlv.num_io;
	dst->ioctlv.vector         = (void *)((u8 *)dst + sizeof(ipcmessage));
	dst->ioctlv.vector[1].data = (void *)((u8 *)dst->ioctlv.vector + 3 * sizeof(ioctlv));
	dst->ioctlv.vector[1].len  = src->ioctlv.vector[1].len;
	dst->ioctlv.vector[0].data = (void *)((u8 *)dst->ioctlv.vector[1].data + sizeof(u16));
	dst->ioctlv.vector[0].len  = src->ioctlv.vector[0].len;
	dst->ioctlv.vector[2].data = (void *)((u8 *)dst + data_offset);
	dst->ioctlv.vector[2].len  = src->ioctlv.vector[2].len;

	/* bEndpoint */
	*(u8 *)dst->ioctlv.vector[0].data = *(u8 *)src->ioctlv.vector[0].data;
	/* wLength */
	*(u16 *)dst->ioctlv.vector[1].data = *(u16 *)src->ioctlv.vector[1].data;

	/* Invalidate the data before the USB DMA in case we had dirty lines on the Dcache */
	os_sync_before_read(dst->ioctlv.vector[2].data, dst->ioctlv.vector[2].len);

	return dst;
}

static inline bool is_message_ours(const ipcmessage *msg)
{
	return ((uintptr_t)msg >= (uintptr_t)ipcmessages_heap_data) &&
	       ((uintptr_t)msg < ((uintptr_t)ipcmessages_heap_data + sizeof(ipcmessages_heap_data)));
}
#endif

static inline bool is_message_injected(const ipcmessage *msg)
{
	return msg->command == INJECTED_IPCMESSAGE_COOKIE;
}

static int handle_bulk_intr_pending_message(ipcmessage *pend_msg, u16 size, ipcmessage **ret_msg,
					    int ready_queue_id, int pending_queue_id,
					    ipcmessage *hand_down_msg, int *fwd_to_usb)
{
	int ret;
	ipcmessage *ready_msg;

	/* Fast-path: check if we already have a message ready to be delivered */
	ret = os_message_queue_receive(ready_queue_id, &ready_msg, IOS_MESSAGE_NOBLOCK);
	if (ret == IOS_OK) {
		/* Copy message data if the return value is positive */
		if (*(int *)&ready_msg->result > 0)
			copy_ipcmessage_bulk_intr_io_data(pend_msg, ready_msg, ready_msg->result);
		/* Finally, we can ACK the message! */
		ret = os_message_queue_ack(pend_msg, ready_msg->result);
		/* If it was a message we injected ourselves, we have to deallocate it */
		if (is_message_injected(ready_msg))
			os_heap_free(ipcmessages_heap_id, ready_msg);
		/* We have already ACK it, we don't have to hand it down to OH1 */
		*fwd_to_usb = 0;
	} else {
		/* Hand down to OH1 a copy of the message for it to fill it from real USB data */
		ret = configure_usb_bulk_intr_msg_handed_to_oh1(hand_down_msg, pend_msg->fd, size);
		if (ret == IOS_OK)
			*ret_msg = hand_down_msg;
		/* Push the received message to the PendingQ */
		ret = os_message_queue_send(pending_queue_id, pend_msg, IOS_MESSAGE_NOBLOCK);
	}

	return ret;
}

static int handle_bulk_intr_ready_message(ipcmessage *ready_msg, int retval, int pending_queue_id,
					  int ready_queue_id)
{
	int ret;
	ipcmessage *pend_msg;

	/* Fast-path: check if we have a PendingQ message to fill */
	ret = os_message_queue_receive(pending_queue_id, &pend_msg, IOS_MESSAGE_NOBLOCK);
	if (ret == IOS_OK) {
		/* If retval is positive, it contains the data size, an error otherwise */
		if (retval > 0)
			copy_ipcmessage_bulk_intr_io_data(pend_msg, ready_msg, retval);
		/* Finally we can ACK the message! */
		ret = os_message_queue_ack(pend_msg, retval);
		assert(ret == IOS_OK);
		/* If it was a message we injected ourselves, we have to deallocate it */
		if (is_message_injected(ready_msg))
			os_heap_free(ipcmessages_heap_id, ready_msg);
	} else {
		/* Push message to ReadyQ. We store the return value/size to the "result" field */
		*(int *)&ready_msg->result = retval;
		ret = os_message_queue_send(ready_queue_id, ready_msg, IOS_MESSAGE_NOBLOCK);
	}

	return ret;
}

/* Hooked functions */

static int OH1_IOS_ReceiveMessage_hook(int queueid, ipcmessage **ret_msg, u32 flags)
{
	int ret;
	ipcmessage *recv_msg;
	int fwd_to_usb;

	/* We don't care about other queues... */
	if (queueid != orig_msg_queueid)
		return os_message_queue_receive(queueid, (void *)ret_msg, flags);

	ensure_init_oh1_context();

	while (1) {
		ret = os_message_queue_receive(queueid, (void *)&recv_msg, flags);
		if (ret != IOS_OK) {
			printf("Message queue recv err: %d\n", ret);
			break;
		} else if (recv_msg == (ipcmessage *)0xcafef00d) {
			*ret_msg = (ipcmessage *)0xcafef00d;
			break;
		} else if (recv_msg == (void *)&periodic_timer_cookie) {
			fakedev_tick_devices();
			fwd_to_usb = 0;
		} else {
			*ret_msg = NULL;
			/* Default to yes (few commands return fake data to BT SW stack) */
			fwd_to_usb = 1;

			switch (recv_msg->command) {
			case IOS_OPEN:
				break;
			case IOS_CLOSE:
				break;
			case IOS_READ:
				break;
			case IOS_WRITE:
				break;
			case IOS_SEEK:
				break;
			case IOS_IOCTLV: {
				ioctlv *vector = recv_msg->ioctlv.vector;
				u32     inlen  = recv_msg->ioctlv.num_in;
				u32     iolen  = recv_msg->ioctlv.num_io;
				u32     cmd    = recv_msg->ioctlv.command;
				ret = handle_oh1_dev_ioctlv(recv_msg, ret_msg, cmd, vector,
							    inlen, iolen, &fwd_to_usb);
				break;
			}
			default:
				/* Unknown command */
				printf("Unhandled IPC command: 0x%x\n", recv_msg->command);
				break;
			}
		}

		/* Break the loop and return from the hook if we want
		 * to deliver the message to the BT USB dongle */
		if (fwd_to_usb) {
			/* Just send the original message we received if we don't
			 * want to hand down a "private" message to OH1 */
			if (*ret_msg == NULL)
				*ret_msg = recv_msg;
			break;
		}
	}

	return ret;
}

static int OH1_IOS_ResourceReply_hook(ipcmessage *ready_msg, int retval)
{
	int ret;
	ioctlv *vector;

	if (ready_msg == &usb_intr_msg_handed_to_oh1) {
		ensure_init_oh1_context();
		vector = ready_msg->ioctlv.vector;
		assert(ready_msg->command == IOS_IOCTLV);
		assert(ready_msg->ioctlv.command == USBV0_IOCTLV_INTRMSG);
		/* Let the HCI tracker know about this HCI event response coming from OH1 */
		hci_state_handle_hci_event_from_controller(vector[2].data,
							   *(u16 *)vector[1].data);

		ret = handle_bulk_intr_ready_message(ready_msg, retval,
						     pending_usb_intr_msg_queue_id,
						     ready_usb_intr_msg_queue_id);
		return ret;
	} else if (ready_msg == &usb_bulk_in_msg_handed_to_oh1) {
		ensure_init_oh1_context();
		vector = ready_msg->ioctlv.vector;
		assert(ready_msg->command == IOS_IOCTLV);
		assert(ready_msg->ioctlv.command == USBV0_IOCTLV_BLKMSG);
		/* Let the HCI tracker know about this HCI ACL IN response coming from OH1 */
		hci_state_handle_acl_data_in_response_from_controller(vector[2].data,
							   *(u16 *)vector[1].data);

		ret = handle_bulk_intr_ready_message(ready_msg, retval,
						     pending_usb_bulk_in_msg_queue_id,
						     ready_usb_bulk_in_msg_queue_id);
		return ret;
	}

	return os_message_queue_ack(ready_msg, retval);
}

static int ensure_init_oh1_context(void)
{
	static int initialized = 0;
	int ret;

	if (!initialized) {
		/* Message queues can only be used on the process they were created in */
		ret = os_message_queue_create(ready_usb_intr_msg_queue_data,
					      ARRAY_SIZE(ready_usb_intr_msg_queue_data));
		if (ret < 0)
			return ret;
		ready_usb_intr_msg_queue_id = ret;

		ret = os_message_queue_create(pending_usb_intr_msg_queue_data,
					      ARRAY_SIZE(pending_usb_intr_msg_queue_data));
		if (ret < 0)
			return ret;
		pending_usb_intr_msg_queue_id = ret;

		ret = os_message_queue_create(ready_usb_bulk_in_msg_queue_data,
					      ARRAY_SIZE(ready_usb_bulk_in_msg_queue_data));
		if (ret < 0)
			return ret;
		ready_usb_bulk_in_msg_queue_id = ret;

		ret = os_message_queue_create(pending_usb_bulk_in_msg_queue_data,
					      ARRAY_SIZE(pending_usb_bulk_in_msg_queue_data));
		if (ret < 0)
			return ret;
		pending_usb_bulk_in_msg_queue_id = ret;

		periodic_timer_id = os_create_timer(PERIODC_TIMER_PERIOD, PERIODC_TIMER_PERIOD,
						    orig_msg_queueid, (u32)&periodic_timer_cookie);
		if (ret < 0)
			return ret;

		initialized = 1;
	}

	return 0;
}

static s32 Patch_OH1UsbModule(void)
{
	u32 addr_recv;
	u32 addr_reply;

	/* Check version */
	u32 bytes = *(u16 *)OH1_IOS_ReceiveMessage_ADDR1;
	if (bytes == 0x4778) {
		addr_recv = OH1_IOS_ReceiveMessage_ADDR1;
		addr_reply = OH1_IOS_ResourceReply_ADDR1;
	} else if (bytes == 0xbd00) {
		addr_recv = OH1_IOS_ReceiveMessage_ADDR2;
		addr_reply = OH1_IOS_ResourceReply_ADDR2;
	} else {
		return IOS_ENOENT;
	}

	/* Get original /dev/usb/oh1 queueid */
	orig_msg_queueid = *(int *)OH1_DEV_OH1_QUEUEID_ADDR;

	/* Patch IOS_ReceiveMessage syscall wrapper to jump to our function */
	DCWrite32(addr_recv, 0x4B004718);
	DCWrite32(addr_recv + 4, (u32)OH1_IOS_ReceiveMessage_hook);

	/* Patch IOS_ResourceReply syscall wrapper to jump to our function */
	DCWrite32(addr_reply, 0x4B004718);
	DCWrite32(addr_reply + 4, (u32)OH1_IOS_ResourceReply_hook);

	return 0;
}

int main(void)
{
	int ret;

	/* Print info */
	svc_write("Hello world from Starlet!\n");

	/* Initialize global memory heap */
	ret = Mem_Init(heapspace, sizeof(heapspace));
	if (ret < 0)
		return ret;

	/* Initialize heaps for inject and hand down messages */
	ret = os_heap_create(ipcmessages_heap_data, sizeof(ipcmessages_heap_data));
	if (ret < 0)
		return ret;
	ipcmessages_heap_id = ret;

	/* Initialize global state */
	hci_state_init();
	fakedev_init();

#if 0
	static u8 conf_buffer[0x4000] ATTRIBUTE_ALIGN(32);
	static struct conf_pads_setting conf_pads ATTRIBUTE_ALIGN(32);

	int fd = os_open("/shared2/sys/SYSCONF", IOS_OPEN_READ);
	os_read(fd, conf_buffer, sizeof(conf_buffer));
	os_close(fd);

	ret = conf_get(conf_buffer, "BT.DINF", &conf_pads, sizeof(conf_pads));
	printf("conf_get(): %d\n", ret);
	printf("  num_registered: %d\n", conf_pads.num_registered);
	printf("  registered[0]: %s\n", conf_pads.registered[0].name);
	printf("  registered[1]: %s\n", conf_pads.registered[1].name);

	bdaddr_t bdaddr = {.b = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
	for (int i = 0; i < 6; i++)
		conf_pads.registered[1].bdaddr[i] = bdaddr.b[5 - i];
	conf_pads.num_registered = 2;
	memset(conf_pads.registered[1].name, 0, sizeof(conf_pads.registered[1].name));
	strcpy(conf_pads.registered[1].name, "UwU");
	conf_set(conf_buffer, "BT.DINF", &conf_pads, sizeof(conf_pads));

	fd = os_open("/shared2/sys/SYSCONF", IOS_OPEN_WRITE);
	os_write(fd, conf_buffer, sizeof(conf_buffer));
	os_close(fd);
#endif

	/* System patchers */
	patcher patchers[] = {
		{Patch_OH1UsbModule, 0},
	};

	/* Initialize plugin with patchers */
	ret = IOS_InitSystem(patchers, sizeof(patchers));
	printf("IOS_InitSystem(): %d\n", ret);

	return 0;
}

void my_assert_func(const char *file, int line, const char *func, const char *failedexpr)
{
	printf("assertion \"%s\" failed: file \"%s\", line %d%s%s\n",
		failedexpr, file, line, func ? ", function: " : "", func ? func : "");
}