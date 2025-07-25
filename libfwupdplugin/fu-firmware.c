/*
 * Copyright 2019 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuFirmware"

#include "config.h"

#include "fu-byte-array.h"
#include "fu-bytes.h"
#include "fu-chunk-private.h"
#include "fu-common.h"
#include "fu-firmware.h"
#include "fu-input-stream.h"
#include "fu-mem.h"
#include "fu-partial-input-stream.h"

/**
 * FuFirmware:
 *
 * A firmware file which can have children which represent the images within.
 *
 * See also: [class@FuDfuFirmware], [class@FuIhexFirmware], [class@FuSrecFirmware]
 */

typedef struct {
	FuFirmwareFlags flags;
	FuFirmware *parent; /* noref */
	GPtrArray *images;  /* FuFirmware */
	gchar *version;
	guint64 version_raw;
	FwupdVersionFormat version_format;
	GBytes *bytes;
	GInputStream *stream;
	gsize streamsz;
	FuFirmwareAlignment alignment;
	gchar *id;
	gchar *filename;
	guint64 idx;
	guint64 addr;
	guint64 offset;
	gsize size;
	gsize size_max;
	guint images_max;
	guint depth;
	GPtrArray *chunks;  /* nullable, element-type FuChunk */
	GPtrArray *patches; /* nullable, element-type FuFirmwarePatch */
} FuFirmwarePrivate;

G_DEFINE_TYPE_WITH_PRIVATE(FuFirmware, fu_firmware, G_TYPE_OBJECT)
#define GET_PRIVATE(o) (fu_firmware_get_instance_private(o))

enum { PROP_0, PROP_PARENT, PROP_LAST };

#define FU_FIRMWARE_IMAGE_DEPTH_MAX 50

typedef struct {
	gsize offset;
	GBytes *blob;
} FuFirmwarePatch;

static void
fu_firmware_patch_free(FuFirmwarePatch *ptch)
{
	g_bytes_unref(ptch->blob);
	g_free(ptch);
}

/**
 * fu_firmware_add_flag:
 * @firmware: a #FuFirmware
 * @flag: the firmware flag
 *
 * Adds a specific firmware flag to the firmware.
 *
 * Since: 1.5.0
 **/
void
fu_firmware_add_flag(FuFirmware *firmware, FuFirmwareFlags flag)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(firmware);
	g_return_if_fail(FU_IS_FIRMWARE(firmware));
	priv->flags |= flag;
}

/**
 * fu_firmware_has_flag:
 * @firmware: a #FuFirmware
 * @flag: the firmware flag
 *
 * Finds if the firmware has a specific firmware flag.
 *
 * Returns: %TRUE if the flag is set
 *
 * Since: 1.5.0
 **/
gboolean
fu_firmware_has_flag(FuFirmware *firmware, FuFirmwareFlags flag)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(firmware);
	g_return_val_if_fail(FU_IS_FIRMWARE(firmware), FALSE);
	return (priv->flags & flag) > 0;
}

/**
 * fu_firmware_get_version:
 * @self: a #FuFirmware
 *
 * Gets an optional version that represents the firmware.
 *
 * Returns: a string, or %NULL
 *
 * Since: 1.3.3
 **/
const gchar *
fu_firmware_get_version(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	return priv->version;
}

/**
 * fu_firmware_set_version:
 * @self: a #FuFirmware
 * @version: (nullable): optional string version
 *
 * Sets an optional version that represents the firmware.
 *
 * Since: 1.3.3
 **/
void
fu_firmware_set_version(FuFirmware *self, const gchar *version)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));

	/* not changed */
	if (g_strcmp0(priv->version, version) == 0)
		return;

	g_free(priv->version);
	priv->version = g_strdup(version);
}

/**
 * fu_firmware_get_version_raw:
 * @self: a #FuFirmware
 *
 * Gets an raw version that represents the firmware. This is most frequently
 * used when building firmware with `<version_raw>0x123456</version_raw>` in a
 * `firmware.builder.xml` file to avoid string splitting and sanity checks.
 *
 * Returns: an integer, or %G_MAXUINT64 for invalid
 *
 * Since:  1.5.7
 **/
guint64
fu_firmware_get_version_raw(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), G_MAXUINT64);
	return priv->version_raw;
}

/**
 * fu_firmware_set_version_raw:
 * @self: a #FuFirmware
 * @version_raw: a raw version, or %G_MAXUINT64 for invalid
 *
 * Sets an raw version that represents the firmware.
 *
 * This is optional, and is typically only used for debugging.
 *
 * Since: 1.5.7
 **/
void
fu_firmware_set_version_raw(FuFirmware *self, guint64 version_raw)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);

	g_return_if_fail(FU_IS_FIRMWARE(self));

	priv->version_raw = version_raw;

	/* convert this */
	if (klass->convert_version != NULL) {
		g_autofree gchar *version = klass->convert_version(self, version_raw);
		if (version != NULL)
			fu_firmware_set_version(self, version);
	}
}

/**
 * fu_firmware_get_version_format:
 * @self: a #FuFirmware
 *
 * Gets the version format.
 *
 * Returns: the version format, or %FWUPD_VERSION_FORMAT_UNKNOWN if unset
 *
 * Since: 2.0.0
 **/
FwupdVersionFormat
fu_firmware_get_version_format(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), FWUPD_VERSION_FORMAT_UNKNOWN);
	return priv->version_format;
}

/**
 * fu_firmware_set_version_format:
 * @self: a #FuFirmware
 * @version_format: the version format, e.g. %FWUPD_VERSION_FORMAT_NUMBER
 *
 * Sets the version format.
 *
 * Since: 2.0.0
 **/
void
fu_firmware_set_version_format(FuFirmware *self, FwupdVersionFormat version_format)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);

	g_return_if_fail(FU_IS_FIRMWARE(self));

	/* same */
	if (priv->version_format == version_format)
		return;
	priv->version_format = version_format;

	/* convert this, now we know */
	if (klass->convert_version != NULL && priv->version != NULL && priv->version_raw != 0) {
		g_autofree gchar *version = klass->convert_version(self, priv->version_raw);
		fu_firmware_set_version(self, version);
	}
}

/**
 * fu_firmware_get_filename:
 * @self: a #FuFirmware
 *
 * Gets an optional filename that represents the image source or destination.
 *
 * Returns: a string, or %NULL
 *
 * Since: 1.6.0
 **/
const gchar *
fu_firmware_get_filename(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	return priv->filename;
}

/**
 * fu_firmware_set_filename:
 * @self: a #FuFirmware
 * @filename: (nullable): a string filename
 *
 * Sets an optional filename that represents the image source or destination.
 *
 * Since: 1.6.0
 **/
void
fu_firmware_set_filename(FuFirmware *self, const gchar *filename)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));

	/* not changed */
	if (g_strcmp0(priv->filename, filename) == 0)
		return;

	g_free(priv->filename);
	priv->filename = g_strdup(filename);
}

/**
 * fu_firmware_set_id:
 * @self: a #FuPlugin
 * @id: (nullable): image ID, e.g. `config`
 *
 * Since: 1.6.0
 **/
void
fu_firmware_set_id(FuFirmware *self, const gchar *id)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));

	/* not changed */
	if (g_strcmp0(priv->id, id) == 0)
		return;

	g_free(priv->id);
	priv->id = g_strdup(id);
}

/**
 * fu_firmware_get_id:
 * @self: a #FuPlugin
 *
 * Gets the image ID, typically set at construction.
 *
 * Returns: image ID, e.g. `config`
 *
 * Since: 1.6.0
 **/
const gchar *
fu_firmware_get_id(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	return priv->id;
}

/**
 * fu_firmware_set_addr:
 * @self: a #FuPlugin
 * @addr: integer
 *
 * Sets the base address of the image.
 *
 * Since: 1.6.0
 **/
void
fu_firmware_set_addr(FuFirmware *self, guint64 addr)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	priv->addr = addr;
}

/**
 * fu_firmware_get_addr:
 * @self: a #FuPlugin
 *
 * Gets the base address of the image.
 *
 * Returns: integer
 *
 * Since: 1.6.0
 **/
guint64
fu_firmware_get_addr(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), G_MAXUINT64);
	return priv->addr;
}

/**
 * fu_firmware_set_offset:
 * @self: a #FuPlugin
 * @offset: integer
 *
 * Sets the base offset of the image.
 *
 * Since: 1.6.0
 **/
void
fu_firmware_set_offset(FuFirmware *self, guint64 offset)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	priv->offset = offset;
}

/**
 * fu_firmware_get_offset:
 * @self: a #FuPlugin
 *
 * Gets the base offset of the image.
 *
 * Returns: integer
 *
 * Since: 1.6.0
 **/
