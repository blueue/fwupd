/*
 * Copyright 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define BCM_VENDOR_BROADCOM 0x14E4

#define BCM_FW_BRANCH_UNKNOWN	   NULL
#define BCM_FW_BRANCH_OSS_FIRMWARE "oss-firmware"

#define BCM_FIRMWARE_SIZE     0x40000 /* x2 for Dell */
#define BCM_PHYS_ADDR_DEFAULT 0x08003800

#define BCM_NVRAM_MAGIC 0x669955AA

/* offsets into NVMRAM */
#define BCM_NVRAM_HEADER_BASE	 0x00
#define BCM_NVRAM_DIRECTORY_BASE 0x14
#define BCM_NVRAM_INFO_BASE	 0x74
#define BCM_NVRAM_VPD_BASE	 0x100
#define BCM_NVRAM_INFO2_BASE	 0x200
#define BCM_NVRAM_STAGE1_BASE	 0x28c

#define BCM_NVRAM_VPD_SZ 0x100

#define BCM_NVRAM_INFO2_SZ 0x8c

#define BCM_NVRAM_STAGE1_VERADDR 0x08
#define BCM_NVRAM_STAGE1_VERSION 0x0C

typedef struct {
	gchar *branch;
	gchar *version;
	FwupdVersionFormat verfmt;
} Bcm57xxVeritem;

gboolean
fu_bcm57xx_verify_crc(GInputStream *stream, GError **error);
gboolean
fu_bcm57xx_verify_magic(GInputStream *stream, gsize offset, GError **error);

/* parses stage1 version */
void
fu_bcm57xx_veritem_free(Bcm57xxVeritem *veritem);
Bcm57xxVeritem *
fu_bcm57xx_veritem_new(const guint8 *buf, gsize bufsz);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Bcm57xxVeritem, fu_bcm57xx_veritem_free)
