#include <stdio.h>
#include <string.h>
#include "fake_wiimote_mgr.h"
#include "input_device.h"
#include "ipc.h"
#include "usb.h"
#include "usb_device_drivers.h"
#include "usb_hid.h"
#include "syscalls.h"
#include "utils.h"
#include "wiimote.h"

/* USBv5 USB_HID IOCTL commands */
#define USBV5_IOCTL_GETVERSION                   0
#define USBV5_IOCTL_GETDEVICECHANGE              1
#define USBV5_IOCTL_SHUTDOWN                     2
#define USBV5_IOCTL_GETDEVPARAMS                 3
#define USBV5_IOCTL_ATTACH                       4
#define USBV5_IOCTL_RELEASE                      5
#define USBV5_IOCTL_ATTACHFINISH                 6
#define USBV5_IOCTL_SETALTERNATE                 7
#define USBV5_IOCTL_SUSPEND_RESUME              16
#define USBV5_IOCTL_CANCELENDPOINT              17
#define USBV5_IOCTL_CTRLMSG                     18
#define USBV5_IOCTL_INTRMSG                     19
#define USBV5_IOCTL_ISOMSG                      20 /* Not available in USB_HID */
#define USBV5_IOCTL_BULKMSG                     21 /* Not available in USB_HID */

#define USBV4_IOCTL_GETDEVICECHANGE				0
#define USBV4_IOCTL_SETSUSPEND					1
#define USBV4_IOCTL_CONTROLMESSAGE				2
#define USBV4_IOCTL_INTERRUPTMESSAGEIN			3
#define USBV4_IOCTL_INTERRUPTMESSAGEOUT			4
#define USBV4_IOCTL_GETUSSTRING					5
#define USBV4_IOCTL_GETVERSION					6
#define USBV4_IOCTL_SHUTDOWN					7
#define USBV4_IOCTL_CANCELINTERRUPT				8

/* Constants */
#define USB_MAX_DEVICES		32
#define DEV_USB_HID4_DEVICE_CHANGE_SIZE 0x180

/* USBv5 HID message structure */
struct usb_hid_v5_transfer {
	u32 dev_id;
	u32 zero;

	union {
		struct {
			u8 bmRequestType;
			u8 bmRequest;
			u16 wValue;
			u16 wIndex;
		} ctrl;

		struct {
			u32 out;
		} intr;

		u32 data[14];
	};
} ATTRIBUTE_PACKED;


struct usb_hid_v4_transfer {
	u8 padding[16]; // anything you want can go here
	s32 dev_id;
	union {
		struct {
			u8 bmRequestType;
			u8 bmRequest;
			u16 wValue;
			u16 wIndex;
			u16 wLength;
		} ctrl;
		struct {
			u32 out;
			u32 dLength;
		} intr;
	};
	void* data; // virtual pointer, not physical!
} ATTRIBUTE_PACKED; // 32 bytes

static_assert(sizeof(struct usb_hid_v5_transfer) == 64);
static_assert(sizeof(struct usb_hid_v4_transfer) == 32);

static const usb_device_driver_t *usb_device_drivers[] = {
	&ds3_usb_device_driver,
	&ds4_usb_device_driver,
	&gh_guitar_usb_device_driver,
	&gh_drum_usb_device_driver,
	&turntable_usb_device_driver


};

static usb_input_device_t usb_devices[MAX_FAKE_WIIMOTES];
static usb_device_entry device_change_devices[USB_MAX_DEVICES] ATTRIBUTE_ALIGN(32);
static uint32_t device_change_v4_devices[DEV_USB_HID4_DEVICE_CHANGE_SIZE] ATTRIBUTE_ALIGN(32);
static int host_fd = -1;
static u8 worker_thread_stack[1024] ATTRIBUTE_ALIGN(32);
static u32 queue_data[32] ATTRIBUTE_ALIGN(32);
static int queue_id = -1;
static bool using_v4_api = false;
static u8 v4_data[128] ATTRIBUTE_ALIGN(32);

/* Async notification messages */
static areply notification_messages[2] = {0};
#define MESSAGE_DEVCHANGE	&notification_messages[0]
#define MESSAGE_ATTACHFINISH	&notification_messages[1]

