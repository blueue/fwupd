/*
 * Copyright 2022 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-ch341a-cfi-device.h"
#include "fu-ch341a-device.h"
#include "fu-ch341a-struct.h"

struct _FuCh341aDevice {
	FuUsbDevice parent_instance;
	guint8 speed;
};

G_DEFINE_TYPE(FuCh341aDevice, fu_ch341a_device, FU_TYPE_USB_DEVICE)

#define CH341A_USB_TIMEOUT 1000
#define CH341A_EP_OUT	   0x02 /* host to device (write) */
#define CH341A_EP_IN	   0x82 /* device to host (read) */
#define CH341A_EP_SIZE	   0x20

#define FU_CH341A_STM_SPI_MODUS_STANDARD 0x00
#define FU_CH341A_STM_SPI_MODUS_DOUBLE	 0x04

#define FU_CH341A_STM_SPI_ENDIAN_BIG	0x0
#define FU_CH341A_STM_SPI_ENDIAN_LITTLE 0x80

static const gchar *
fu_ch341a_device_speed_to_string(guint8 speed)
{
	if (speed == FU_CH341A_STM_I2C_SPEED_LOW)
		return "20kHz";
	if (speed == FU_CH341A_STM_I2C_SPEED_STANDARD)
		return "100kHz";
	if (speed == FU_CH341A_STM_I2C_SPEED_FAST)
		return "400kHz";
	if (speed == FU_CH341A_STM_I2C_SPEED_HIGH)
		return "750kHz";
	if (speed == (FU_CH341A_STM_I2C_SPEED_LOW | FU_CH341A_STM_SPI_MODUS_DOUBLE))
		return "2*20kHz";
	if (speed == (FU_CH341A_STM_I2C_SPEED_STANDARD | FU_CH341A_STM_SPI_MODUS_DOUBLE))
		return "2*100kHz";
	if (speed == (FU_CH341A_STM_I2C_SPEED_FAST | FU_CH341A_STM_SPI_MODUS_DOUBLE))
		return "2*400kHz";
	if (speed == (FU_CH341A_STM_I2C_SPEED_HIGH | FU_CH341A_STM_SPI_MODUS_DOUBLE))
		return "2*750kHz";
	return NULL;
}

static void
fu_ch341a_device_to_string(FuDevice *device, guint idt, GString *str)
{
	FuCh341aDevice *self = FU_CH341A_DEVICE(device);
	fwupd_codec_string_append(str, idt, "Speed", fu_ch341a_device_speed_to_string(self->speed));
}

static gboolean
fu_ch341a_device_write(FuCh341aDevice *self, guint8 *buf, gsize bufsz, GError **error)
{
	gsize actual_length = 0;

	/* debug */
	fu_dump_raw(G_LOG_DOMAIN, "write", buf, bufsz);
	if (!fu_usb_device_bulk_transfer(FU_USB_DEVICE(self),
					 CH341A_EP_OUT,
					 buf,
					 bufsz,
					 &actual_length,
					 CH341A_USB_TIMEOUT,
					 NULL,
					 error)) {
		g_prefix_error(error, "failed to write 0x%x bytes: ", (guint)bufsz);
		return FALSE;
	}
	if (bufsz != actual_length) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "only wrote 0x%x of 0x%x",
			    (guint)actual_length,
			    (guint)bufsz);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_ch341a_device_read(FuCh341aDevice *self, guint8 *buf, gsize bufsz, GError **error)
{
	gsize actual_length = 0;

	if (!fu_usb_device_bulk_transfer(FU_USB_DEVICE(self),
					 CH341A_EP_IN,
					 buf,
					 bufsz,
					 &actual_length,
					 CH341A_USB_TIMEOUT,
					 NULL,
					 error)) {
		g_prefix_error(error, "failed to read 0x%x bytes: ", (guint)bufsz);
		return FALSE;
	}
	if (bufsz != actual_length) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "only read 0x%x of 0x%x",
			    (guint)actual_length,
			    (guint)bufsz);
		return FALSE;
	}

	/* debug */
	fu_dump_raw(G_LOG_DOMAIN, "read", buf, bufsz);

	/* success */
	return TRUE;
}

/**
 * fu_ch341a_device_reverse_uint8:
 * @value: integer
 *
 * Calculates the reverse bit order for a single byte.
 *
 * Returns: the @value, reversed
 **/
