/*
 * HEIF codec.
 * Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBHEIF_HEIF_GAIN_MAP_H
#define LIBHEIF_HEIF_GAIN_MAP_H

#include <libheif/heif_version.h>
#include <libheif/heif_error.h>
#include <libheif/heif_image.h>

#ifdef __cplusplus
extern "C" {
#endif

// ISO/IEC 21496-1:2025 gain map (Ultra HDR) support.
//
// A gain map image is an auxiliary image item bound to the primary (SDR/base)
// image through a 'tmap' tone-mapped derived item. Applying the map to the base
// rendition reconstructs the HDR/alternate rendition.

struct heif_image_handle;
struct heif_decoding_options;


// Per-channel gain map parameters, exposed as plain C rationals. The values
// mirror the ISO/IEC 21496-1 GainMapMetadata syntax. There are always three
// logical channel entries; when the file stores a single channel all three
// entries are identical.
struct heif_gain_map_channel
{
  // log2-domain minimum/maximum gain (may be negative).
  struct
  {
    int32_t numerator;
    uint32_t denominator;
  } gain_map_min;
  struct
  {
    int32_t numerator;
    uint32_t denominator;
  } gain_map_max;

  // Encoding gamma of the stored map (positive, unsigned).
  struct
  {
    uint32_t numerator;
    uint32_t denominator;
  } gamma;

  // Small offsets added before the log-domain math (may be negative).
  struct
  {
    int32_t numerator;
    uint32_t denominator;
  } base_offset;
  struct
  {
    int32_t numerator;
    uint32_t denominator;
  } alternate_offset;
};

struct heif_gain_map_metadata
{
  uint8_t version;

  // 1 (all channels identical) or 3 (per-channel).
  int nb_channels;

  // Non-zero when the map is applied in the base colour space.
  int use_base_color_space;

  // log2 of the display headroom of the base / alternate rendition.
  struct
  {
    uint32_t numerator;
    uint32_t denominator;
  } base_hdr_headroom;
  struct
  {
    uint32_t numerator;
    uint32_t denominator;
  } alternate_hdr_headroom;

  struct heif_gain_map_channel channels[3];
};


// Returns non-zero when the image handle has an ISO/IEC 21496-1 gain map.
LIBHEIF_API int heif_image_handle_has_gain_map(const struct heif_image_handle* handle);

// Fill 'out' with the parsed gain map metadata.
//
// Distinguishable failures:
//  - heif_suberror_Gain_map_missing            : no gain map is present
//  - heif_suberror_Unsupported_data_version    : unknown metadata version
//  - heif_suberror_Invalid_gain_map_channel_count : channel count not 1/3
//  - heif_suberror_Invalid_gain_map_metadata   : malformed metadata
LIBHEIF_API struct heif_error heif_image_handle_get_gain_map_metadata(
    const struct heif_image_handle* handle,
    struct heif_gain_map_metadata* out);

// Decode the gain map image. The returned image is a single-channel (R) or
// three-channel (R,G,B) HeifPixelImage at the gain map's own resolution.
// Returns heif_suberror_Gain_map_missing when no gain map item exists.
LIBHEIF_API struct heif_error heif_image_handle_decode_gain_map(
    const struct heif_image_handle* handle,
    const struct heif_decoding_options* options,
    struct heif_image** out_img);

// Compose the HDR/alternate rendition by decoding the base image and applying
// its gain map. The output is an RGB image in the alternate rendition's colour
// space with the same dimensions as the base image.
//
// When the handle has no gain map this behaves like a normal base-image decode
// so callers can use it unconditionally.
LIBHEIF_API struct heif_error heif_image_handle_decode_hdr_image(
    const struct heif_image_handle* handle,
    heif_colorspace colorspace,
    heif_chroma chroma,
    struct heif_image** out_img,
    const struct heif_decoding_options* options);

#ifdef __cplusplus
}
#endif

#endif // LIBHEIF_HEIF_GAIN_MAP_H
