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

// ISO/IEC 21496-1:2025 — Gain map metadata for image conversion.
//
// This module implements the binary GainMapMetadata syntax shared by the
// 'hdrgm' item-property box and the tone-mapped ('tmap') derived image item,
// plus the public HDR composition entry points. The Box_hdrgm class itself
// lives in box.h/box.cc alongside the other box types; this header only holds
// the box-independent data model and (de)serialization helpers.

#ifndef LIBHEIF_GAINMAP_H
#define LIBHEIF_GAINMAP_H

#include "error.h"

#include <array>
#include <cstdint>
#include <vector>


// Number of colour channels a gain map and its metadata can describe.
static constexpr int GAIN_MAP_MAX_CHANNELS = 3;

// The only version of the ISO/IEC 21496-1 GainMapMetadata syntax this
// implementation reads and writes.
static constexpr uint16_t ISO21496_SUPPORTED_VERSION = 0;

// Auxiliary type URN carried by the 'auxC' property of a gain map item.
extern const char* const GAIN_MAP_AUX_TYPE_URN;


// A single signed (S32 numerator) or unsigned (U32 numerator) rational.
// The on-wire encoding always uses a 32-bit numerator and a 32-bit
// denominator; min/max/offset numerators may be negative.
struct GainMapRational
{
  int64_t num = 0;
  uint32_t den = 1;
};


// Per-channel parameters. When only one channel is stored on the wire, the
// single record is replicated to all three entries after parsing.
struct GainMapChannelParams
{
  GainMapRational gain_map_min;     // log2-domain minimum gain (signed)
  GainMapRational gain_map_max;     // log2-domain maximum gain (signed)
  GainMapRational gamma;            // encoding gamma of the stored map (unsigned, num > 0)
  GainMapRational base_offset;      // offset added on the SDR/base side (signed)
  GainMapRational alternate_offset; // offset added on the HDR/alternate side (signed)
};


// Parsed representation of the ISO/IEC 21496-1 GainMapMetadata payload.
struct GainMapMetadata
{
  uint16_t minimum_version = 0;
  uint16_t writer_version = 0;

  // flags bit 7: three channels are stored instead of one.
  bool multi_channel = false;
  // flags bit 6: apply the map in the base rendition's colour space.
  bool use_base_color_space = true;

  // log2 of the display headroom of the base / alternate rendition.
  GainMapRational base_hdr_headroom;
  GainMapRational alternate_hdr_headroom;

  // Always three logical entries; nb_channels distinguishes the wire count.
  int nb_channels = 1;
  std::array<GainMapChannelParams, GAIN_MAP_MAX_CHANNELS> channels{};
};


// Parse an ISO/IEC 21496-1 GainMapMetadata payload (the bytes inside an
// 'hdrgm' box, excluding the box header; or the bytes following the version
// byte of a 'tmap' item's data).
//
// Distinguishable failures:
//  - heif_suberror_Unsupported_data_version : minimum/writer version unknown
//  - heif_suberror_Invalid_gain_map_channel_count : stored channel count not 1 or 3
//  - heif_suberror_End_of_data / heif_suberror_Invalid_gain_map_metadata : malformed payload
Error parse_gain_map_metadata(const std::vector<uint8_t>& data, GainMapMetadata* out);

// Serialize the metadata to the ISO/IEC 21496-1 payload syntax (no box header).
// Returns the exact bytes that Box_hdrgm wraps and that a 'tmap' item stores
// after its leading version byte.
Error write_gain_map_metadata(const GainMapMetadata& metadata, std::vector<uint8_t>* out);

// Cross-check that the metadata's stored channel count matches the number of
// components of the gain map image item (1 for a monochrome map, 3 for an RGB
// map). Returns heif_suberror_Invalid_gain_map_channel_count on mismatch.
Error check_gain_map_channel_count_matches(const GainMapMetadata& metadata,
                                           int gain_map_component_count);


#endif // LIBHEIF_GAINMAP_H