static guint8
fu_ch341a_device_reverse_uint8(guint8 value)
{
	guint8 tmp = 0;
	if (value & 0x01)
		tmp = 0x80;
	if (value & 0x02)
		tmp |= 0x40;
	if (value & 0x04)
		tmp |= 0x20;
	if (value & 0x08)
		tmp |= 0x10;
	if (value & 0x10)
		tmp |= 0x08;
	if (value & 0x20)
		tmp |= 0x04;
	if (value & 0x40)
		tmp |= 0x02;
	if (value & 0x80)
		tmp |= 0x01;
	return tmp;
}

gboolean
fu_ch341a_device_spi_transfer(FuCh341aDevice *self, guint8 *buf, gsize bufsz, GError **error)
{
	gsize buf2sz = bufsz + 1;
	g_autofree guint8 *buf2 = g_malloc0(buf2sz);

	/* requires LSB first */
	buf2[0] = FU_CH341A_CMD_SPI_STREAM;
	for (gsize i = 0; i < bufsz; i++)
		buf2[i + 1] = fu_ch341a_device_reverse_uint8(buf[i]);

	/* debug */
	fu_dump_raw(G_LOG_DOMAIN, "SPIwrite", buf, bufsz);
	if (!fu_ch341a_device_write(self, buf2, buf2sz, error))
		return FALSE;

	if (!fu_ch341a_device_read(self, buf, bufsz, error))
		return FALSE;

	/* requires LSB first */
	for (gsize i = 0; i < bufsz; i++)
		buf[i] = fu_ch341a_device_reverse_uint8(buf[i]);

	/* debug */
	fu_dump_raw(G_LOG_DOMAIN, "SPIread", buf, bufsz);

	/* success */
	return TRUE;
}

static gboolean
fu_ch341a_device_configure_stream(FuCh341aDevice *self, GError **error)
{
	guint8 buf[] = {FU_CH341A_CMD_I2C_STREAM,
			FU_CH341A_CMD_I2C_STM_SET | self->speed,
			FU_CH341A_CMD_I2C_STM_END};
	if (!fu_ch341a_device_write(self, buf, sizeof(buf), error)) {
		g_prefix_error(error, "failed to configure stream: ");
		return FALSE;
	}

	/* success */
	return TRUE;
}

gboolean
fu_ch341a_device_chip_select(FuCh341aDevice *self, gboolean val, GError **error)
{
	guint8 buf[] = {
	    FU_CH341A_CMD_UIO_STREAM,
	    FU_CH341A_CMD_UIO_STM_OUT | (val ? 0x36 : 0x37), /* CS* high, SCK=0, DOUBT*=1 */
	    FU_CH341A_CMD_UIO_STM_DIR | (val ? 0x3F : 0x00), /* pin direction */
	    FU_CH341A_CMD_UIO_STM_END,
	};
	return fu_ch341a_device_write(self, buf, sizeof(buf), error);
}

static gboolean
fu_ch341a_device_probe(FuDevice *device, GError **error)
{
	g_autoptr(FuCh341aCfiDevice) cfi_device = NULL;
	cfi_device = g_object_new(FU_TYPE_CH341A_CFI_DEVICE,
				  "context",
				  fu_device_get_context(device),
				  "proxy",
				  device,
				  "logical-id",
				  "SPI",
				  NULL);
	fu_device_add_child(device, FU_DEVICE(cfi_device));
	return TRUE;
}

static gboolean
fu_ch341a_device_setup(FuDevice *device, GError **error)
{
	FuCh341aDevice *self = FU_CH341A_DEVICE(device);

	/* FuUsbDevice->setup */
	if (!FU_DEVICE_CLASS(fu_ch341a_device_parent_class)->setup(device, error))
		return FALSE;

	/* set speed */
	if (!fu_ch341a_device_configure_stream(self, error))
		return FALSE;

	/* success */
	return TRUE;
}

static void
fu_ch341a_device_init(FuCh341aDevice *self)
{
	self->speed = FU_CH341A_STM_I2C_SPEED_STANDARD;
	fu_usb_device_add_interface(FU_USB_DEVICE(self), 0x0);
	fu_device_set_name(FU_DEVICE(self), "CH341A");
	fu_device_set_vendor(FU_DEVICE(self), "WinChipHead");
}

static void
fu_ch341a_device_class_init(FuCh341aDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	device_class->probe = fu_ch341a_device_probe;
	device_class->setup = fu_ch341a_device_setup;
	device_class->to_string = fu_ch341a_device_to_string;
}
