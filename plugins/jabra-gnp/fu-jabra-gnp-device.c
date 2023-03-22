/*
 * Copyright (C) 2023 GN Audio
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>

#include <string.h>

#include "fu-jabra-gnp-device.h"

#define FU_JABRA_GNP_BUF_SIZE			63
#define FU_JABRA_GNP_MAX_RETRIES		3
#define FU_JABRA_GNP_RETRY_DELAY		100
#define FU_JABRA_GNP_STANDARD_SEND_TIMEOUT	3000
#define FU_JABRA_GNP_STANDARD_RECEIVE_TIMEOUT	1000
#define FU_JABRA_GNP_LONG_RECEIVE_TIMEOUT	30000
#define FU_JABRA_GNP_EXTRA_LONG_RECEIVE_TIMEOUT 60000

struct _FuJabraGnpDevice {
	FuUsbDevice parent_instance;
	guint8 iface_hid;
	guint8 sequence_number;
	gchar *version;
};

typedef struct __attribute__((packed)) {
	const guint8 *txbuf;
	const guint timeout;
} FuJabraGnpSendData;

typedef struct __attribute__((packed)) {
	guint8 *rxbuf;
	const guint timeout;
} FuJabraGnpReceiveData;

G_DEFINE_TYPE(FuJabraGnpDevice, fu_jabra_gnp_device, FU_TYPE_HID_DEVICE)

static guint8
_g_usb_device_get_interface_for_class(GUsbDevice *dev, guint8 intf_class, GError **error)
{
	g_autoptr(GPtrArray) intfs = NULL;
	intfs = g_usb_device_get_interfaces(dev, error);
	if (intfs == NULL)
		return 0xff;
	for (guint i = 0; i < intfs->len; i++) {
		GUsbInterface *intf = g_ptr_array_index(intfs, i);
		if (g_usb_interface_get_class(intf) == intf_class)
			return g_usb_interface_get_number(intf);
	}
	return 0xff;
}

gboolean
fu_jabra_gnp_device_send(FuJabraGnpDevice *self, gpointer *user_data, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	FuJabraGnpSendData *send_data = (FuJabraGnpSendData *)user_data;
	if (!g_usb_device_control_transfer(fu_usb_device_get_dev(FU_USB_DEVICE(self)),
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_CLASS,
					   G_USB_DEVICE_RECIPIENT_INTERFACE,
					   0x09,
					   0x0200 | 0x05,
					   self->iface_hid,
					   send_data->txbuf,
					   FU_JABRA_GNP_BUF_SIZE,
					   NULL,
					   send_data->timeout,
					   NULL, /* cancellable */
					   &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_WRITE,
			    "failed to write to device: %s",
			    error_local->message);
		g_debug("sending error: %s, ignoring", error_local->message);
	} else {
		return TRUE;
	}
	return FALSE;
}

gboolean
fu_jabra_gnp_device_receive_with_sequence(FuJabraGnpDevice *self,
					  gpointer *user_data,
					  GError **error)
{
	g_autoptr(GError) error_local = NULL;
	FuJabraGnpReceiveData *receive_data;
	if (!fu_jabra_gnp_device_receive(self, user_data, error))
		return FALSE;
	receive_data = (FuJabraGnpReceiveData *)user_data;

	if (self->sequence_number != receive_data->rxbuf[3]) {
		g_debug("sequence_number error");
		return FALSE;
	} else {
		self->sequence_number += 1;
		return TRUE;
	}

	return FALSE;
}

gboolean
fu_jabra_gnp_device_receive(FuJabraGnpDevice *self, gpointer *user_data, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	FuJabraGnpReceiveData *receive_data = (FuJabraGnpReceiveData *)user_data;

	if (!g_usb_device_interrupt_transfer(fu_usb_device_get_dev(FU_USB_DEVICE(self)),
					     0x81,
					     receive_data->rxbuf,
					     FU_JABRA_GNP_BUF_SIZE,
					     NULL,
					     receive_data->timeout,
					     NULL, /* cancellable */
					     &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_READ,
			    "failed to read from device: %s",
			    error_local->message);
		g_debug("receiving error: %s, ignoring", error_local->message);
	} else {
		return TRUE;
	}
	return FALSE;
}