guint64
fu_firmware_get_offset(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), G_MAXUINT64);
	return priv->offset;
}

/**
 * fu_firmware_get_parent:
 * @self: a #FuFirmware
 *
 * Gets the parent.
 *
 * Returns: (transfer none): the parent firmware, or %NULL if unset
 *
 * Since: 1.8.2
 **/
FuFirmware *
fu_firmware_get_parent(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	return priv->parent;
}

/**
 * fu_firmware_set_parent:
 * @self: a #FuFirmware
 * @parent: (nullable): another #FuFirmware
 *
 * Sets the parent. Only used internally.
 *
 * Since: 1.8.2
 **/
void
fu_firmware_set_parent(FuFirmware *self, FuFirmware *parent)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));

	if (priv->parent != NULL)
		g_object_remove_weak_pointer(G_OBJECT(priv->parent), (gpointer *)&priv->parent);
	if (parent != NULL)
		g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&priv->parent);
	priv->parent = parent;
}

/**
 * fu_firmware_set_size:
 * @self: a #FuPlugin
 * @size: integer
 *
 * Sets the total size of the image, which should be the same size as the
 * data from fu_firmware_write().
 *
 * Since: 1.6.0
 **/
void
fu_firmware_set_size(FuFirmware *self, gsize size)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	priv->size = size;
}

/**
 * fu_firmware_get_size:
 * @self: a #FuPlugin
 *
 * Gets the total size of the image, which is typically the same size as the
 * data from fu_firmware_write().
 *
 * If the size has not been explicitly set, and fu_firmware_set_bytes() has been
 * used then the size of this is used instead.
 *
 * Returns: integer
 *
 * Since: 1.6.0
 **/
gsize
fu_firmware_get_size(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), G_MAXSIZE);
	if (priv->size != 0)
		return priv->size;
	if (priv->stream != NULL && priv->streamsz != 0)
		return priv->streamsz;
	if (priv->bytes != NULL)
		return g_bytes_get_size(priv->bytes);
	return 0;
}

/**
 * fu_firmware_set_size_max:
 * @self: a #FuPlugin
 * @size_max: integer, or 0 for no limit
 *
 * Sets the maximum size of the image allowed during parsing.
 * Implementations should query fu_firmware_get_size_max() during parsing when adding images to
 * ensure the limit is not exceeded.
 *
 * Since: 1.9.7
 **/
void
fu_firmware_set_size_max(FuFirmware *self, gsize size_max)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	priv->size_max = size_max;
}

/**
 * fu_firmware_get_size_max:
 * @self: a #FuPlugin
 *
 * Gets the maximum size of the image allowed during parsing.
 *
 * Returns: integer, or 0 if not set
 *
 * Since: 1.9.7
 **/
gsize
fu_firmware_get_size_max(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), G_MAXSIZE);
	return priv->size_max;
}

/**
 * fu_firmware_set_idx:
 * @self: a #FuPlugin
 * @idx: integer
 *
 * Sets the index of the image which is used for ordering.
 *
 * Since: 1.6.0
 **/
void
fu_firmware_set_idx(FuFirmware *self, guint64 idx)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	priv->idx = idx;
}

/**
 * fu_firmware_get_idx:
 * @self: a #FuPlugin
 *
 * Gets the index of the image which is used for ordering.
 *
 * Returns: integer
 *
 * Since: 1.6.0
 **/
guint64
fu_firmware_get_idx(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), G_MAXUINT64);
	return priv->idx;
}

/**
 * fu_firmware_set_bytes:
 * @self: a #FuPlugin
 * @bytes: data blob
 *
 * Sets the contents of the image if not created with fu_firmware_new_from_bytes().
 *
 * Since: 1.6.0
 **/
void
fu_firmware_set_bytes(FuFirmware *self, GBytes *bytes)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	g_return_if_fail(bytes != NULL);
	if (priv->bytes == bytes)
		return;
	if (priv->bytes != NULL)
		g_bytes_unref(priv->bytes);
	priv->bytes = g_bytes_ref(bytes);

	/* the input stream is no longer valid */
	g_clear_object(&priv->stream);
}

/**
 * fu_firmware_get_bytes:
 * @self: a #FuPlugin
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware payload, which does not have any header or footer included.
 *
 * If there is more than one potential payload or image section then fu_firmware_add_image()
 * should be used instead.
 *
 * Returns: (transfer full): a #GBytes, or %NULL if the payload has never been set
 *
 * Since: 1.6.0
 **/
GBytes *
fu_firmware_get_bytes(FuFirmware *self, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	if (priv->bytes != NULL)
		return g_bytes_ref(priv->bytes);
	if (priv->stream != NULL) {
		if (priv->streamsz == 0) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_DATA,
					    "stream size unknown");
			return NULL;
		}
		return fu_input_stream_read_bytes(priv->stream, 0x0, priv->streamsz, NULL, error);
	}
	g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no payload set");
	return NULL;
}

/**
 * fu_firmware_get_bytes_with_patches:
 * @self: a #FuPlugin
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware payload, with any defined patches applied.
 *
 * Returns: (transfer full): a #GBytes, or %NULL if the payload has never been set
 *
 * Since: 1.7.4
 **/
GBytes *
fu_firmware_get_bytes_with_patches(FuFirmware *self, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_autoptr(GByteArray) buf = g_byte_array_new();

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);

	if (priv->bytes == NULL) {
		if (priv->stream != NULL)
			return fu_firmware_get_bytes(self, error);
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no payload set");
		return NULL;
	}

	/* usual case */
	if (priv->patches == NULL)
		return fu_firmware_get_bytes(self, error);

	/* convert to a mutable buffer, apply each patch, aborting if the offset isn't valid */
	fu_byte_array_append_bytes(buf, priv->bytes);
	for (guint i = 0; i < priv->patches->len; i++) {
		FuFirmwarePatch *ptch = g_ptr_array_index(priv->patches, i);
		if (!fu_memcpy_safe(buf->data,
				    buf->len,
				    ptch->offset, /* dst */
				    g_bytes_get_data(ptch->blob, NULL),
				    g_bytes_get_size(ptch->blob),
				    0x0, /* src */
				    g_bytes_get_size(ptch->blob),
				    error)) {
			g_prefix_error(error, "failed to apply patch @0x%x: ", (guint)ptch->offset);
			return NULL;
		}
	}

	/* success */
	return g_bytes_new(buf->data, buf->len);
}

/**
 * fu_firmware_set_alignment:
 * @self: a #FuFirmware
 * @alignment: a #FuFirmwareAlignment
 *
 * Sets the alignment of the firmware.
 *
 * This allows a firmware to pad to a power of 2 boundary, where @alignment
 * is the bit position to align to.
 *
 * Since: 1.6.0
 **/
void
fu_firmware_set_alignment(FuFirmware *self, FuFirmwareAlignment alignment)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	priv->alignment = alignment;
}

/**
 * fu_firmware_get_stream:
 * @self: a #FuPlugin
 * @error: (nullable): optional return location for an error
 *
 * Gets the input stream which was used to parse the firmware.
 *
 * Returns: (transfer full): a #GInputStream, or %NULL if the payload has never been set
 *
 * Since: 2.0.0
 **/
GInputStream *
fu_firmware_get_stream(FuFirmware *self, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	if (priv->stream != NULL)
		return g_object_ref(priv->stream);
	if (priv->bytes != NULL)
		return g_memory_input_stream_new_from_bytes(priv->bytes);
	g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no stream or bytes set");
	return NULL;
}

/**
 * fu_firmware_set_stream:
 * @self: a #FuPlugin
 * @stream: (nullable): #GInputStream
 * @error: (nullable): optional return location for an error
 *
 * Sets the input stream.
 *
 * Returns: %TRUE on success
 *
 * Since: 2.0.0
 **/
gboolean
fu_firmware_set_stream(FuFirmware *self, GInputStream *stream, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(stream == NULL || G_IS_INPUT_STREAM(stream), FALSE);
	if (stream != NULL) {
		if (!fu_input_stream_size(stream, &priv->streamsz, error))
			return FALSE;
	} else {
		priv->streamsz = 0;
	}
	g_set_object(&priv->stream, stream);
	return TRUE;
}

/**
 * fu_firmware_get_alignment:
 * @self: a #FuFirmware
 *
 * Gets the alignment of the firmware.
 *
 * This allows a firmware to pad to a power of 2 boundary, where @alignment
 * is the bit position to align to.
 *
 * Returns: a #FuFirmwareAlignment
 *
 * Since: 1.6.0
 **/
FuFirmwareAlignment
fu_firmware_get_alignment(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), FU_FIRMWARE_ALIGNMENT_LAST);
	return priv->alignment;
}