static inline usb_input_device_t *get_usb_device_for_dev_id(u32 dev_id)
{
	for (int i = 0; i < ARRAY_SIZE(usb_devices); i++) {
		if (usb_devices[i].valid && (usb_devices[i].dev_id == dev_id))
			return &usb_devices[i];
	}

	return NULL;
}

static inline usb_input_device_t *get_free_usb_device_slot(void)
{
	for (int i = 0; i < ARRAY_SIZE(usb_devices); i++) {
		if (!usb_devices[i].valid)
			return &usb_devices[i];
	}

	return NULL;
}

static inline bool is_usb_device_connected(u32 dev_id)
{
	return get_usb_device_for_dev_id(dev_id) != NULL;
}

static inline const usb_device_driver_t *get_usb_device_driver_for(u16 vid, u16 pid)
{
	for (int i = 0; i < ARRAY_SIZE(usb_device_drivers); i++) {
		if (usb_device_drivers[i]->probe(vid, pid))
			return usb_device_drivers[i];
	}

	return NULL;
}

static int usb_hid_v5_get_descriptors(int host_fd, u32 dev_id, usb_devdesc *udd)
{
	u32 inbuf[8] ATTRIBUTE_ALIGN(32);
	u8 outbuf[96] ATTRIBUTE_ALIGN(32);

	/* Setup buffer */
	inbuf[0] = dev_id;
	inbuf[2] = 0;

	/* Get device parameters */
	return os_ioctl(host_fd, USBV5_IOCTL_GETDEVPARAMS, inbuf, sizeof(inbuf), outbuf, sizeof(outbuf));
}

static inline void build_v5_ctrl_transfer(struct usb_hid_v5_transfer *transfer, int dev_id,
				       u8 bmRequestType, u8 bmRequest, u16 wValue, u16 wIndex)
{
	memset(transfer, 0, sizeof(*transfer));
	transfer->dev_id = dev_id;
	transfer->ctrl.bmRequestType = bmRequestType;
	transfer->ctrl.bmRequest = bmRequest;
	transfer->ctrl.wValue = wValue;
	transfer->ctrl.wIndex = wIndex;
}

static inline void build_v5_intr_transfer(struct usb_hid_v5_transfer *transfer, int dev_id, int out)
{
	memset(transfer, 0, sizeof(*transfer));
	transfer->dev_id = dev_id;
	transfer->intr.out = out;
}

static inline int usb_hid_v5_ctrl_transfer(int host_fd, int dev_id, u8 bmRequestType, u8 bmRequest,
					   u16 wValue, u16 wIndex, u16 wLength, void *rpData)
{
	struct usb_hid_v5_transfer transfer ATTRIBUTE_ALIGN(32);
	ioctlv vectors[2];
	int out = !(bmRequestType & USB_ENDPOINT_IN);

	build_v5_ctrl_transfer(&transfer, dev_id, bmRequestType, bmRequest, wValue, wIndex);

	vectors[0].data = &transfer;
	vectors[0].len  = sizeof(transfer);
	vectors[1].data = rpData;
	vectors[1].len  = wLength;

	return os_ioctlv(host_fd, USBV5_IOCTL_CTRLMSG, 1 - out, 1 + out, vectors);
}

static inline int usb_hid_v5_ctrl_transfer_async(int host_fd, int dev_id, u8 bmRequestType,
				                 u8 bmRequest, u16 wValue, u16 wIndex, u16 wLength,
				                 void *rpData, int queue_id, void *message)
{
	struct usb_hid_v5_transfer transfer ATTRIBUTE_ALIGN(32);
	ioctlv vectors[2];
	int out = !(bmRequestType & USB_ENDPOINT_IN);

	build_v5_ctrl_transfer(&transfer, dev_id, bmRequestType, bmRequest, wValue, wIndex);

	vectors[0].data = &transfer;
	vectors[0].len  = sizeof(transfer);
	vectors[1].data = rpData;
	vectors[1].len  = wLength;

	return os_ioctlv_async(host_fd, USBV5_IOCTL_CTRLMSG, 1 - out, 1 + out, vectors,
			       queue_id, message);
}

