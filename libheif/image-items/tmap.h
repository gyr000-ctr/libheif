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

#ifndef LIBHEIF_IMAGEITEM_TMAP_H
#define LIBHEIF_IMAGEITEM_TMAP_H

#include "image_item.h"
#include "gainmap.h"

#include <array>


// ISO/IEC 21496-1 tone-mapped image. A derived item of type 'tmap' that binds
// an SDR/base image item and a gain-map image item through a 'dimg' item
// reference with exactly two entries, and carries the GainMapMetadata either
// in its own item data (a leading version byte followed by the 21496-1
// payload) or in an associated 'hdrgm' item property.
class ImageItem_tmap : public ImageItem
{
public:
  ImageItem_tmap(HeifContext* ctx, heif_item_id id);

  ImageItem_tmap(HeifContext* ctx);

  uint32_t get_infe_type() const override { return fourcc("tmap"); }

  Result<Encoder::CodedImageData> encode(const std::shared_ptr<HeifPixelImage>& image,
                                         heif_encoder* encoder,
                                         const heif_encoding_options& options,
                                         heif_image_input_class input_class) override
  {
    return Error{heif_error_Unsupported_feature,
                 heif_suberror_Unspecified, "Cannot encode image to 'tmap' directly"};
  }

  // A tmap item decodes to its SDR/base rendition so that ordinary decode
  // paths keep working unchanged.
  Result<std::shared_ptr<HeifPixelImage>> decode_compressed_image(const heif_decoding_options& options,
                                                                  bool decode_tile_only, uint32_t tile_x0, uint32_t tile_y0,
                                                                  DecodeTraversalState decode_state) const override;

  Error get_coded_image_colorspace(heif_colorspace* out_colorspace, heif_chroma* out_chroma) const override;

  heif_brand2 get_compatible_brand() const override;

  void populate_component_descriptions() override;

  // --- ISO/IEC 21496-1 specific ---

  // Resolve and validate the two 'dimg' references.
  // On success fills base_id (the SDR rendition) and gain_map_id.
  Error get_reference_ids(heif_item_id* base_id, heif_item_id* gain_map_id) const;

  // Parse and return the gain map metadata, preferring the 'hdrgm' item
  // property and falling back to this item's data. Returns a distinguishable
  // error when the metadata is missing or invalid.
  Result<GainMapMetadata> get_gain_map_metadata() const;

  // The decoded gain map image (single-channel or RGB), or an error if the
  // referenced gain map item cannot be decoded.
  Result<std::shared_ptr<HeifPixelImage>> decode_gain_map_image(const heif_decoding_options& options) const;

private:
  int get_luma_bits_per_pixel_unlocked() const override;

  int get_chroma_bits_per_pixel_unlocked() const override;
};


#endif //LIBHEIF_IMAGEITEM_TMAP_H