/**
 * fu_firmware_get_chunks:
 * @self: a #FuFirmware
 * @error: (nullable): optional return location for an error
 *
 * Gets the optional image chunks.
 *
 * Returns: (transfer container) (element-type FuChunk) (nullable): chunk data, or %NULL
 *
 * Since: 1.6.0
 **/
GPtrArray *
fu_firmware_get_chunks(FuFirmware *self, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* set */
	if (priv->chunks != NULL)
		return g_ptr_array_ref(priv->chunks);

	/* lets build something plausible */
	if (priv->bytes != NULL) {
		g_autoptr(GPtrArray) chunks = NULL;
		g_autoptr(FuChunk) chk = NULL;
		chunks = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
		chk = fu_chunk_bytes_new(priv->bytes);
		fu_chunk_set_idx(chk, priv->idx);
		fu_chunk_set_address(chk, priv->addr);
		g_ptr_array_add(chunks, g_steal_pointer(&chk));
		return g_steal_pointer(&chunks);
	}

	/* nothing to do */
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "no bytes or chunks found in firmware");
	return NULL;
}

/**
 * fu_firmware_add_chunk:
 * @self: a #FuFirmware
 * @chk: a #FuChunk
 *
 * Adds a chunk to the image.
 *
 * Since: 1.6.0
 **/
void
fu_firmware_add_chunk(FuFirmware *self, FuChunk *chk)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	g_return_if_fail(FU_IS_CHUNK(chk));
	if (priv->chunks == NULL)
		priv->chunks = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	g_ptr_array_add(priv->chunks, g_object_ref(chk));
}

/**
 * fu_firmware_get_checksum:
 * @self: a #FuPlugin
 * @csum_kind: a checksum type, e.g. %G_CHECKSUM_SHA256
 * @error: (nullable): optional return location for an error
 *
 * Returns a checksum of the payload data.
 *
 * Returns: (transfer full): a checksum string, or %NULL if the checksum is not available
 *
 * Since: 1.6.0
 **/
gchar *
fu_firmware_get_checksum(FuFirmware *self, GChecksumType csum_kind, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);
	g_autoptr(GBytes) blob = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* subclassed */
	if (klass->get_checksum != NULL) {
		g_autoptr(GError) error_local = NULL;
		g_autofree gchar *checksum = klass->get_checksum(self, csum_kind, &error_local);
		if (checksum != NULL)
			return g_steal_pointer(&checksum);
		if (!g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
			g_propagate_error(error, g_steal_pointer(&error_local));
			return NULL;
		}
	}

	/* internal data */
	if (priv->bytes != NULL)
		return g_compute_checksum_for_bytes(csum_kind, priv->bytes);
	if (priv->stream != NULL)
		return fu_input_stream_compute_checksum(priv->stream, csum_kind, error);

	/* write */
	blob = fu_firmware_write(self, error);
	if (blob == NULL)
		return NULL;
	return g_compute_checksum_for_bytes(csum_kind, blob);
}

/**
 * fu_firmware_tokenize:
 * @self: a #FuFirmware
 * @stream: a #GInputStream
 * @flags: #FuFirmwareParseFlags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Tokenizes a firmware, typically breaking the firmware into records.
 *
 * Records can be enumerated using subclass-specific functionality, for example
 * using fu_srec_firmware_get_records().
 *
 * Returns: %TRUE for success
 *
 * Since: 2.0.0
 **/
gboolean
fu_firmware_tokenize(FuFirmware *self,
		     GInputStream *stream,
		     FuFirmwareParseFlags flags,
		     GError **error)
{
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(G_IS_INPUT_STREAM(stream), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* optionally subclassed */
	if (klass->tokenize != NULL)
		return klass->tokenize(self, stream, flags, error);
	return TRUE;
}

/**
 * fu_firmware_check_compatible:
 * @self: a #FuFirmware
 * @other: a #FuFirmware
 * @flags: #FuFirmwareParseFlags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Check a new firmware is compatible with the existing firmware.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.8.4
 **/
gboolean
fu_firmware_check_compatible(FuFirmware *self,
			     FuFirmware *other,
			     FuFirmwareParseFlags flags,
			     GError **error)
{
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(FU_IS_FIRMWARE(other), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* optionally subclassed */
	if (klass->check_compatible == NULL)
		return TRUE;
	return klass->check_compatible(self, other, flags, error);
}

static gboolean
fu_firmware_validate_for_offset(FuFirmware *self,
				GInputStream *stream,
				gsize *offset,
				FuFirmwareParseFlags flags,
				GError **error)
{
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);
	gsize streamsz = 0;

	/* not implemented */
	if (klass->validate == NULL)
		return TRUE;

	/* fuzzing */
	if (!fu_firmware_has_flag(self, FU_FIRMWARE_FLAG_ALWAYS_SEARCH) &&
	    (flags & FU_FIRMWARE_PARSE_FLAG_NO_SEARCH) > 0) {
		if (!klass->validate(self, stream, *offset, error))
			return FALSE;
		return TRUE;
	}

	/* limit the size of firmware we search */
	if (!fu_input_stream_size(stream, &streamsz, error))
		return FALSE;
	if (streamsz > FU_FIRMWARE_SEARCH_MAGIC_BUFSZ_MAX) {
		if (!klass->validate(self, stream, *offset, error)) {
			g_prefix_error(error,
				       "failed to search for magic as firmware size was 0x%x and "
				       "limit was 0x%x: ",
				       (guint)streamsz,
				       (guint)FU_FIRMWARE_SEARCH_MAGIC_BUFSZ_MAX);
			return FALSE;
		}
		return TRUE;
	}

	/* increment the offset, looking for the magic */
	for (gsize offset_tmp = *offset; offset_tmp < streamsz; offset_tmp++) {
		if (klass->validate(self, stream, offset_tmp, NULL)) {
			fu_firmware_set_offset(self, offset_tmp);
			*offset = offset_tmp;
			return TRUE;
		}
	}

	/* did not find what we were looking for */
	g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE, "did not find magic");
	return FALSE;
}

/**
 * fu_firmware_parse_stream:
 * @self: a #FuFirmware
 * @stream: input stream
 * @offset: start offset
 * @flags: #FuFirmwareParseFlags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Parses a firmware from a stream, typically breaking the firmware into images.
 *
 * Returns: %TRUE for success
 *
 * Since: 2.0.0
 **/
gboolean
fu_firmware_parse_stream(FuFirmware *self,
			 GInputStream *stream,
			 gsize offset,
			 FuFirmwareParseFlags flags,
			 GError **error)
{
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	gsize streamsz = 0;
	g_autoptr(GInputStream) partial_stream = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(G_IS_INPUT_STREAM(stream), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* sanity check */
	if (fu_firmware_has_flag(self, FU_FIRMWARE_FLAG_DONE_PARSE)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "firmware object cannot be reused");
		return FALSE;
	}

	/* check size */
	if (!fu_input_stream_size(stream, &streamsz, error))
		return FALSE;
	if (streamsz <= offset) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "stream size 0x%x is smaller than offset 0x%x",
			    (guint)streamsz,
			    (guint)offset);
		return FALSE;
	}

	/* optional */
	if (!fu_firmware_validate_for_offset(self, stream, &offset, flags, error))
		return FALSE;

	/* save stream size */
	priv->streamsz = streamsz - offset;
	if (priv->streamsz == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "invalid firmware as zero sized");
		return FALSE;
	}
	if (priv->size_max > 0 && priv->streamsz > priv->size_max) {
		g_autofree gchar *sz_val = g_format_size(priv->streamsz);
		g_autofree gchar *sz_max = g_format_size(priv->size_max);
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_FILE,
			    "firmware is too large (%s, limit %s)",
			    sz_val,
			    sz_max);
		return FALSE;
	}

	/* any FuFirmware subclass that gets past this point might have allocated memory in
	 * ->tokenize() or ->parse() and needs to be destroyed before parsing again */
	fu_firmware_add_flag(self, FU_FIRMWARE_FLAG_DONE_PARSE);

	/* this allows devices to skip reading the old firmware if the GType is unsuitable */
	if (klass->check_compatible != NULL)
		fu_firmware_add_flag(self, FU_FIRMWARE_FLAG_HAS_CHECK_COMPATIBLE);

	/* save stream */
	if (offset == 0) {
		partial_stream = g_object_ref(stream);
	} else {
		partial_stream = fu_partial_input_stream_new(stream, offset, priv->streamsz, error);
		if (partial_stream == NULL) {
			g_prefix_error(error, "failed to cut firmware: ");
			return FALSE;
		}
	}

	/* cache */
	if (flags & FU_FIRMWARE_PARSE_FLAG_CACHE_BLOB) {
		g_autoptr(GBytes) blob = NULL;
		blob = fu_input_stream_read_bytes(partial_stream, 0x0, priv->streamsz, NULL, error);
		if (blob == NULL)
			return FALSE;
		fu_firmware_set_bytes(self, blob);
	}
	if (flags & FU_FIRMWARE_PARSE_FLAG_CACHE_STREAM) {
		g_set_object(&priv->stream, partial_stream);
	}

	/* optional */
	if (klass->tokenize != NULL) {
		if (!klass->tokenize(self, partial_stream, flags, error))
			return FALSE;
	}

	/* optional */
	if (klass->parse != NULL)
		return klass->parse(self, partial_stream, flags, error);

	/* verify alignment */
	if (streamsz % (1ull << priv->alignment) != 0) {
		g_autofree gchar *str = NULL;
		str = g_format_size_full(1ull << priv->alignment, G_FORMAT_SIZE_IEC_UNITS);
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_FILE,
			    "raw firmware is not aligned to 0x%x (%s)",
			    (guint)(1ull << priv->alignment),
			    str);
		return FALSE;
	}

	/* success */
	return TRUE;
}