static inline int usb_hid_v5_intr_transfer(int host_fd, int dev_id, int out, u16 wLength, void *rpData)
{
	struct usb_hid_v5_transfer transfer ATTRIBUTE_ALIGN(32);
	ioctlv vectors[2];

	build_v5_intr_transfer(&transfer, dev_id, out);

	vectors[0].data = &transfer;
	vectors[0].len  = sizeof(transfer);
	vectors[1].data = rpData;
	vectors[1].len  = wLength;

	return os_ioctlv(host_fd, USBV5_IOCTL_INTRMSG, 1 - out, 1 + out, vectors);
}

static inline int usb_hid_v5_intr_transfer_async(int host_fd, int dev_id, int out, u16 wLength,
						 void *rpData, int queue_id, void *message)
{
	struct usb_hid_v5_transfer transfer ATTRIBUTE_ALIGN(32);
	ioctlv vectors[2];

	build_v5_intr_transfer(&transfer, dev_id, out);

	vectors[0].data = &transfer;
	vectors[0].len  = sizeof(transfer);
	vectors[1].data = rpData;
	vectors[1].len  = wLength;

	return os_ioctlv_async(host_fd, USBV5_IOCTL_INTRMSG, 1 - out, 1 + out, vectors,
			       queue_id, message);
}

static int usb_hid_v5_attach(int host_fd, u32 dev_id)
{
	u32 buf[8] ATTRIBUTE_ALIGN(32);

	memset(buf, 0, sizeof(buf));
	buf[0] = dev_id;

	return os_ioctl(host_fd, USBV5_IOCTL_ATTACH, buf, sizeof(buf), NULL, 0);
}

static int usb_hid_v5_release(int host_fd, u32 dev_id)
{
	u32 buf[8] ATTRIBUTE_ALIGN(32);

	memset(buf, 0, sizeof(buf));
	buf[0] = dev_id;

	return os_ioctl(host_fd, USBV5_IOCTL_RELEASE, buf, sizeof(buf), NULL, 0);
}

static int usb_hid_v5_suspend_resume(int host_fd, int dev_id, int resumed, u32 unk)
{
	u32 buf[8] ATTRIBUTE_ALIGN(32);

	memset(buf, 0, sizeof(buf));
	buf[0] = dev_id;
	buf[2] = unk;
	*(u8 *)((u8 *)buf + 0xb) = resumed;

	return os_ioctl(host_fd, USBV5_IOCTL_SUSPEND_RESUME, buf, sizeof(buf), NULL, 0);
}

static inline void build_v4_ctrl_transfer(struct usb_hid_v4_transfer *transfer, int dev_id,
				       u8 bmRequestType, u8 bmRequest, u16 wValue, u16 wIndex, u16 wLength, void *rpData)
{
	memset(transfer, 0, sizeof(*transfer));
	transfer->dev_id = dev_id;
	transfer->ctrl.bmRequestType = bmRequestType;
	transfer->ctrl.bmRequest = bmRequest;
	transfer->ctrl.wValue = wValue;
	transfer->ctrl.wIndex = wIndex;
	transfer->ctrl.wLength = wLength;
	transfer->data = v4_data;
	memcpy(v4_data, rpData, wLength);
}

static inline void build_v4_intr_transfer(struct usb_hid_v4_transfer *transfer, int dev_id, int out, int dLength, void *rpData)
{
	memset(transfer, 0, sizeof(*transfer));
	transfer->dev_id = dev_id;
	transfer->intr.out = out;
	transfer->intr.dLength = dLength;
	transfer->data = v4_data;
	memcpy(v4_data, rpData, dLength);
}

static inline int usb_hid_v4_ctrl_transfer(int host_fd, int dev_id, u8 bmRequestType, u8 bmRequest,
					   u16 wValue, u16 wIndex, u16 wLength, void *rpData)
{
	struct usb_hid_v4_transfer transfer ATTRIBUTE_ALIGN(32);

	build_v4_ctrl_transfer(&transfer, dev_id, bmRequestType, bmRequest, wValue, wIndex, wLength, rpData);
	return os_ioctl(host_fd, USBV4_IOCTL_CONTROLMESSAGE, &transfer, sizeof(transfer), NULL, 0);
	
}

