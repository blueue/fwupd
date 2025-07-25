/*
 * Copyright 2017 VIA Corporation
 * Copyright 2019 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#include "fu-vli-common.h"

#define VLI_USBHUB_PD_FLASHMAP_ADDR_LEGACY 0x4000
#define VLI_USBHUB_PD_FLASHMAP_ADDR	   0x1003

FuVliDeviceKind
fu_vli_pd_common_guess_device_kind(guint32 fwver);