/**
 * fu_firmware_parse_bytes:
 * @self: a #FuFirmware
 * @fw: firmware blob
 * @offset: start offset, useful for ignoring a bootloader
 * @flags: #FuFirmwareParseFlags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Parses a firmware, typically breaking the firmware into images.
 *
 * Returns: %TRUE for success
 *
 * Since: 2.0.1
 **/
gboolean
fu_firmware_parse_bytes(FuFirmware *self,
			GBytes *fw,
			gsize offset,
			FuFirmwareParseFlags flags,
			GError **error)
{
	g_autoptr(GInputStream) stream = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(fw != NULL, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	stream = g_memory_input_stream_new_from_bytes(fw);
	return fu_firmware_parse_stream(self, stream, offset, flags, error);
}

/**
 * fu_firmware_build:
 * @self: a #FuFirmware
 * @n: a Xmlb node
 * @error: (nullable): optional return location for an error
 *
 * Builds a firmware from an XML manifest. The manifest would typically have the
 * following form:
 *
 * |[<!-- language="XML" -->
 * <?xml version="1.0" encoding="UTF-8"?>
 * <firmware gtype="FuBcm57xxFirmware">
 *   <version>1.2.3</version>
 *   <firmware gtype="FuBcm57xxStage1Image">
 *     <version>7.8.9</version>
 *     <id>stage1</id>
 *     <idx>0x01</idx>
 *     <filename>stage1.bin</filename>
 *   </firmware>
 *   <firmware gtype="FuBcm57xxStage2Image">
 *     <id>stage2</id>
 *     <data/> <!-- empty! -->
 *   </firmware>
 *   <firmware gtype="FuBcm57xxDictImage">
 *     <id>ape</id>
 *     <addr>0x7</addr>
 *     <data>aGVsbG8gd29ybGQ=</data> <!-- base64 -->
 *   </firmware>
 * </firmware>
 * ]|
 *
 * This would be used in a build-system to merge images from generated files:
 * `fwupdtool firmware-build fw.builder.xml test.fw`
 *
 * Static binary content can be specified in the `<firmware>/<data>` section and
 * is encoded as base64 text if not empty.
 *
 * Additionally, extra nodes can be included under nested `<firmware>` objects
 * which can be parsed by the subclassed objects. You should verify the
 * subclassed object `FuFirmware->build` vfunc for the specific additional
 * options supported.
 *
 * Plugins should manually g_type_ensure() subclassed image objects if not
 * constructed as part of the plugin fu_plugin_init() or fu_plugin_setup()
 * functions.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.5.0
 **/
gboolean
fu_firmware_build(FuFirmware *self, XbNode *n, GError **error)
{
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);
	const gchar *tmp;
	guint64 tmpval;
	guint64 version_raw;
	g_autoptr(GPtrArray) chunks = NULL;
	g_autoptr(GPtrArray) xb_images = NULL;
	g_autoptr(XbNode) data = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(XB_IS_NODE(n), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* set attributes */
	tmp = xb_node_query_text(n, "version", NULL);
	if (tmp != NULL)
		fu_firmware_set_version(self, tmp);
	tmp = xb_node_query_text(n, "version_format", NULL);
	if (tmp != NULL) {
		FwupdVersionFormat version_format = fwupd_version_format_from_string(tmp);
		if (version_format == FWUPD_VERSION_FORMAT_UNKNOWN) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "%s is not a valid version format",
				    tmp);
			return FALSE;
		}
		fu_firmware_set_version_format(self, version_format);
	}
	version_raw = xb_node_query_text_as_uint(n, "version_raw", NULL);
	if (version_raw != G_MAXUINT64)
		fu_firmware_set_version_raw(self, version_raw);
	tmp = xb_node_query_text(n, "id", NULL);
	if (tmp != NULL)
		fu_firmware_set_id(self, tmp);
	tmpval = xb_node_query_text_as_uint(n, "idx", NULL);
	if (tmpval != G_MAXUINT64)
		fu_firmware_set_idx(self, tmpval);
	tmpval = xb_node_query_text_as_uint(n, "addr", NULL);
	if (tmpval != G_MAXUINT64)
		fu_firmware_set_addr(self, tmpval);
	tmpval = xb_node_query_text_as_uint(n, "offset", NULL);
	if (tmpval != G_MAXUINT64)
		fu_firmware_set_offset(self, tmpval);
	tmpval = xb_node_query_text_as_uint(n, "size", NULL);
	if (tmpval != G_MAXUINT64)
		fu_firmware_set_size(self, tmpval);
	tmpval = xb_node_query_text_as_uint(n, "size_max", NULL);
	if (tmpval != G_MAXUINT64)
		fu_firmware_set_size_max(self, tmpval);
	tmpval = xb_node_query_text_as_uint(n, "alignment", NULL);
	if (tmpval != G_MAXUINT64) {
		if (tmpval > FU_FIRMWARE_ALIGNMENT_2G) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "0x%x invalid, maximum is 0x%x",
				    (guint)tmpval,
				    (guint)FU_FIRMWARE_ALIGNMENT_2G);
			return FALSE;
		}
		fu_firmware_set_alignment(self, (guint8)tmpval);
	}
	tmp = xb_node_query_text(n, "filename", NULL);
	if (tmp != NULL) {
		g_autoptr(GBytes) blob = NULL;
		blob = fu_bytes_get_contents(tmp, error);
		if (blob == NULL)
			return FALSE;
		fu_firmware_set_bytes(self, blob);
		fu_firmware_set_filename(self, tmp);
	}
	data = xb_node_query_first(n, "data", NULL);
	if (data != NULL) {
		guint64 sz = xb_node_get_attr_as_uint(data, "size");
		g_autoptr(GBytes) blob = NULL;

		/* base64 encoded data */
		if (xb_node_get_text(data) != NULL) {
			gsize bufsz = 0;
			g_autofree guchar *buf = NULL;
			buf = g_base64_decode(xb_node_get_text(data), &bufsz);
			blob = g_bytes_new(buf, bufsz);
		} else {
			blob = g_bytes_new(NULL, 0);
		}

		/* padding is optional */
		if (sz == 0 || sz == G_MAXUINT64) {
			fu_firmware_set_bytes(self, blob);
		} else {
			g_autoptr(GBytes) blob_padded = fu_bytes_pad(blob, (gsize)sz, 0xFF);
			fu_firmware_set_bytes(self, blob_padded);
		}
	}

	/* optional chunks */
	chunks = xb_node_query(n, "chunks/chunk", 0, NULL);
	if (chunks != NULL) {
		for (guint i = 0; i < chunks->len; i++) {
			XbNode *c = g_ptr_array_index(chunks, i);
			g_autoptr(FuChunk) chk = fu_chunk_bytes_new(NULL);
			fu_chunk_set_idx(chk, i);
			if (!fu_chunk_build(chk, c, error))
				return FALSE;
			fu_firmware_add_chunk(self, chk);
		}
	}

	/* parse images */
	xb_images = xb_node_query(n, "firmware", 0, NULL);
	if (xb_images != NULL) {
		for (guint i = 0; i < xb_images->len; i++) {
			XbNode *xb_image = g_ptr_array_index(xb_images, i);
			g_autoptr(FuFirmware) img = NULL;
			tmp = xb_node_get_attr(xb_image, "gtype");
			if (tmp != NULL) {
				GType gtype = g_type_from_name(tmp);
				if (gtype == G_TYPE_INVALID) {
					g_set_error(error,
						    FWUPD_ERROR,
						    FWUPD_ERROR_NOT_FOUND,
						    "GType %s not registered",
						    tmp);
					return FALSE;
				}
				img = g_object_new(gtype, NULL);
			} else {
				img = fu_firmware_new();
			}
			if (!fu_firmware_add_image_full(self, img, error))
				return FALSE;
			if (!fu_firmware_build(img, xb_image, error))
				return FALSE;
		}
	}

	/* subclassed */
	if (klass->build != NULL) {
		if (!klass->build(self, n, error))
			return FALSE;
	}

	/* success */
	return TRUE;
}