static inline int usb_hid_v4_ctrl_transfer_async(int host_fd, int dev_id, u8 bmRequestType,
				                 u8 bmRequest, u16 wValue, u16 wIndex, u16 wLength,
				                 void *rpData, int queue_id, void *message)
{
	struct usb_hid_v4_transfer transfer ATTRIBUTE_ALIGN(32);

	build_v4_ctrl_transfer(&transfer, dev_id, bmRequestType, bmRequest, wValue, wIndex, wLength, rpData);

	return os_ioctl_async(host_fd, USBV4_IOCTL_CONTROLMESSAGE, &transfer, sizeof(transfer), NULL, 0,
			       queue_id, message);
}

static inline int usb_hid_v4_intr_transfer(int host_fd, int dev_id, int out, u16 wLength, void *rpData)
{
	struct usb_hid_v4_transfer transfer ATTRIBUTE_ALIGN(32);


	int is_out = !(out & USB_ENDPOINT_IN);

	build_v4_intr_transfer(&transfer, dev_id, out, wLength, rpData);
	
	if (is_out) {
		return os_ioctl(host_fd, USBV4_IOCTL_INTERRUPTMESSAGEOUT,  &transfer, sizeof(transfer), NULL, 0);
	}
	return os_ioctl(host_fd, USBV4_IOCTL_INTERRUPTMESSAGEIN,  &transfer, sizeof(transfer), NULL, 0);
}

static inline int usb_hid_v4_intr_transfer_async(int host_fd, int dev_id, int out, u16 wLength,
						 void *rpData, int queue_id, void *message)
{
	struct usb_hid_v4_transfer transfer ATTRIBUTE_ALIGN(32);

	int is_out = !(out & USB_ENDPOINT_IN);
	build_v4_intr_transfer(&transfer, dev_id, out, wLength, rpData);
	
	if (is_out) {
		return os_ioctl_async(host_fd, USBV4_IOCTL_INTERRUPTMESSAGEOUT, &transfer, sizeof(transfer), NULL, 0,
			       queue_id, message);
	}
	return os_ioctl_async(host_fd, USBV4_IOCTL_INTERRUPTMESSAGEIN, &transfer, sizeof(transfer), NULL, 0,
			       queue_id, message);
}

/* API exposed to USB device drivers */
int usb_device_driver_issue_ctrl_transfer(usb_input_device_t *device, u8 requesttype, u8 request,
					  u16 value, u16 index, void *data, u16 length)
{
	if (using_v4_api) {
		return usb_hid_v4_ctrl_transfer(device->host_fd, device->dev_id, requesttype, request,
					value, index, length, data);

	}
	return usb_hid_v5_ctrl_transfer(device->host_fd, device->dev_id, requesttype, request,
					value, index, length, data);
}

int usb_device_driver_issue_intr_transfer(usb_input_device_t *device, int out, void *data, u16 length)
{
	if (using_v4_api) {
		return usb_hid_v4_intr_transfer(device->host_fd, device->dev_id, out, length, data);
	}
	return usb_hid_v5_intr_transfer(device->host_fd, device->dev_id, out, length, data);
}

int usb_device_driver_issue_ctrl_transfer_async(usb_input_device_t *device, u8 requesttype,
						u8 request, u16 value, u16 index, void *data, u16 length)
{
	if (using_v4_api) {
		return usb_hid_v4_ctrl_transfer_async(device->host_fd, device->dev_id, requesttype, request,
					      value, index, length, data, queue_id,
					      &device->usb_async_resp_msg);
	}
	return usb_hid_v5_ctrl_transfer_async(device->host_fd, device->dev_id, requesttype, request,
					      value, index, length, data, queue_id,
					      &device->usb_async_resp_msg);
}

int usb_device_driver_issue_intr_transfer_async(usb_input_device_t *device, int out, void *data, u16 length)
{
	if (using_v4_api) {
		return usb_hid_v4_intr_transfer_async(device->host_fd, device->dev_id, out, length, data,
					      queue_id, &device->usb_async_resp_msg);
	}
	return usb_hid_v5_intr_transfer_async(device->host_fd, device->dev_id, out, length, data,
					      queue_id, &device->usb_async_resp_msg);
}

