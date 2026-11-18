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

// ISO/IEC 21496-1:2025 gain map metadata payload (de)serialization.
// The Box_hdrgm wrapper is implemented in box.cc.

#include "gainmap.h"
#include "bitstream.h"

const char* const GAIN_MAP_AUX_TYPE_URN = "urn:iso:std:iso:ts:21496:-1";

// flags byte layout (ISO/IEC 21496-1, GainMapMetadata payload).
static constexpr uint8_t ISO21496_FLAG_MULTICHANNEL = 0x80; // bit 7
static constexpr uint8_t ISO21496_FLAG_USE_BASE_CG = 0x40; // bit 6
static constexpr uint8_t ISO21496_FLAG_RESERVED = 0x3f;     // bits 5..0

// Fixed payload size: 5 header bytes + 2 headrooms (8 each) + per-channel
// block of five rationals (8 each).
static size_t expected_payload_size(int nb_channels)
{
  return 5 + 2 * 8 + static_cast<size_t>(nb_channels) * 5 * 8;
}


static GainMapRational read_signed_rational(BitstreamRange& range)
{
  GainMapRational r;
  r.num = range.read32s();
  r.den = range.read32();
  return r;
}


static GainMapRational read_unsigned_rational(BitstreamRange& range)
{
  GainMapRational r;
  r.num = range.read32();
  r.den = range.read32();
  return r;
}


static void write_rational(StreamWriter& writer, const GainMapRational& r)
{
  writer.write32s(static_cast<int32_t>(r.num));
  writer.write32(r.den);
}


// Validate the cross-field constraints of a fully populated metadata block.
static Error validate_metadata(const GainMapMetadata& m)
{
  if (m.nb_channels != 1 && m.nb_channels != GAIN_MAP_MAX_CHANNELS) {
    return {heif_error_Invalid_input,
            heif_suberror_Invalid_gain_map_channel_count,
            "Gain map metadata channel count must be 1 or 3"};
  }

  if (m.base_hdr_headroom.den == 0 || m.alternate_hdr_headroom.den == 0) {
    return {heif_error_Invalid_input,
            heif_suberror_Invalid_gain_map_metadata,
            "Gain map headroom denominator cannot be zero"};
  }

  for (int c = 0; c < m.nb_channels; c++) {
    const auto& ch = m.channels[c];
    if (ch.gain_map_min.den == 0 || ch.gain_map_max.den == 0 ||
        ch.gamma.den == 0 || ch.base_offset.den == 0 ||
        ch.alternate_offset.den == 0) {
      return {heif_error_Invalid_input,
              heif_suberror_Invalid_gain_map_metadata,
              "Gain map metadata contains a zero denominator"};
    }
    if (ch.gamma.num <= 0) {
      return {heif_error_Invalid_input,
              heif_suberror_Invalid_gain_map_metadata,
              "Gain map gamma must be positive"};
    }
  }

  return Error::Ok;
}