/**
 * fu_firmware_build_from_xml:
 * @self: a #FuFirmware
 * @xml: XML text
 * @error: (nullable): optional return location for an error
 *
 * Builds a firmware from an XML manifest. The manifest would typically have the
 * following form:
 *
 * |[<!-- language="XML" -->
 * <?xml version="1.0" encoding="UTF-8"?>
 * <firmware gtype="FuBcm57xxFirmware">
 *   <version>1.2.3</version>
 *   <firmware gtype="FuBcm57xxStage1Image">
 *     <version>7.8.9</version>
 *     <id>stage1</id>
 *     <idx>0x01</idx>
 *     <filename>stage1.bin</filename>
 *   </firmware>
 *   <firmware gtype="FuBcm57xxStage2Image">
 *     <id>stage2</id>
 *     <data/> <!-- empty! -->
 *   </firmware>
 *   <firmware gtype="FuBcm57xxDictImage">
 *     <id>ape</id>
 *     <addr>0x7</addr>
 *     <data>aGVsbG8gd29ybGQ=</data> <!-- base64 -->
 *   </firmware>
 * </firmware>
 * ]|
 *
 * This would be used in a build-system to merge images from generated files:
 * `fwupdtool firmware-build fw.builder.xml test.fw`
 *
 * Static binary content can be specified in the `<firmware>/<data>` section and
 * is encoded as base64 text if not empty.
 *
 * Additionally, extra nodes can be included under nested `<firmware>` objects
 * which can be parsed by the subclassed objects. You should verify the
 * subclassed object `FuFirmware->build` vfunc for the specific additional
 * options supported.
 *
 * Plugins should manually g_type_ensure() subclassed image objects if not
 * constructed as part of the plugin fu_plugin_init() or fu_plugin_setup()
 * functions.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.6.0
 **/
gboolean
fu_firmware_build_from_xml(FuFirmware *self, const gchar *xml, GError **error)
{
	g_autoptr(XbBuilder) builder = xb_builder_new();
	g_autoptr(XbBuilderSource) source = xb_builder_source_new();
	g_autoptr(XbNode) n = NULL;
	g_autoptr(XbSilo) silo = NULL;

	/* parse XML */
	if (!xb_builder_source_load_xml(source, xml, XB_BUILDER_SOURCE_FLAG_NONE, error)) {
		g_prefix_error(error, "could not parse XML: ");
		fwupd_error_convert(error);
		return FALSE;
	}
	xb_builder_import_source(builder, source);
	silo = xb_builder_compile(builder, XB_BUILDER_COMPILE_FLAG_NONE, NULL, error);
	if (silo == NULL) {
		fwupd_error_convert(error);
		return FALSE;
	}

	/* create FuFirmware of specific GType */
	n = xb_silo_query_first(silo, "firmware", error);
	if (n == NULL) {
		fwupd_error_convert(error);
		return FALSE;
	}
	return fu_firmware_build(self, n, error);
}

/**
 * fu_firmware_build_from_filename:
 * @self: a #FuFirmware
 * @filename: filename of XML builder
 * @error: (nullable): optional return location for an error
 *
 * Builds a firmware from an XML manifest.
 *
 * Returns: %TRUE for success
 *
 * Since: 2.0.0
 **/
gboolean
fu_firmware_build_from_filename(FuFirmware *self, const gchar *filename, GError **error)
{
	g_autofree gchar *xml = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(filename != NULL, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (!g_file_get_contents(filename, &xml, NULL, error))
		return FALSE;
	return fu_firmware_build_from_xml(self, xml, error);
}

/**
 * fu_firmware_parse_file:
 * @self: a #FuFirmware
 * @file: a file
 * @flags: #FuFirmwareParseFlags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Parses a firmware file, typically breaking the firmware into images.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.3.3
 **/
gboolean
fu_firmware_parse_file(FuFirmware *self, GFile *file, FuFirmwareParseFlags flags, GError **error)
{
	g_autoptr(GFileInputStream) stream = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(G_IS_FILE(file), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	stream = g_file_read(file, NULL, error);
	if (stream == NULL) {
		fwupd_error_convert(error);
		return FALSE;
	}
	return fu_firmware_parse_stream(self, G_INPUT_STREAM(stream), 0, flags, error);
}

/**
 * fu_firmware_write:
 * @self: a #FuFirmware
 * @error: (nullable): optional return location for an error
 *
 * Writes a firmware, typically packing the images into a binary blob.
 *
 * Returns: (transfer full): a data blob
 *
 * Since: 1.3.1
 **/
GBytes *
fu_firmware_write(FuFirmware *self, GError **error)
{
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* subclassed */
	if (klass->write != NULL) {
		g_autoptr(GByteArray) buf = klass->write(self, error);
		if (buf == NULL)
			return NULL;
		return g_bytes_new(buf->data, buf->len);
	}

	/* just add default blob */
	return fu_firmware_get_bytes_with_patches(self, error);
}

/**
 * fu_firmware_add_patch:
 * @self: a #FuFirmware
 * @offset: an address smaller than fu_firmware_get_size()
 * @blob: (not nullable): bytes to replace
 *
 * Adds a byte patch at a specific offset. If a patch already exists at the specified address then
 * it is replaced.
 *
 * If the @address is larger than the size of the image then an error is returned.
 *
 * Since: 1.7.4
 **/
void
fu_firmware_add_patch(FuFirmware *self, gsize offset, GBytes *blob)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	FuFirmwarePatch *ptch;

	g_return_if_fail(FU_IS_FIRMWARE(self));
	g_return_if_fail(blob != NULL);

	/* ensure exists */
	if (priv->patches == NULL) {
		priv->patches =
		    g_ptr_array_new_with_free_func((GDestroyNotify)fu_firmware_patch_free);
	}

	/* find existing of exact same size */
	for (guint i = 0; i < priv->patches->len; i++) {
		ptch = g_ptr_array_index(priv->patches, i);
		if (ptch->offset == offset &&
		    g_bytes_get_size(ptch->blob) == g_bytes_get_size(blob)) {
			g_bytes_unref(ptch->blob);
			ptch->blob = g_bytes_ref(blob);
			return;
		}
	}

	/* add new */
	ptch = g_new0(FuFirmwarePatch, 1);
	ptch->offset = offset;
	ptch->blob = g_bytes_ref(blob);
	g_ptr_array_add(priv->patches, ptch);
}

/**
 * fu_firmware_write_chunk:
 * @self: a #FuFirmware
 * @address: an address smaller than fu_firmware_get_addr()
 * @chunk_sz_max: the size of the new chunk
 * @error: (nullable): optional return location for an error
 *
 * Gets a block of data from the image. If the contents of the image is
 * smaller than the requested chunk size then the #GBytes will be smaller
 * than @chunk_sz_max. Use fu_bytes_pad() if padding is required.
 *
 * If the @address is larger than the size of the image then an error is returned.
 *
 * Returns: (transfer full): a #GBytes, or %NULL
 *
 * Since: 1.6.0
 **/
GBytes *
fu_firmware_write_chunk(FuFirmware *self, guint64 address, guint64 chunk_sz_max, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	gsize chunk_left;
	guint64 offset;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* check address requested is larger than base address */
	if (address < priv->addr) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "requested address 0x%x less than base address 0x%x",
			    (guint)address,
			    (guint)priv->addr);
		return NULL;
	}

	/* offset into data */
	offset = address - priv->addr;
	if (offset > g_bytes_get_size(priv->bytes)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "offset 0x%x larger than data size 0x%x",
			    (guint)offset,
			    (guint)g_bytes_get_size(priv->bytes));
		return NULL;
	}

	/* if we have less data than requested */
	chunk_left = g_bytes_get_size(priv->bytes) - offset;
	if (chunk_sz_max > chunk_left) {
		return fu_bytes_new_offset(priv->bytes, offset, chunk_left, error);
	}

	/* check chunk */
	return fu_bytes_new_offset(priv->bytes, offset, chunk_sz_max, error);
}

/**
 * fu_firmware_write_file:
 * @self: a #FuFirmware
 * @file: a file
 * @error: (nullable): optional return location for an error
 *
 * Writes a firmware, typically packing the images into a binary blob.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.3.3
 **/
