/*
 * Copyright 2017 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuCommon"

#include "config.h"

#ifdef HAVE_CPUID_H
#include <cpuid.h>
#endif

#include "fu-common-private.h"
#include "fu-firmware.h"
#include "fu-path.h"
#include "fu-string.h"

/**
 * fu_error_map_entry_to_gerror:
 * @value: the value to look up
 * @entries: the #FuErrorMapEntry map
 * @n_entries: number of @entries
 * @error: (nullable): optional return location for an error
 *
 * Sets the #GError from the integer value and the error map.
 *
 * Any entries with a error_code of `FWUPD_ERROR_LAST` will return success.
 *
 * Returns: boolean success
 *
 * Since: 2.0.13
 **/
gboolean
fu_error_map_entry_to_gerror(guint value,
			     const FuErrorMapEntry entries[],
			     guint n_entries,
			     GError **error)
{
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	for (guint i = 0; i < n_entries; i++) {
		const FuErrorMapEntry entry = entries[i];
		if (entry.value != value)
			continue;
		if (entry.code == FWUPD_ERROR_LAST)
			return TRUE;
		g_set_error(error,
			    FWUPD_ERROR,
			    entry.code,
			    "%s [0x%x]",
			    entry.message != NULL ? entry.message
						  : fwupd_error_to_string(entry.value),
			    entry.value);
		return FALSE;
	}
	g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "generic failure [0x%x]", value);
	return FALSE;
}

/**
 * fu_cpuid:
 * @leaf: the CPUID level, now called the 'leaf' by Intel
 * @eax: (out) (nullable): EAX register
 * @ebx: (out) (nullable): EBX register
 * @ecx: (out) (nullable): ECX register
 * @edx: (out) (nullable): EDX register
 * @error: (nullable): optional return location for an error
 *
 * Calls CPUID and returns the registers for the given leaf.
 *
 * Returns: %TRUE if the registers are set.
 *
 * Since: 1.8.2
 **/
gboolean
fu_cpuid(guint32 leaf, guint32 *eax, guint32 *ebx, guint32 *ecx, guint32 *edx, GError **error)
{
#ifdef HAVE_CPUID_H
	guint eax_tmp = 0;
	guint ebx_tmp = 0;
	guint ecx_tmp = 0;
	guint edx_tmp = 0;

	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* get vendor */
	__get_cpuid_count(leaf, 0x0, &eax_tmp, &ebx_tmp, &ecx_tmp, &edx_tmp);
	if (eax != NULL)
		*eax = eax_tmp;
	if (ebx != NULL)
		*ebx = ebx_tmp;
	if (ecx != NULL)
		*ecx = ecx_tmp;
	if (edx != NULL)
		*edx = edx_tmp;
	return TRUE;
#else
	g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "no <cpuid.h> support");
	return FALSE;
#endif
}

/**
 * fu_cpu_get_attrs:
 * @error: (nullable): optional return location for an error
 *
 * Gets attributes for the first CPU listed in `/proc/cpuinfo`.
 *
 * Returns: (element-type utf8 utf8) (transfer full): CPU attributes
 *
 * Since: 2.0.7
 **/
GHashTable *
fu_cpu_get_attrs(GError **error)
{
	gsize bufsz = 0;
	g_autofree gchar *buf = NULL;
	g_autofree gchar *procpath = fu_path_from_kind(FU_PATH_KIND_PROCFS);
	g_autofree gchar *fn = g_build_filename(procpath, "cpuinfo", NULL);
	g_autoptr(GHashTable) hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	if (!g_file_get_contents(fn, &buf, &bufsz, error))
		return NULL;
	if (bufsz > 0) {
		g_auto(GStrv) lines = fu_strsplit(buf, bufsz, "\n", -1);
		for (guint i = 0; lines[i] != NULL; i++) {
			g_auto(GStrv) tokens = NULL;
			if (lines[i][0] == '\0')
				break;
			tokens = g_strsplit(lines[i], ": ", 2);
			for (guint j = 0; tokens[j] != NULL; j++) {
				g_hash_table_insert(hash,
						    fu_strstrip(tokens[0]),
						    g_strdup(tokens[1]));
			}
		}
	}

	/* success */
	return g_steal_pointer(&hash);
}

/**
 * fu_cpu_get_vendor:
 *
 * Uses CPUID to discover the CPU vendor.
 *
 * Returns: a CPU vendor, e.g. %FU_CPU_VENDOR_AMD if the vendor was AMD.
 *
 * Since: 1.8.2
 **/
FuCpuVendor
fu_cpu_get_vendor(void)
{
#ifdef HAVE_CPUID_H
	guint ebx = 0;
	guint ecx = 0;
	guint edx = 0;

	if (fu_cpuid(0x0, NULL, &ebx, &ecx, &edx, NULL)) {
		if (ebx == signature_INTEL_ebx && edx == signature_INTEL_edx &&
		    ecx == signature_INTEL_ecx) {
			return FU_CPU_VENDOR_INTEL;
		}
		if (ebx == signature_AMD_ebx && edx == signature_AMD_edx &&
		    ecx == signature_AMD_ecx) {
			return FU_CPU_VENDOR_AMD;
		}
	}
#endif

	/* failed */
	return FU_CPU_VENDOR_UNKNOWN;
}

/**
 * fu_common_get_memory_size:
 *
 * Returns the size of physical memory.
 *
 * Returns: bytes
 *
 * Since: 1.5.6
 **/
guint64
fu_common_get_memory_size(void)
{
	return fu_common_get_memory_size_impl();
}