static gboolean
fu_jabra_gnp_device_read_version(FuJabraGnpDevice *self, GError **error)
{
	guint version_length = 0;
	const guint8 txbuf[FU_JABRA_GNP_BUF_SIZE] =
	    {0x05, 0x08, 0x00, self->sequence_number, 0x46, 0x02, 0x03};
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	FuJabraGnpSendData send_data = {.txbuf = txbuf,
					.timeout = FU_JABRA_GNP_STANDARD_SEND_TIMEOUT};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_STANDARD_RECEIVE_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_send,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&send_data,
				  error))
		return FALSE;
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_receive_with_sequence,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&receive_data,
				  error))
		return FALSE;
	do {
		version_length++;
	} while (receive_data.rxbuf[version_length] != 0x00);
	self->version = g_strdup_printf("%.*s", version_length - 8, receive_data.rxbuf + 8);
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_write_partition(FuJabraGnpDevice *self, guint8 part, GError **error)
{
	const guint8 txbuf[FU_JABRA_GNP_BUF_SIZE] =
	    {0x05, 0x08, 0x00, self->sequence_number, 0x87, 0x0F, 0x2D, part};
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	FuJabraGnpSendData send_data = {.txbuf = txbuf,
					.timeout = FU_JABRA_GNP_STANDARD_SEND_TIMEOUT};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_STANDARD_RECEIVE_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_send,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&send_data,
				  error))
		return FALSE;

	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_receive_with_sequence,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&receive_data,
				  error))
		return FALSE;
	if (receive_data.rxbuf[5] != 0xFF) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "internal error: expected 0xFF, got 0x%02x 0x%02x",
			    receive_data.rxbuf[5],
			    receive_data.rxbuf[6]);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_start(FuJabraGnpDevice *self, GError **error)
{
	const guint8 txbuf[FU_JABRA_GNP_BUF_SIZE] =
	    {0x05, 0x08, 0x00, self->sequence_number, 0x86, 0x0F, 0x17};
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	FuJabraGnpSendData send_data = {.txbuf = txbuf,
					.timeout = FU_JABRA_GNP_STANDARD_SEND_TIMEOUT};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_STANDARD_RECEIVE_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_send,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&send_data,
				  error))
		return FALSE;
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_receive_with_sequence,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&receive_data,
				  error))
		return FALSE;
	if (receive_data.rxbuf[5] != 0xFF) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "internal error: expected 0xFF, got 0x%02x 0x%02x",
			    receive_data.rxbuf[5],
			    receive_data.rxbuf[6]);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_flash_erase_done(FuJabraGnpDevice *self, GError **error)
{
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	const guint8 match_buf[FU_JABRA_GNP_BUF_SIZE] = {0x05, 0x08, 0x00, 0x00, 0x06, 0x0F, 0x18};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_EXTRA_LONG_RECEIVE_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_receive,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&receive_data,
				  error))
		return FALSE;
	if (receive_data.rxbuf != match_buf) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "internal error, buf did not match");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_write_crc(FuJabraGnpDevice *self,
			      guint crc,
			      guint total_chunks,
			      guint preload_count,
			      GError **error)
{
	const guint8 txbuf[FU_JABRA_GNP_BUF_SIZE] = {0x05,
						     0x08,
						     0x00,
						     self->sequence_number,
						     0x8E,
						     0x0F,
						     0x19,
						     (guint8)(crc & 0xff),
						     (guint8)((crc >> 8) & 0xff),
						     (guint8)((crc >> 16) & 0xff),
						     (guint8)((crc >> 24) & 0xff),
						     (guint8)(total_chunks & 0xff),
						     (guint8)((total_chunks >> 8) & 0xff),
						     (guint8)(preload_count & 0xff),
						     (guint8)((preload_count >> 8) & 0xff)};
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	FuJabraGnpSendData send_data = {.txbuf = txbuf,
					.timeout = FU_JABRA_GNP_STANDARD_SEND_TIMEOUT};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_STANDARD_RECEIVE_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_send,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&send_data,
				  error))
		return FALSE;
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_receive_with_sequence,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&receive_data,
				  error))
		return FALSE;
	if (receive_data.rxbuf[5] != 0xFF) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "internal error: expected 0xFF, got 0x%02x 0x%02x",
			    receive_data.rxbuf[5],
			    receive_data.rxbuf[6]);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_write_chunk(FuJabraGnpDevice *self,
				guint32 chunk_number,
				guint8 *data,
				guint32 data_size,
				GError **error)
{
	guint8 write_length = 0x00 + data_size;
	guint8 txbuf[FU_JABRA_GNP_BUF_SIZE] = {0x05,
					       0x08,
					       0x00,
					       0x00,
					       write_length,
					       0x0F,
					       0x19,
					       (guint8)(chunk_number & 0xff),
					       (guint8)((chunk_number >> 8) & 0xff),
					       (guint8)(data_size & 0xff),
					       (guint8)((data_size >> 8) & 0xff)};
	memcpy(txbuf + 11, data, data_size);
	FuJabraGnpSendData send_data = {.txbuf = txbuf,
					.timeout = FU_JABRA_GNP_STANDARD_SEND_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_send,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&send_data,
				  error))
		return FALSE;
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_write_chunks(FuJabraGnpDevice *self, GPtrArray *chunks, GError **error)
{
	guint32 preload_count = 100;
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	const guint8 match_buf[FU_JABRA_GNP_BUF_SIZE] = {0x05, 0x08, 0x00, 0x00, 0x06, 0x0F, 0x1B};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_LONG_RECEIVE_TIMEOUT};

	for (guint chunk_number = 0; chunk_number < chunks->len; chunk_number++) {
		FuChunk *chk = g_ptr_array_index(chunks, chunk_number);
		if (!fu_jabra_gnp_device_write_chunk(self,
						     chunk_number,
						     fu_chunk_get_data(chk),
						     fu_chunk_get_data_sz(chk),
						     error))
			return FALSE;
		if ((chunk_number % preload_count) == 0) {
			if (!fu_device_retry_full(FU_DEVICE(self),
						  fu_jabra_gnp_device_receive,
						  FU_JABRA_GNP_MAX_RETRIES,
						  FU_JABRA_GNP_RETRY_DELAY,
						  (gpointer)&receive_data,
						  error))
				return FALSE;
			if (rxbuf != match_buf) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INTERNAL,
					    "internal error, buf did not match");
				return FALSE;
			}
			if ((((rxbuf[8] << 8u) | rxbuf[7]) == chunk_number) == FALSE) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INTERNAL,
					    "internal error, buf did not match");
				return FALSE;
			}
		}
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_read_verify_status(FuJabraGnpDevice *self, GError **error)
{
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	const guint8 match_buf[FU_JABRA_GNP_BUF_SIZE] = {0x05, 0x08, 0x00, 0x00, 0x06, 0x0F, 0x1C};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_LONG_RECEIVE_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_receive,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&receive_data,
				  error))
		return FALSE;
	if (receive_data.rxbuf != match_buf) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "internal error, buf did not match");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_write_version(FuJabraGnpDevice *self,
				  guint8 v1,
				  guint8 v2,
				  guint8 v3,
				  GError **error)
{
	const guint8 txbuf[FU_JABRA_GNP_BUF_SIZE] =
	    {0x05, 0x08, 0x00, self->sequence_number, 0x89, 0x0F, 0x1E, v1, v2, v3};
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	FuJabraGnpSendData send_data = {.txbuf = txbuf,
					.timeout = FU_JABRA_GNP_STANDARD_SEND_TIMEOUT};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_STANDARD_RECEIVE_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_send,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&send_data,
				  error))
		return FALSE;
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_receive_with_sequence,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&receive_data,
				  error))
		return FALSE;
	if (receive_data.rxbuf[5] != 0xFF) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "internal error: expected 0xFF, got 0x%02x 0x%02x",
			    receive_data.rxbuf[5],
			    receive_data.rxbuf[6]);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_write_dfu_from_squif(FuJabraGnpDevice *self, GError **error)
{
	const guint8 txbuf[FU_JABRA_GNP_BUF_SIZE] =
	    {0x05, 0x08, 0x00, self->sequence_number, 0x86, 0x0F, 0x1D};
	guint8 rxbuf[FU_JABRA_GNP_BUF_SIZE] = {0x00};
	FuJabraGnpSendData send_data = {.txbuf = txbuf,
					.timeout = FU_JABRA_GNP_STANDARD_SEND_TIMEOUT};
	FuJabraGnpReceiveData receive_data = {.rxbuf = rxbuf,
					      .timeout = FU_JABRA_GNP_STANDARD_RECEIVE_TIMEOUT};
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_send,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&send_data,
				  error))
		return FALSE;
	if (!fu_device_retry_full(FU_DEVICE(self),
				  fu_jabra_gnp_device_receive_with_sequence,
				  FU_JABRA_GNP_MAX_RETRIES,
				  FU_JABRA_GNP_RETRY_DELAY,
				  (gpointer)&receive_data,
				  error))
		return FALSE;
	if (receive_data.rxbuf[5] != 0xFF) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "internal error: expected 0xFF, got 0x%02x 0x%02x",
			    receive_data.rxbuf[5],
			    receive_data.rxbuf[6]);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_probe(FuDevice *device, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	FuJabraGnpDevice *self = FU_JABRA_GNP_DEVICE(device);
	self->iface_hid =
	    _g_usb_device_get_interface_for_class(fu_usb_device_get_dev(FU_USB_DEVICE(self)),
						  G_USB_DEVICE_CLASS_HID,
						  &error_local);
	if (self->iface_hid == 0xff) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "cannot find HID interface: %s",
			    error_local->message);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_open(FuDevice *device, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	FuJabraGnpDevice *self = FU_JABRA_GNP_DEVICE(device);
	if (!FU_DEVICE_CLASS(fu_jabra_gnp_device_parent_class)->open(device, error))
		return FALSE;
	g_debug("claiming interface 0x%02x", self->iface_hid);
	if (!g_usb_device_claim_interface(fu_usb_device_get_dev(FU_USB_DEVICE(self)),
					  (gint)(self->iface_hid),
					  G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					  &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "cannot claim interface 0x%02x: %s",
			    self->iface_hid,
			    error_local->message);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_setup(FuDevice *device, GError **error)
{
	FuJabraGnpDevice *self = FU_JABRA_GNP_DEVICE(device);
	if (!fu_jabra_gnp_device_read_version(self, error))
		return FALSE;
	fu_device_set_version_format(FU_DEVICE(device), FWUPD_VERSION_FORMAT_PLAIN);
	fu_device_set_version(FU_DEVICE(device), self->version);
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_write_firmware(FuDevice *device,
				   FuFirmware *firmware,
				   FuProgress *progress,
				   FwupdInstallFlags flags,
				   GError **error)
{
	guint8 part = 0x00;
	guint crc = 0;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) chunks = NULL;
	FuJabraGnpDevice *self = FU_JABRA_GNP_DEVICE(device);

	while (part != 0x00) {
		guint chunk_size = 52;

		if (!fu_jabra_gnp_device_write_partition(self, part, error))
			return FALSE;
		if (!fu_jabra_gnp_device_start(self, error))
			return FALSE;
		if (!fu_jabra_gnp_device_flash_erase_done(self, error))
			return FALSE;

		crc = ((crc & 0xffff) << 16) | ((crc & 0xffff0000) >> 16);
		if (!fu_jabra_gnp_device_write_crc)
			return FALSE;

		chunks = fu_chunk_array_new_from_bytes(firmware, 0x00, 0x00, chunk_size);
		if (!fu_jabra_gnp_device_write_chunks(self, chunks, error))
			return FALSE;
		if (!fu_jabra_gnp_device_read_verify_status(self, error))
			return FALSE;
		if (!fu_jabra_gnp_device_write_version(self, 0x01, 0x02, 0x03, error))
			return FALSE;
	}
	// if (!fu_jabra_gnp_device_write_dfu_from_squif(self, error))
	// return FALSE;
	return TRUE;
}

static gboolean
fu_jabra_gnp_device_close(FuDevice *device, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	FuJabraGnpDevice *self = FU_JABRA_GNP_DEVICE(device);
	if (!g_usb_device_release_interface(fu_usb_device_get_dev(FU_USB_DEVICE(self)),
					    (gint)(self->iface_hid),
					    G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					    &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "failed to release interface 0x%02x: %s",
			    self->iface_hid,
			    error_local->message);
		return FALSE;
	}
	return TRUE;
}

static void
fu_jabra_gnp_device_init(FuJabraGnpDevice *self)
{
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_ADD_COUNTERPART_GUIDS);
	fu_device_add_protocol(FU_DEVICE(self), "org.jabra.gnp");
}

static void
fu_jabra_gnp_device_class_init(FuJabraGnpDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS(klass);
	klass_device->probe = fu_jabra_gnp_device_probe;
	klass_device->open = fu_jabra_gnp_device_open;
	klass_device->setup = fu_jabra_gnp_device_setup;
	klass_device->write_firmware = fu_jabra_gnp_device_write_firmware;
	klass_device->close = fu_jabra_gnp_device_close;
}