gboolean
fu_firmware_write_file(FuFirmware *self, GFile *file, GError **error)
{
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GFile) parent = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(G_IS_FILE(file), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	blob = fu_firmware_write(self, error);
	if (blob == NULL)
		return FALSE;
	parent = g_file_get_parent(file);
	if (!g_file_query_exists(parent, NULL)) {
		if (!g_file_make_directory_with_parents(parent, NULL, error))
			return FALSE;
	}
	return g_file_replace_contents(file,
				       g_bytes_get_data(blob, NULL),
				       g_bytes_get_size(blob),
				       NULL,
				       FALSE,
				       G_FILE_CREATE_NONE,
				       NULL,
				       NULL,
				       error);
}

static void
fu_firmware_set_depth(FuFirmware *self, guint depth)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	priv->depth = depth;
}

/**
 * fu_firmware_get_depth:
 * @self: a #FuPlugin
 *
 * Gets the depth of this child image relative to the root.
 *
 * Returns: integer, or 0 for the root.
 *
 * Since: 1.9.14
 **/
guint
fu_firmware_get_depth(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), G_MAXUINT);
	return priv->depth;
}

/**
 * fu_firmware_add_image_full:
 * @self: a #FuPlugin
 * @img: a child firmware image
 * @error: (nullable): optional return location for an error
 *
 * Adds an image to the firmware. This method will fail if the number of images would be
 * above the limit set by fu_firmware_set_images_max().
 *
 * If %FU_FIRMWARE_FLAG_DEDUPE_ID is set, an image with the same ID is already
 * present it is replaced.
 *
 * Returns: %TRUE if the image was added
 *
 * Since: 1.9.3
 **/
gboolean
fu_firmware_add_image_full(FuFirmware *self, FuFirmware *img, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(FU_IS_FIRMWARE(img), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* check depth */
	if (priv->depth > FU_FIRMWARE_IMAGE_DEPTH_MAX) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "images are nested too deep, limit is %u",
			    (guint)FU_FIRMWARE_IMAGE_DEPTH_MAX);
		return FALSE;
	}

	/* dedupe */
	if (priv->flags & FU_FIRMWARE_FLAG_DEDUPE_ID) {
		for (guint i = 0; i < priv->images->len; i++) {
			FuFirmware *img_tmp = g_ptr_array_index(priv->images, i);
			if (g_strcmp0(fu_firmware_get_id(img_tmp), fu_firmware_get_id(img)) == 0) {
				g_ptr_array_remove_index(priv->images, i);
				break;
			}
		}
	}
	if (priv->flags & FU_FIRMWARE_FLAG_DEDUPE_IDX) {
		for (guint i = 0; i < priv->images->len; i++) {
			FuFirmware *img_tmp = g_ptr_array_index(priv->images, i);
			if (fu_firmware_get_idx(img_tmp) == fu_firmware_get_idx(img)) {
				g_ptr_array_remove_index(priv->images, i);
				break;
			}
		}
	}

	/* sanity check */
	if (priv->images_max > 0 && priv->images->len >= priv->images_max) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "too many images, limit is %u",
			    priv->images_max);
		return FALSE;
	}

	g_ptr_array_add(priv->images, g_object_ref(img));

	/* set the other way around */
	fu_firmware_set_parent(img, self);
	fu_firmware_set_depth(img, priv->depth + 1);

	/* success */
	return TRUE;
}

/**
 * fu_firmware_add_image:
 * @self: a #FuPlugin
 * @img: a child firmware image
 *
 * Adds an image to the firmware.
 *
 * NOTE: If adding images in a loop of any kind then fu_firmware_add_image_full() should be used
 * instead, and fu_firmware_set_images_max() should be set before adding images.
 *
 * If %FU_FIRMWARE_FLAG_DEDUPE_ID is set, an image with the same ID is already
 * present it is replaced.
 *
 * Since: 1.3.1
 **/
void
fu_firmware_add_image(FuFirmware *self, FuFirmware *img)
{
	g_autoptr(GError) error_local = NULL;

	g_return_if_fail(FU_IS_FIRMWARE(self));
	g_return_if_fail(FU_IS_FIRMWARE(img));

	if (!fu_firmware_add_image_full(self, img, &error_local))
		g_critical("failed to add image: %s", error_local->message);
}

/**
 * fu_firmware_set_images_max:
 * @self: a #FuPlugin
 * @images_max: integer, or 0 for unlimited
 *
 * Sets the maximum number of images this container can hold.
 *
 * Since: 1.9.3
 **/
void
fu_firmware_set_images_max(FuFirmware *self, guint images_max)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_FIRMWARE(self));
	priv->images_max = images_max;
}

/**
 * fu_firmware_get_images_max:
 * @self: a #FuPlugin
 *
 * Gets the maximum number of images this container can hold.
 *
 * Returns: integer, or 0 for unlimited.
 *
 * Since: 1.9.3
 **/
guint
fu_firmware_get_images_max(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_FIRMWARE(self), G_MAXUINT);
	return priv->images_max;
}

/**
 * fu_firmware_remove_image:
 * @self: a #FuPlugin
 * @img: a child firmware image
 * @error: (nullable): optional return location for an error
 *
 * Remove an image from the firmware.
 *
 * Returns: %TRUE if the image was removed
 *
 * Since: 1.5.0
 **/
gboolean
fu_firmware_remove_image(FuFirmware *self, FuFirmware *img, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(FU_IS_FIRMWARE(img), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (g_ptr_array_remove(priv->images, img))
		return TRUE;

	/* did not exist */
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_FOUND,
		    "image %s not found in firmware",
		    fu_firmware_get_id(img));
	return FALSE;
}

/**
 * fu_firmware_remove_image_by_idx:
 * @self: a #FuPlugin
 * @idx: index
 * @error: (nullable): optional return location for an error
 *
 * Removes the first image from the firmware matching the index.
 *
 * Returns: %TRUE if an image was removed
 *
 * Since: 1.5.0
 **/
gboolean
fu_firmware_remove_image_by_idx(FuFirmware *self, guint64 idx, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_autoptr(FuFirmware) img = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	img = fu_firmware_get_image_by_idx(self, idx, error);
	if (img == NULL)
		return FALSE;
	g_ptr_array_remove(priv->images, img);
	return TRUE;
}

/**
 * fu_firmware_remove_image_by_id:
 * @self: a #FuPlugin
 * @id: (nullable): image ID, e.g. `config`
 * @error: (nullable): optional return location for an error
 *
 * Removes the first image from the firmware matching the ID.
 *
 * Returns: %TRUE if an image was removed
 *
 * Since: 1.5.0
 **/
gboolean
fu_firmware_remove_image_by_id(FuFirmware *self, const gchar *id, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_autoptr(FuFirmware) img = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	img = fu_firmware_get_image_by_id(self, id, error);
	if (img == NULL)
		return FALSE;
	g_ptr_array_remove(priv->images, img);
	return TRUE;
}

/**
 * fu_firmware_get_images:
 * @self: a #FuFirmware
 *
 * Returns all the images in the firmware.
 *
 * Returns: (transfer container) (element-type FuFirmware): images
 *
 * Since: 1.3.1
 **/
GPtrArray *
fu_firmware_get_images(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_autoptr(GPtrArray) imgs = NULL;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);

	imgs = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	for (guint i = 0; i < priv->images->len; i++) {
		FuFirmware *img = g_ptr_array_index(priv->images, i);
		g_ptr_array_add(imgs, g_object_ref(img));
	}
	return g_steal_pointer(&imgs);
}

/**
 * fu_firmware_get_image_by_id:
 * @self: a #FuPlugin
 * @id: (nullable): image ID, e.g. `config` or `*.mfg|*.elf`
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image using the image ID.
 *
 * Returns: (transfer full): a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 1.3.1
 **/
FuFirmware *
fu_firmware_get_image_by_id(FuFirmware *self, const gchar *id, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* non-NULL */
	if (id != NULL) {
		g_auto(GStrv) split = g_strsplit(id, "|", 0);
		for (guint i = 0; i < priv->images->len; i++) {
			FuFirmware *img = g_ptr_array_index(priv->images, i);
			for (guint j = 0; split[j] != NULL; j++) {
				if (fu_firmware_get_id(img) == NULL)
					continue;
				if (g_pattern_match_simple(split[j], fu_firmware_get_id(img)))
					return g_object_ref(img);
			}
		}
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "no image id %s found in firmware",
			    id);
		return NULL;
	}

	/* NULL */
	for (guint i = 0; i < priv->images->len; i++) {
		FuFirmware *img = g_ptr_array_index(priv->images, i);
		if (fu_firmware_get_id(img) == NULL)
			return g_object_ref(img);
	}
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "no NULL image id found in firmware");
	return NULL;
}