static int usb_device_ops_resume(void *usrdata, fake_wiimote_t *wiimote)
{
	usb_input_device_t *device = usrdata;

	DEBUG("usb_device_ops_resume\n");

	if (device->suspended) {
		/* FIXME: Doesn't work properly with DS3.
		 * It doesn't report any data after suspend+resume... */
#if 0
		if (usb_hid_v5_suspend_resume(device->host_fd, device->dev_id, 1, 0) != IOS_OK)
			return IOS_ENOENT;
#endif
		device->suspended = false;
	}

	/* Store assigned fake Wiimote */
	device->wiimote = wiimote;

	if (device->driver->init)
		return device->driver->init(device, device->vid, device->pid);

	return 0;
}

static int usb_device_ops_suspend(void *usrdata)
{
	int ret = 0;
	usb_input_device_t *device = usrdata;

	DEBUG("usb_device_ops_suspend\n");

	if (device->driver->disconnect)
		ret = device->driver->disconnect(device);

	/* Suspend the device */
#if 0
	usb_hid_v5_suspend_resume(device->host_fd, device->dev_id, 0, 0);
#endif
	device->suspended = true;

	return ret;
}

static int usb_device_ops_set_leds(void *usrdata, int leds)
{
	int slot;
	usb_input_device_t *device = usrdata;

	DEBUG("usb_device_ops_set_leds\n");

	if (device->driver->slot_changed) {
		slot = __builtin_ffs(leds);
		return device->driver->slot_changed(device, slot);
	}

	return 0;
}

static int usb_device_ops_set_rumble(void *usrdata, bool rumble_on)
{
	usb_input_device_t *device = usrdata;

	DEBUG("usb_device_ops_set_rumble\n");

	if (device->driver->set_rumble)
		return device->driver->set_rumble(device, rumble_on);

	return 0;
}

static bool usb_device_ops_report_input(void *usrdata)
{
	usb_input_device_t *device = usrdata;

	//DEBUG("usb_device_ops_report_input\n");

	return device->driver->report_input(device);
}

static const input_device_ops_t input_device_usb_ops = {
	.resume		= usb_device_ops_resume,
	.suspend	= usb_device_ops_suspend,
	.set_leds	= usb_device_ops_set_leds,
	.set_rumble	= usb_device_ops_set_rumble,
	.report_input	= usb_device_ops_report_input,
};

static void handle_v5_device_change_reply(int host_fd, areply *reply)
{
	usb_devdesc udd;
	usb_input_device_t *device;
	const usb_device_driver_t *driver;
	u16 vid, pid;
	u32 dev_id;
	int ret;
	bool found;

	DEBUG("Device change, #Attached devices: %d\n", reply->result);

	if (reply->result < 0)
		return;

	/* First look for disconnections */
	for (int i = 0; i < ARRAY_SIZE(usb_devices); i++) {
		device = &usb_devices[i];
		if (!device->valid)
			continue;

		found = false;
		for (int j = 0; j < reply->result; j++) {
			if (device->dev_id == device_change_devices[j].device_id) {
				found = true;
				break;
			}
		}

		/* Oops, it got disconnected */
		if (!found) {
			DEBUG("Device with VID: 0x%04x, PID: 0x%04x, dev_id: 0x%x got disconnected\n",
				device->vid, device->pid, device->dev_id);

			if (device->driver->disconnect)
				ret = device->driver->disconnect(device);
			/* Tell the fake Wiimote manager we got an input device removal */
			input_devices_remove(device->input_device);
			/* Set this device as not valid */
			device->valid = false;
		}
	}

	/* Now look for new connections */
	for (int i = 0; i < reply->result; i++) {
		vid = device_change_devices[i].vid;
		pid = device_change_devices[i].pid;
		dev_id = device_change_devices[i].device_id;
		DEBUG("[%d] VID: 0x%04x, PID: 0x%04x, dev_id: 0x%x\n", i, vid, pid, dev_id);

		/* Check if we already have that device (same dev_id) connected */
		if (is_usb_device_connected(dev_id))
			continue;

		/* Find if we have a driver for that VID/PID */
		driver = get_usb_device_driver_for(vid, pid);
		if (!driver)
			continue;

		/* Get an empty device slot */
		device = get_free_usb_device_slot();
		if (!device)
			break;

		/* Now we can attach it to take ownership! */
		ret = usb_hid_v5_attach(host_fd, dev_id);
		if (ret != IOS_OK)
			continue;

		/* We must resume the USB device before interacting with it */
		ret = usb_hid_v5_suspend_resume(host_fd, dev_id, 1, 0);
		if (ret != IOS_OK) {
			usb_hid_v5_release(host_fd, dev_id);
			continue;
		}

		/* We must read the USB device descriptor before interacting with the device */
		ret = usb_hid_v5_get_descriptors(host_fd, dev_id, &udd);
		if (ret != IOS_OK) {
			usb_hid_v5_release(host_fd, dev_id);
			continue;
		}

		/* Get a fake Wiimote from the manager */
		if (!input_devices_add(device, &input_device_usb_ops, &device->input_device)) {
			usb_hid_v5_release(host_fd, dev_id);
			continue;
		}

		/* We have ownership, populate the device info */
		device->vid = vid;
		device->pid = pid;
		device->host_fd = host_fd;
		device->dev_id = dev_id;
		device->driver = driver;
		/* With HIDv5, we don't need to specify these values explicitly */
		device->max_packet_len = 128;
		device->endpoint_address = 0;
		/* We will get a fake Wiimote assigneed at the init() callback */
		device->wiimote = NULL;
		device->suspended = false;
		device->valid = true;

	}

	ret = os_ioctl_async(host_fd, USBV5_IOCTL_ATTACHFINISH, NULL, 0, NULL, 0,
			     queue_id, MESSAGE_ATTACHFINISH);
	DEBUG("ioctl(ATTACHFINISH): %d\n", ret);
}

