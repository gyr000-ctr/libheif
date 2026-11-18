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

#include "tmap.h"
#include "context.h"
#include "file.h"
#include "box.h"


ImageItem_tmap::ImageItem_tmap(HeifContext* ctx)
    : ImageItem(ctx)
{
}


ImageItem_tmap::ImageItem_tmap(HeifContext* ctx, heif_item_id id)
    : ImageItem(ctx, id)
{
}


Error ImageItem_tmap::get_reference_ids(heif_item_id* base_id, heif_item_id* gain_map_id) const
{
  auto iref_box = get_file()->get_iref_box();
  if (!iref_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iref_box,
                 "No iref box available, but needed for tmap image");
  }

  std::vector<heif_item_id> refs = iref_box->get_references(get_id(), fourcc("dimg"));
  if (refs.size() != 2) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Gain_map_missing,
                 "'tmap' image must reference exactly two images (base and gain map)");
  }

  for (heif_item_id ref : refs) {
    if (!get_file()->has_item_with_id(ref)) {
      return Error(heif_error_Invalid_input,
                   heif_suberror_Nonexisting_item_referenced,
                   "'tmap' image references a non-existing item");
    }
  }

  if (base_id) {
    *base_id = refs[0];
  }
  if (gain_map_id) {
    *gain_map_id = refs[1];
  }
  return Error::Ok;
}


Result<GainMapMetadata> ImageItem_tmap::get_gain_map_metadata() const
{
  // 1. An 'hdrgm' property attached directly to this tmap item.
  if (auto hdrgm = get_property<Box_hdrgm>()) {
    return hdrgm->get_metadata();
  }

  // 2. An 'hdrgm' property attached to the referenced gain map image item.
  //    This is where the 'mini' expansion (and common native files) place it.
  heif_item_id gain_map_id = 0;
  if (!get_reference_ids(nullptr, &gain_map_id)) {
    std::vector<std::shared_ptr<Box>> gain_props;
    get_file()->get_properties(gain_map_id, gain_props);
    for (const auto& prop : gain_props) {
      if (auto hdrgm = std::dynamic_pointer_cast<Box_hdrgm>(prop)) {
        return hdrgm->get_metadata();
      }
    }
  }

  // 3. Otherwise the metadata is this item's data: one version byte followed
  //    by the ISO/IEC 21496-1 payload.
  auto dataResult = get_file()->get_uncompressed_item_data(get_id());
  if (dataResult.is_error() || dataResult->empty()) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Gain_map_missing,
                 "No 'hdrgm' property or 'tmap' item data carrying gain map metadata");
  }
  const std::vector<uint8_t>& data = *dataResult;
  if (data.size() < 2 || data[0] != ISO21496_SUPPORTED_VERSION) {
    return Error(heif_error_Unsupported_feature,
                 heif_suberror_Unsupported_data_version,
                 "Unsupported version byte in 'tmap' item data");
  }

  std::vector<uint8_t> payload(data.begin() + 1, data.end());
  GainMapMetadata metadata;
  if (Error err = parse_gain_map_metadata(payload, &metadata)) {
    return err;
  }
  return metadata;
}


Result<std::shared_ptr<HeifPixelImage>>
ImageItem_tmap::decode_gain_map_image(const heif_decoding_options& options) const
{
  heif_item_id gain_map_id = 0;
  if (Error err = get_reference_ids(nullptr, &gain_map_id)) {
    return err;
  }

  auto gain_item = get_context()->get_image(gain_map_id, true);
  if (!gain_item) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Gain_map_missing,
                 "'tmap' gain map image item is not available");
  }
  if (auto err = gain_item->get_item_error()) {
    return err;
  }

  DecodeTraversalState state;
  auto imgResult = gain_item->decode_image(options, false, 0, 0, state);
  if (imgResult.is_error()) {
    return imgResult.error();
  }

  // Verify the gain map image component count agrees with the metadata.
  auto metaResult = get_gain_map_metadata();
  if (metaResult.is_error()) {
    return metaResult.error();
  }
  const int component_count = (*imgResult)->has_channel(heif_channel_G) ? 3 : 1;
  if (Error err = check_gain_map_channel_count_matches(*metaResult, component_count)) {
    return err;
  }

  return imgResult;
}


Result<std::shared_ptr<HeifPixelImage>> ImageItem_tmap::decode_compressed_image(
    const heif_decoding_options& options,
    bool decode_tile_only, uint32_t tile_x0, uint32_t tile_y0,
    DecodeTraversalState decode_state) const
{
  if (decode_state.processed_ids.contains(get_id())) {
    return Error{heif_error_Invalid_input,
                 heif_suberror_Unspecified,
                 "'iref' has cyclic references"};
  }
  decode_state.processed_ids.insert(get_id());

  heif_item_id base_id = 0;
  if (Error err = get_reference_ids(&base_id, nullptr)) {
    return err;
  }

  auto base_item = get_context()->get_image(base_id, true);
  if (!base_item) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Nonexisting_item_referenced,
                 "'tmap' base image references an unavailable image");
  }
  if (auto err = base_item->get_item_error()) {
    return err;
  }

  // Default decode yields the unchanged SDR/base rendition.
  return base_item->decode_image(options, decode_tile_only, tile_x0, tile_y0, decode_state);
}


Error ImageItem_tmap::get_coded_image_colorspace(heif_colorspace* out_colorspace, heif_chroma* out_chroma) const
{
  heif_item_id base_id = 0;
  if (Error err = get_reference_ids(&base_id, nullptr)) {
    return err;
  }

  auto image = get_context()->get_image(base_id, true);
  if (!image) {
    return Error{heif_error_Invalid_input,
                 heif_suberror_Nonexisting_item_referenced};
  }
  return image->get_coded_image_colorspace(out_colorspace, out_chroma);
}


void ImageItem_tmap::populate_component_descriptions()
{
  if (!get_component_descriptions().empty()) {
    return;
  }

  heif_item_id base_id = 0;
  if (get_reference_ids(&base_id, nullptr)) {
    return;
  }

  auto child = get_context()->get_image(base_id, true);
  if (!child) {
    return;
  }

  populate_descriptions_from_child(*child, child->get_width(), child->get_height());
}


int ImageItem_tmap::get_luma_bits_per_pixel_unlocked() const
{
  heif_item_id base_id = 0;
  if (get_reference_ids(&base_id, nullptr)) {
    return -1;
  }
  auto image = get_context()->get_image(base_id, true);
  return image ? image->get_luma_bits_per_pixel() : -1;
}


int ImageItem_tmap::get_chroma_bits_per_pixel_unlocked() const
{
  heif_item_id base_id = 0;
  if (get_reference_ids(&base_id, nullptr)) {
    return -1;
  }
  auto image = get_context()->get_image(base_id, true);
  return image ? image->get_chroma_bits_per_pixel() : -1;
}


heif_brand2 ImageItem_tmap::get_compatible_brand() const
{
  assert(false);
  return 0; // TODO (we never write 'tmap' images through the generic encoder)
}