/**
 * fu_common_get_kernel_cmdline:
 * @error: (nullable): optional return location for an error
 *
 * Returns the current kernel command line options.
 *
 * Returns: options as a string, or %NULL on error
 *
 * Since: 1.5.6
 **/
gchar *
fu_common_get_kernel_cmdline(GError **error)
{
	return fu_common_get_kernel_cmdline_impl(error);
}

/**
 * fu_common_get_olson_timezone_id:
 * @error: (nullable): optional return location for an error
 *
 * Gets the system Olson timezone ID, as used in the CLDR and ICU specifications.
 *
 * Returns: timezone string, e.g. `Europe/London` or %NULL on error
 *
 * Since: 1.9.7
 **/
gchar *
fu_common_get_olson_timezone_id(GError **error)
{
	return fu_common_get_olson_timezone_id_impl(error);
}

/**
 * fu_common_align_up:
 * @value: value to align
 * @alignment: align to this power of 2, where 0x1F is the maximum value of 2GB
 *
 * Align a value to a power of 2 boundary, where @alignment is the bit position
 * to align to. If @alignment is zero then @value is always returned unchanged.
 *
 * Returns: aligned value, which will be the same as @value if already aligned,
 * 		or %G_MAXSIZE if the value would overflow
 *
 * Since: 1.6.0
 **/
gsize
fu_common_align_up(gsize value, guint8 alignment)
{
	gsize value_new;
	gsize mask = (gsize)1 << alignment;

	g_return_val_if_fail(alignment <= FU_FIRMWARE_ALIGNMENT_2G, G_MAXSIZE);

	/* no alignment required */
	if ((value & (mask - 1)) == 0)
		return value;

	/* increment up to the next alignment value */
	value_new = value + mask;
	value_new &= ~(mask - 1);

	/* overflow */
	if (value_new < value)
		return G_MAXSIZE;

	/* success */
	return value_new;
}

/**
 * fu_power_state_is_ac:
 * @power_state: a power state, e.g. %FU_POWER_STATE_AC_FULLY_CHARGED
 *
 * Determines if the power state can be considered "on AC power".
 *
 * Returns: %TRUE for success
 *
 * Since: 1.8.11
 **/
gboolean
fu_power_state_is_ac(FuPowerState power_state)
{
	return power_state != FU_POWER_STATE_BATTERY;
}

/**
 * fu_error_convert:
 * @entries: the #FuErrorConvertEntry map
 * @n_entries: number of @entries
 * @perror: (nullable): A #GError, perhaps with domain #GIOError
 *
 * Convert the error to a #FwupdError, if required.
 *
 * Since: 2.0.14
 **/
gboolean
fu_error_convert(const FuErrorConvertEntry entries[], guint n_entries, GError **perror)
{
	GError *error = (perror != NULL) ? *perror : NULL;

	/* sanity check */
	if (error == NULL)
		return TRUE;

	/* convert GIOError and GFileError */
	fwupd_error_convert(perror);
	if (error->domain == FWUPD_ERROR)
		return FALSE;
	for (guint i = 0; i < n_entries; i++) {
		if (g_error_matches(error, entries[i].domain, entries[i].code)) {
			error->domain = FWUPD_ERROR;
			error->code = entries[i].error;
			return FALSE;
		}
	}

#ifndef SUPPORTED_BUILD
	/* fallback */
	g_critical("GError %s:%i was not converted to FwupdError",
		   g_quark_to_string(error->domain),
		   error->code);
#endif
	error->domain = FWUPD_ERROR;
	error->code = FWUPD_ERROR_INTERNAL;
	return FALSE;
}

/**
 * fu_xmlb_builder_insert_kv:
 * @bn: #XbBuilderNode
 * @key: string key
 * @value: string value
 *
 * Convenience function to add an XML node with a string value. If @value is %NULL
 * then no member is added.
 *
 * Since: 1.6.0
 **/
void
fu_xmlb_builder_insert_kv(XbBuilderNode *bn, const gchar *key, const gchar *value)
{
	if (value == NULL)
		return;
	xb_builder_node_insert_text(bn, key, value, NULL);
}

/**
 * fu_xmlb_builder_insert_kx:
 * @bn: #XbBuilderNode
 * @key: string key
 * @value: integer value
 *
 * Convenience function to add an XML node with an integer value. If @value is 0
 * then no member is added.
 *
 * Since: 1.6.0
 **/
void
fu_xmlb_builder_insert_kx(XbBuilderNode *bn, const gchar *key, guint64 value)
{
	g_autofree gchar *value_hex = NULL;
	if (value == 0)
		return;
	value_hex = g_strdup_printf("0x%x", (guint)value);
	xb_builder_node_insert_text(bn, key, value_hex, NULL);
}

/**
 * fu_xmlb_builder_insert_kb:
 * @bn: #XbBuilderNode
 * @key: string key
 * @value: boolean value
 *
 * Convenience function to add an XML node with a boolean value.
 *
 * Since: 1.6.0
 **/
void
fu_xmlb_builder_insert_kb(XbBuilderNode *bn, const gchar *key, gboolean value)
{
	xb_builder_node_insert_text(bn, key, value ? "true" : "false", NULL);
}

/**
 * fu_snap_is_in_snap:
 *
 * Check whether the current process is running inside a snap.
 *
 * Returns: TRUE if current process is running inside a snap.
 *
 * Since: 2.0.4
 **/
gboolean
fu_snap_is_in_snap(void)
{
	return getenv("SNAP") != NULL;
}