/**
 * fu_firmware_get_image_by_id_bytes:
 * @self: a #FuPlugin
 * @id: (nullable): image ID, e.g. `config`
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image bytes using the image ID.
 *
 * Returns: (transfer full): a #GBytes of a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 1.3.1
 **/
GBytes *
fu_firmware_get_image_by_id_bytes(FuFirmware *self, const gchar *id, GError **error)
{
	g_autoptr(FuFirmware) img = fu_firmware_get_image_by_id(self, id, error);
	if (img == NULL)
		return NULL;
	return fu_firmware_write(img, error);
}

/**
 * fu_firmware_get_image_by_id_stream:
 * @self: a #FuPlugin
 * @id: (nullable): image ID, e.g. `config`
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image stream using the image ID.
 *
 * Returns: (transfer full): a #GInputStream of a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 2.0.0
 **/
GInputStream *
fu_firmware_get_image_by_id_stream(FuFirmware *self, const gchar *id, GError **error)
{
	g_autoptr(FuFirmware) img = fu_firmware_get_image_by_id(self, id, error);
	if (img == NULL)
		return NULL;
	return fu_firmware_get_stream(img, error);
}

/**
 * fu_firmware_get_image_by_idx:
 * @self: a #FuPlugin
 * @idx: image index
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image using the image index.
 *
 * Returns: (transfer full): a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 1.3.1
 **/
FuFirmware *
fu_firmware_get_image_by_idx(FuFirmware *self, guint64 idx, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	for (guint i = 0; i < priv->images->len; i++) {
		FuFirmware *img = g_ptr_array_index(priv->images, i);
		if (fu_firmware_get_idx(img) == idx)
			return g_object_ref(img);
	}
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_FOUND,
		    "no image idx %" G_GUINT64_FORMAT " found in firmware",
		    idx);
	return NULL;
}

/**
 * fu_firmware_get_image_by_checksum:
 * @self: a #FuPlugin
 * @checksum: checksum string of any format
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image using the image checksum. The checksum type is guessed
 * based on the length of the input string.
 *
 * Returns: (transfer full): a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 1.5.5
 **/
FuFirmware *
fu_firmware_get_image_by_checksum(FuFirmware *self, const gchar *checksum, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	GChecksumType csum_kind;

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	g_return_val_if_fail(checksum != NULL, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	csum_kind = fwupd_checksum_guess_kind(checksum);
	for (guint i = 0; i < priv->images->len; i++) {
		FuFirmware *img = g_ptr_array_index(priv->images, i);
		g_autofree gchar *checksum_tmp = NULL;

		/* if this expensive then the subclassed FuFirmware can
		 * cache the result as required */
		checksum_tmp = fu_firmware_get_checksum(img, csum_kind, error);
		if (checksum_tmp == NULL)
			return NULL;
		if (g_strcmp0(checksum_tmp, checksum) == 0)
			return g_object_ref(img);
	}
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_FOUND,
		    "no image with checksum %s found in firmware",
		    checksum);
	return NULL;
}

/**
 * fu_firmware_get_image_by_idx_bytes:
 * @self: a #FuPlugin
 * @idx: image index
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image bytes using the image index.
 *
 * Returns: (transfer full): a #GBytes of a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 1.3.1
 **/
GBytes *
fu_firmware_get_image_by_idx_bytes(FuFirmware *self, guint64 idx, GError **error)
{
	g_autoptr(FuFirmware) img = fu_firmware_get_image_by_idx(self, idx, error);
	if (img == NULL)
		return NULL;
	return fu_firmware_write(img, error);
}

/**
 * fu_firmware_get_image_by_idx_stream:
 * @self: a #FuPlugin
 * @idx: image index
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image stream using the image index.
 *
 * Returns: (transfer full): a #GInputStream of a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 2.0.0
 **/
GInputStream *
fu_firmware_get_image_by_idx_stream(FuFirmware *self, guint64 idx, GError **error)
{
	g_autoptr(FuFirmware) img = fu_firmware_get_image_by_idx(self, idx, error);
	if (img == NULL)
		return NULL;
	return fu_firmware_get_stream(img, error);
}

/**
 * fu_firmware_get_image_by_gtype_bytes:
 * @self: a #FuPlugin
 * @gtype: an image #GType
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image bytes using the image #GType.
 *
 * Returns: (transfer full): a #GBytes of a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 1.9.3
 **/
GBytes *
fu_firmware_get_image_by_gtype_bytes(FuFirmware *self, GType gtype, GError **error)
{
	g_autoptr(FuFirmware) img = fu_firmware_get_image_by_gtype(self, gtype, error);
	if (img == NULL)
		return NULL;
	return fu_firmware_write(img, error);
}

/**
 * fu_firmware_get_image_by_gtype:
 * @self: a #FuPlugin
 * @gtype: an image #GType
 * @error: (nullable): optional return location for an error
 *
 * Gets the firmware image using the image #GType.
 *
 * Returns: (transfer full): a #FuFirmware, or %NULL if the image is not found
 *
 * Since: 1.9.3
 **/
FuFirmware *
fu_firmware_get_image_by_gtype(FuFirmware *self, GType gtype, GError **error)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_FIRMWARE(self), NULL);
	g_return_val_if_fail(gtype != G_TYPE_INVALID, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	for (guint i = 0; i < priv->images->len; i++) {
		FuFirmware *img = g_ptr_array_index(priv->images, i);
		if (g_type_is_a(G_OBJECT_TYPE(img), gtype))
			return g_object_ref(img);
	}
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_FOUND,
		    "no image GType %s found in firmware",
		    g_type_name(gtype));
	return NULL;
}

/**
 * fu_firmware_export:
 * @self: a #FuFirmware
 * @flags: firmware export flags, e.g. %FU_FIRMWARE_EXPORT_FLAG_INCLUDE_DEBUG
 * @bn: a Xmlb builder node
 *
 * This allows us to build an XML object for the nested firmware.
 *
 * Since: 1.6.0
 **/
void
fu_firmware_export(FuFirmware *self, FuFirmwareExportFlags flags, XbBuilderNode *bn)
{
	FuFirmwareClass *klass = FU_FIRMWARE_GET_CLASS(self);
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	const gchar *gtypestr = G_OBJECT_TYPE_NAME(self);

	/* object */
	if (g_strcmp0(gtypestr, "FuFirmware") != 0)
		xb_builder_node_set_attr(bn, "gtype", gtypestr);

	/* subclassed type */
	if (priv->flags != FU_FIRMWARE_FLAG_NONE) {
		g_autofree gchar *str = fu_firmware_flags_to_string(priv->flags);
		fu_xmlb_builder_insert_kv(bn, "flags", str);
	}
	fu_xmlb_builder_insert_kv(bn, "id", priv->id);
	fu_xmlb_builder_insert_kx(bn, "idx", priv->idx);
	fu_xmlb_builder_insert_kv(bn, "version", priv->version);
	fu_xmlb_builder_insert_kx(bn, "version_raw", priv->version_raw);
	if (priv->version_format != FWUPD_VERSION_FORMAT_UNKNOWN) {
		fu_xmlb_builder_insert_kv(bn,
					  "version_format",
					  fwupd_version_format_to_string(priv->version_format));
	}
	fu_xmlb_builder_insert_kx(bn, "addr", priv->addr);
	fu_xmlb_builder_insert_kx(bn, "offset", priv->offset);
	fu_xmlb_builder_insert_kx(bn, "alignment", priv->alignment);
	fu_xmlb_builder_insert_kx(bn, "size", priv->size);
	fu_xmlb_builder_insert_kx(bn, "size_max", priv->size_max);
	fu_xmlb_builder_insert_kv(bn, "filename", priv->filename);
	if (priv->stream != NULL) {
		g_autofree gchar *dataszstr = g_strdup_printf("0x%x", (guint)priv->streamsz);
		g_autofree gchar *datastr = NULL;
		if (priv->streamsz <= 0x100) {
			g_autoptr(GByteArray) buf = fu_input_stream_read_byte_array(priv->stream,
										    0x0,
										    priv->streamsz,
										    NULL,
										    NULL);
			if (buf != NULL) {
				if (flags & FU_FIRMWARE_EXPORT_FLAG_ASCII_DATA) {
					datastr = fu_memstrsafe(buf->data,
								buf->len,
								0x0,
								MIN(buf->len, 0x100),
								NULL);
				} else {
					datastr = g_base64_encode(buf->data, buf->len);
				}
			}
		}
		xb_builder_node_insert_text(bn,
					    "data",
					    datastr,
					    "type",
					    "GInputStream",
					    "size",
					    dataszstr,
					    NULL);
	} else if (priv->bytes != NULL && g_bytes_get_size(priv->bytes) == 0) {
		xb_builder_node_insert_text(bn, "data", NULL, "type", "GBytes", NULL);
	} else if (priv->bytes != NULL) {
		gsize bufsz = 0;
		const guint8 *buf = g_bytes_get_data(priv->bytes, &bufsz);
		g_autofree gchar *datastr = NULL;
		g_autofree gchar *dataszstr = g_strdup_printf("0x%x", (guint)bufsz);
		if (flags & FU_FIRMWARE_EXPORT_FLAG_ASCII_DATA) {
			datastr = fu_memstrsafe(buf, bufsz, 0x0, MIN(bufsz, 0x100), NULL);
		} else {
			datastr = g_base64_encode(buf, bufsz);
		}
		xb_builder_node_insert_text(bn,
					    "data",
					    datastr,
					    "type",
					    "GBytes",
					    "size",
					    dataszstr,
					    NULL);
	}

	/* chunks */
	if (priv->chunks != NULL && priv->chunks->len > 0) {
		g_autoptr(XbBuilderNode) bp = xb_builder_node_insert(bn, "chunks", NULL);
		for (guint i = 0; i < priv->chunks->len; i++) {
			FuChunk *chk = g_ptr_array_index(priv->chunks, i);
			g_autoptr(XbBuilderNode) bc = xb_builder_node_insert(bp, "chunk", NULL);
			fu_chunk_export(chk, flags, bc);
		}
	}

	/* vfunc */
	if (klass->export != NULL)
		klass->export(self, flags, bn);

	/* children */
	if (priv->images->len > 0) {
		for (guint i = 0; i < priv->images->len; i++) {
			FuFirmware *img = g_ptr_array_index(priv->images, i);
			g_autoptr(XbBuilderNode) bc = xb_builder_node_insert(bn, "firmware", NULL);
			fu_firmware_export(img, flags, bc);
		}
	}
}