Error parse_gain_map_metadata(const std::vector<uint8_t>& data, GainMapMetadata* out)
{
  if (!out) {
    return {heif_error_Usage_error, heif_suberror_Null_pointer_argument,
            "null GainMapMetadata output pointer"};
  }

  // The smallest possible payload describes a single channel.
  static constexpr size_t MIN_PAYLOAD_SIZE = 5 + 2 * 8 + 1 * 5 * 8; // 61
  if (data.size() < MIN_PAYLOAD_SIZE) {
    return {heif_error_Invalid_input,
            heif_suberror_End_of_data,
            "Truncated ISO/IEC 21496-1 gain map metadata"};
  }

  auto reader = std::make_shared<StreamReader_memory>(data.data(), data.size(), false);
  BitstreamRange range(reader, data.size());

  GainMapMetadata m;
  m.minimum_version = range.read16();
  m.writer_version = range.read16();

  if (m.minimum_version > ISO21496_SUPPORTED_VERSION ||
      m.writer_version < m.minimum_version) {
    return {heif_error_Unsupported_feature,
            heif_suberror_Unsupported_data_version,
            "Unsupported ISO/IEC 21496-1 gain map metadata version"};
  }

  uint8_t flags = range.read8();
  m.multi_channel = (flags & ISO21496_FLAG_MULTICHANNEL) != 0;
  m.use_base_color_space = (flags & ISO21496_FLAG_USE_BASE_CG) != 0;
  m.nb_channels = m.multi_channel ? 3 : 1;

  // A version-0 writer must leave the reserved flag bits clear.
  if (m.writer_version == 0 && (flags & ISO21496_FLAG_RESERVED) != 0) {
    return {heif_error_Invalid_input,
            heif_suberror_Invalid_gain_map_metadata,
            "Gain map metadata flags contain reserved bits"};
  }

  if (range.get_remaining_bytes() < expected_payload_size(m.nb_channels) - 5) {
    return {heif_error_Invalid_input,
            heif_suberror_End_of_data,
            "Truncated ISO/IEC 21496-1 gain map metadata"};
  }

  m.base_hdr_headroom = read_unsigned_rational(range);
  m.alternate_hdr_headroom = read_unsigned_rational(range);

  for (int c = 0; c < m.nb_channels; c++) {
    auto& ch = m.channels[c];
    ch.gain_map_min = read_signed_rational(range);
    ch.gain_map_max = read_signed_rational(range);
    ch.gamma = read_unsigned_rational(range);
    ch.base_offset = read_signed_rational(range);
    ch.alternate_offset = read_signed_rational(range);
  }

  if (Error err = range.get_error()) {
    return err;
  }

  // A single stored channel applies identically to R, G and B.
  for (int c = m.nb_channels; c < GAIN_MAP_MAX_CHANNELS; c++) {
    m.channels[c] = m.channels[0];
  }

  if (Error err = validate_metadata(m)) {
    return err;
  }

  *out = std::move(m);
  return Error::Ok;
}


Error write_gain_map_metadata(const GainMapMetadata& metadata, std::vector<uint8_t>* out)
{
  if (!out) {
    return {heif_error_Usage_error, heif_suberror_Null_pointer_argument,
            "null output vector"};
  }

  if (Error err = validate_metadata(metadata)) {
    return err;
  }

  StreamWriter writer;
  writer.write16(ISO21496_SUPPORTED_VERSION); // minimum_version
  writer.write16(ISO21496_SUPPORTED_VERSION); // writer_version

  uint8_t flags = 0;
  if (metadata.nb_channels == GAIN_MAP_MAX_CHANNELS) {
    flags |= ISO21496_FLAG_MULTICHANNEL;
  }
  if (metadata.use_base_color_space) {
    flags |= ISO21496_FLAG_USE_BASE_CG;
  }
  writer.write8(flags);

  write_rational(writer, metadata.base_hdr_headroom);
  write_rational(writer, metadata.alternate_hdr_headroom);

  for (int c = 0; c < metadata.nb_channels; c++) {
    const auto& ch = metadata.channels[c];
    write_rational(writer, ch.gain_map_min);
    write_rational(writer, ch.gain_map_max);
    write_rational(writer, ch.gamma);
    write_rational(writer, ch.base_offset);
    write_rational(writer, ch.alternate_offset);
  }

  *out = writer.get_data();
  return Error::Ok;
}


Error check_gain_map_channel_count_matches(const GainMapMetadata& metadata,
                                           int gain_map_component_count)
{
  // The wire syntax can only ever express 1 or 3 stored channels, but the
  // gain map image item may advertise a different component count via its
  // 'pixi' property, which would make the metadata inapplicable.
  if (gain_map_component_count != 1 && gain_map_component_count != GAIN_MAP_MAX_CHANNELS) {
    return {heif_error_Invalid_input,
            heif_suberror_Invalid_gain_map_channel_count,
            "Gain map image component count must be 1 or 3"};
  }
  if (metadata.nb_channels != gain_map_component_count) {
    return {heif_error_Invalid_input,
            heif_suberror_Invalid_gain_map_channel_count,
            "Gain map metadata channel count does not match the gain map image components"};
  }
  return Error::Ok;
}