static void handle_v4_device_change_reply(int host_fd, areply *reply)
{
	usb_input_device_t *device;
	const usb_device_driver_t *driver;
	u16 vid, pid;
	u32 vid_pid;
	u32 dev_id;
	u16 packet_size = 128;
	u8 endpoint_address = 0;
	bool found;

	DEBUG("Device change, #Attached devices: %d\n", reply->result);

	if (reply->result < 0)
		return;
	
	/* First look for disconnections */
	for (int i = 0; i < ARRAY_SIZE(usb_devices); i++) {
		device = &usb_devices[i];
		if (!device->valid)
			continue;

		found = false;
		for (int i = 0; i < DEV_USB_HID4_DEVICE_CHANGE_SIZE && device_change_v4_devices[i] < sizeof(device_change_v4_devices); i += device_change_v4_devices[i] / 4) {
			dev_id = device_change_v4_devices[i + 1];
			if (device->dev_id == dev_id) {
				found = true;
				break;
			}
		}

		/* Oops, it got disconnected */
		if (!found) {
			if (device->driver->disconnect)
				device->driver->disconnect(device);
			/* Tell the fake Wiimote manager we got an input device removal */
			input_devices_remove(device->input_device);
			/* Set this device as not valid */
			device->valid = false;
		}
	}
	for (int i = 0; i < DEV_USB_HID4_DEVICE_CHANGE_SIZE && device_change_v4_devices[i] < sizeof(device_change_v4_devices); i += device_change_v4_devices[i] / 4) {
		dev_id = device_change_v4_devices[i + 1];
		vid_pid = device_change_v4_devices[i + 4];
		vid = (vid_pid >> 16) & 0xFFFF;
		pid = vid_pid & 0xFFFF;
		DEBUG("[%d] VID: 0x%04x, PID: 0x%04x, dev_id: 0x%x\n", i, vid, pid, dev_id);
		u32 total_len = device_change_v4_devices[i] / 4;
		int j = 2;
		while (j < total_len) {
			u8 len = (device_change_v4_devices[i+j] >> 24) & 0xFF;
			u8 type = (device_change_v4_devices[i+j] >> 16) & 0xFF;
			if (type == 5) {
				u8 addr = (device_change_v4_devices[i+j] >> 8) & 0xFF;
				int in = (addr & USB_ENDPOINT_IN);
				if (in) {
					packet_size = (device_change_v4_devices[i+j+1] >> 16) & 0xFFFF;
					endpoint_address = addr;
					break;
				}
			}
			j += (len / 4) + 1;
		}
		
		/* Find if we have a driver for that VID/PID */
		driver = get_usb_device_driver_for(vid, pid);
		if (!driver)
			continue;

		/* Check if we already have that device (same dev_id) connected */
		if (is_usb_device_connected(dev_id))
			continue;

		/* Get an empty device slot */
		device = get_free_usb_device_slot();
		if (!device)
			break;

		/* Get a fake Wiimote from the manager */
		if (!input_devices_add(device, &input_device_usb_ops, &device->input_device)) {
			continue;
		}

		/* We have ownership, populate the device info */
		device->vid = vid;
		device->pid = pid;
		device->host_fd = host_fd;
		device->max_packet_len = packet_size;
		device->dev_id = dev_id;
		device->driver = driver;
		device->endpoint_address = endpoint_address;
		/* We will get a fake Wiimote assigneed at the init() callback */
		device->wiimote = NULL;
		device->suspended = false;
		device->valid = true;

	}

	os_ioctl_async(host_fd, USBV4_IOCTL_GETDEVICECHANGE, NULL, 0,
					device_change_v4_devices, sizeof(device_change_v4_devices),
					queue_id, MESSAGE_DEVCHANGE);
}