/**
 * fu_firmware_export_to_xml:
 * @self: a #FuFirmware
 * @flags: firmware export flags, e.g. %FU_FIRMWARE_EXPORT_FLAG_INCLUDE_DEBUG
 * @error: (nullable): optional return location for an error
 *
 * This allows us to build an XML object for the nested firmware.
 *
 * Returns: a string value, or %NULL for invalid.
 *
 * Since: 1.6.0
 **/
gchar *
fu_firmware_export_to_xml(FuFirmware *self, FuFirmwareExportFlags flags, GError **error)
{
	g_autoptr(XbBuilderNode) bn = xb_builder_node_new("firmware");
	fu_firmware_export(self, flags, bn);
	return xb_builder_node_export(bn,
				      XB_NODE_EXPORT_FLAG_FORMAT_MULTILINE |
					  XB_NODE_EXPORT_FLAG_COLLAPSE_EMPTY |
					  XB_NODE_EXPORT_FLAG_FORMAT_INDENT,
				      error);
}

/**
 * fu_firmware_to_string:
 * @self: a #FuFirmware
 *
 * This allows us to easily print the object.
 *
 * Returns: a string value, or %NULL for invalid.
 *
 * Since: 1.3.1
 **/
gchar *
fu_firmware_to_string(FuFirmware *self)
{
	g_autoptr(XbBuilderNode) bn = xb_builder_node_new("firmware");
	fu_firmware_export(self,
			   FU_FIRMWARE_EXPORT_FLAG_INCLUDE_DEBUG |
			       FU_FIRMWARE_EXPORT_FLAG_ASCII_DATA,
			   bn);
	return xb_builder_node_export(bn,
				      XB_NODE_EXPORT_FLAG_FORMAT_MULTILINE |
					  XB_NODE_EXPORT_FLAG_COLLAPSE_EMPTY |
					  XB_NODE_EXPORT_FLAG_FORMAT_INDENT,
				      NULL);
}

static void
fu_firmware_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	FuFirmware *self = FU_FIRMWARE(object);
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	switch (prop_id) {
	case PROP_PARENT:
		g_value_set_object(value, priv->parent);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
fu_firmware_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	FuFirmware *self = FU_FIRMWARE(object);
	switch (prop_id) {
	case PROP_PARENT:
		fu_firmware_set_parent(self, g_value_get_object(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
fu_firmware_init(FuFirmware *self)
{
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	priv->images = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
}

static void
fu_firmware_finalize(GObject *object)
{
	FuFirmware *self = FU_FIRMWARE(object);
	FuFirmwarePrivate *priv = GET_PRIVATE(self);
	g_free(priv->version);
	g_free(priv->id);
	g_free(priv->filename);
	if (priv->bytes != NULL)
		g_bytes_unref(priv->bytes);
	if (priv->stream != NULL)
		g_object_unref(priv->stream);
	if (priv->chunks != NULL)
		g_ptr_array_unref(priv->chunks);
	if (priv->patches != NULL)
		g_ptr_array_unref(priv->patches);
	if (priv->parent != NULL)
		g_object_remove_weak_pointer(G_OBJECT(priv->parent), (gpointer *)&priv->parent);
	g_ptr_array_unref(priv->images);
	G_OBJECT_CLASS(fu_firmware_parent_class)->finalize(object);
}

static void
fu_firmware_class_init(FuFirmwareClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GParamSpec *pspec;

	object_class->finalize = fu_firmware_finalize;
	object_class->get_property = fu_firmware_get_property;
	object_class->set_property = fu_firmware_set_property;

	/**
	 * FuFirmware:parent:
	 *
	 * The firmware parent.
	 *
	 * Since: 1.8.2
	 */
	pspec = g_param_spec_object("parent",
				    NULL,
				    NULL,
				    FU_TYPE_FIRMWARE,
				    G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_PARENT, pspec);
}

/**
 * fu_firmware_new:
 *
 * Creates an empty firmware object.
 *
 * Returns: a #FuFirmware
 *
 * Since: 1.3.1
 **/
FuFirmware *
fu_firmware_new(void)
{
	FuFirmware *self = g_object_new(FU_TYPE_FIRMWARE, NULL);
	return FU_FIRMWARE(self);
}

/**
 * fu_firmware_new_from_bytes:
 * @fw: firmware blob image
 *
 * Creates a firmware object with the provided image set as default.
 *
 * Returns: a #FuFirmware
 *
 * Since: 1.3.1
 **/
FuFirmware *
fu_firmware_new_from_bytes(GBytes *fw)
{
	FuFirmware *self = fu_firmware_new();
	fu_firmware_set_bytes(self, fw);
	return self;
}

/**
 * fu_firmware_new_from_gtypes:
 * @stream: a #GInputStream
 * @offset: start offset, useful for ignoring a bootloader
 * @flags: install flags, e.g. %FU_FIRMWARE_PARSE_FLAG_IGNORE_CHECKSUM
 * @error: (nullable): optional return location for an error
 * @...: an array of #GTypes, ending with %G_TYPE_INVALID
 *
 * Tries to parse the firmware with each #GType in order.
 *
 * Returns: (transfer full) (nullable): a #FuFirmware, or %NULL
 *
 * Since: 1.5.6
 **/
FuFirmware *
fu_firmware_new_from_gtypes(GInputStream *stream,
			    gsize offset,
			    FuFirmwareParseFlags flags,
			    GError **error,
			    ...)
{
	va_list args;
	g_autoptr(GArray) gtypes = g_array_new(FALSE, FALSE, sizeof(GType));
	g_autoptr(GError) error_all = NULL;

	g_return_val_if_fail(G_IS_INPUT_STREAM(stream), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* create array of GTypes */
	va_start(args, error);
	while (TRUE) {
		GType gtype = va_arg(args, GType);
		if (gtype == G_TYPE_INVALID)
			break;
		g_array_append_val(gtypes, gtype);
	}
	va_end(args);

	/* invalid */
	if (gtypes->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "no GTypes specified");
		return NULL;
	}

	/* try each GType in turn */
	for (guint i = 0; i < gtypes->len; i++) {
		GType gtype = g_array_index(gtypes, GType, i);
		g_autoptr(FuFirmware) firmware = g_object_new(gtype, NULL);
		g_autoptr(GError) error_local = NULL;
		if (!fu_firmware_parse_stream(firmware, stream, offset, flags, &error_local)) {
			g_debug("%s", error_local->message);
			if (error_all == NULL) {
				g_propagate_error(&error_all, g_steal_pointer(&error_local));
			} else {
				g_prefix_error(&error_all, "%s: ", error_local->message);
			}
			continue;
		}
		return g_steal_pointer(&firmware);
	}

	/* failed */
	g_propagate_error(error, g_steal_pointer(&error_all));
	return NULL;
}