static int usb_hid_worker(void *arg)
{
	u32 ver[8] ATTRIBUTE_ALIGN(32);
	usb_input_device_t *device;
	int ret;

	DEBUG("usb_hid_worker thread started\n");

	for (int i = 0; i < ARRAY_SIZE(usb_devices); i++)
		usb_devices[i].valid = false;

	/* USB_HID supports 16 handles, libogc uses handle 0, so we use handle 15...*/
	ret = os_open("/dev/usb/hid", 15);
	if (ret < 0)
		return ret;
	host_fd = ret;

	ret = os_ioctl(host_fd, USBV4_IOCTL_GETVERSION, NULL, 0, NULL, 0);
	if (ret == 0x40001) {
		using_v4_api = true;
		os_ioctl_async(host_fd, USBV4_IOCTL_GETDEVICECHANGE, NULL, 0,
					device_change_v4_devices, sizeof(device_change_v4_devices),
					queue_id, MESSAGE_DEVCHANGE);
	} else {
		ret = os_ioctl(host_fd, USBV5_IOCTL_GETVERSION, NULL, 0, ver, sizeof(ver));
		if (ret < 0)
			return ret;

		if (ver[0] != 0x50001)
			return IOS_EINVAL;

		using_v4_api = false;
		ret = os_ioctl_async(host_fd, USBV5_IOCTL_GETDEVICECHANGE, NULL, 0, device_change_devices,
			     sizeof(device_change_devices), queue_id, MESSAGE_DEVCHANGE);
	}


	while (1) {
		areply *message;

		/* Wait for a message from USB devices */
		ret = os_message_queue_receive(queue_id, (void *)&message, IOS_MESSAGE_BLOCK);
		if (ret != IOS_OK)
			continue;

		if (message == MESSAGE_DEVCHANGE) {
			if (using_v4_api) {
				handle_v4_device_change_reply(host_fd, message);
			} else {
				handle_v5_device_change_reply(host_fd, message);
			}
		} else if (message == MESSAGE_ATTACHFINISH) {
			ret = os_ioctl_async(host_fd, USBV5_IOCTL_GETDEVICECHANGE, NULL, 0,
					     device_change_devices, sizeof(device_change_devices),
					     queue_id, MESSAGE_DEVCHANGE);
		} else {
			/* Find if this is the reply to a USB async req issued by a device driver */
			for (int i = 0; i < ARRAY_SIZE(usb_devices); i++) {
				device = &usb_devices[i];
				if (device->valid && (message == &device->usb_async_resp_msg)) {
					if (device->driver->usb_async_resp) {
						if (using_v4_api) {
							memcpy(device->usb_async_resp, v4_data, sizeof(v4_data));
						}
						device->driver->usb_async_resp(device);
					}
				}
			}
		}
	}

	return 0;
}

int usb_hid_init(void)
{
	int ret;

	ret = os_message_queue_create(queue_data, ARRAY_SIZE(queue_data));
	if (ret < 0)
		return ret;
	queue_id = ret;

	/* Worker USB HID thread that receives and dispatches async events */
	ret = os_thread_create(usb_hid_worker, NULL,
			       &worker_thread_stack[sizeof(worker_thread_stack)],
			       sizeof(worker_thread_stack), 0, 0);
	if (ret < 0)
		return ret;
	os_thread_continue(ret);

	return 0;
}
