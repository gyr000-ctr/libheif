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

#include "libheif/heif_gain_map.h"
#include "libheif/heif_decoding.h"
#include "api_structs.h"
#include "image-items/tmap.h"
#include "image/pixelimage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>


static std::shared_ptr<ImageItem_tmap> get_tmap_item(const heif_image_handle* handle)
{
  if (!handle || !handle->image) {
    return nullptr;
  }
  // If the handle already is the tone-mapped item, use it directly.
  if (auto tmap = std::dynamic_pointer_cast<ImageItem_tmap>(handle->image)) {
    return tmap;
  }
  // Otherwise the handle is the SDR/base image; look up its associated tmap.
  return handle->context->get_tmap_image_for_base(handle->image->get_id());
}


int heif_image_handle_has_gain_map(const heif_image_handle* handle)
{
  auto tmap = get_tmap_item(handle);
  if (!tmap) {
    return 0;
  }
  heif_item_id gain_map_id = 0;
  if (tmap->get_reference_ids(nullptr, &gain_map_id)) {
    return 0;
  }
  return 1;
}


static void copy_rational(int32_t num, uint32_t den,
                          int32_t* out_num, uint32_t* out_den)
{
  *out_num = num;
  *out_den = den;
}

static void copy_urational(uint32_t num, uint32_t den,
                           uint32_t* out_num, uint32_t* out_den)
{
  *out_num = num;
  *out_den = den;
}


heif_error heif_image_handle_get_gain_map_metadata(const heif_image_handle* handle,
                                                   heif_gain_map_metadata* out)
{
  if (!out) {
    return heif_error_null_pointer_argument;
  }
  std::memset(out, 0, sizeof(*out));

  return exception_guard([&]() -> heif_error {
    auto tmap = get_tmap_item(handle);
    if (!tmap) {
      return Error(heif_error_Usage_error,
                   heif_suberror_Gain_map_missing,
                   "Image handle is not an ISO/IEC 21496-1 tone-mapped image")
          .error_struct(handle ? handle->image.get() : nullptr);
    }

    auto result = tmap->get_gain_map_metadata();
    if (result.is_error()) {
      return result.error().error_struct(tmap.get());
    }
    const GainMapMetadata& m = *result;

    out->version = m.minimum_version;
    out->nb_channels = m.nb_channels;
    out->use_base_color_space = m.use_base_color_space ? 1 : 0;
    copy_urational((uint32_t) m.base_hdr_headroom.num, m.base_hdr_headroom.den,
                   &out->base_hdr_headroom.numerator, &out->base_hdr_headroom.denominator);
    copy_urational((uint32_t) m.alternate_hdr_headroom.num, m.alternate_hdr_headroom.den,
                   &out->alternate_hdr_headroom.numerator, &out->alternate_hdr_headroom.denominator);

    for (int c = 0; c < 3; c++) {
      const auto& src = m.channels[c];
      auto& dst = out->channels[c];
      copy_rational((int32_t) src.gain_map_min.num, src.gain_map_min.den,
                    &dst.gain_map_min.numerator, &dst.gain_map_min.denominator);
      copy_rational((int32_t) src.gain_map_max.num, src.gain_map_max.den,
                    &dst.gain_map_max.numerator, &dst.gain_map_max.denominator);
      copy_urational((uint32_t) src.gamma.num, src.gamma.den,
                     &dst.gamma.numerator, &dst.gamma.denominator);
      copy_rational((int32_t) src.base_offset.num, src.base_offset.den,
                    &dst.base_offset.numerator, &dst.base_offset.denominator);
      copy_rational((int32_t) src.alternate_offset.num, src.alternate_offset.den,
                    &dst.alternate_offset.numerator, &dst.alternate_offset.denominator);
    }

    return heif_error_success;
  });
}


heif_error heif_image_handle_decode_gain_map(const heif_image_handle* handle,
                                             const heif_decoding_options* options,
                                             heif_image** out_img)
{
  if (!out_img) {
    return heif_error_null_pointer_argument;
  }
  *out_img = nullptr;

  return exception_guard([&]() -> heif_error {
    auto tmap = get_tmap_item(handle);
    if (!tmap) {
      return Error(heif_error_Usage_error,
                   heif_suberror_Gain_map_missing,
                   "Image handle has no ISO/IEC 21496-1 gain map")
          .error_struct(handle ? handle->image.get() : nullptr);
    }

    heif_decoding_options* dec_options = heif_decoding_options_alloc();
    heif_decoding_options_copy(dec_options, options);

    auto result = tmap->decode_gain_map_image(*dec_options);
    heif_decoding_options_free(dec_options);
    if (result.is_error()) {
      return result.error().error_struct(tmap.get());
    }

    *out_img = new heif_image();
    (*out_img)->image = *result;
    return heif_error_success;
  });
}


// --- HDR composition -------------------------------------------------------

static double rational_to_double(const GainMapRational& r)
{
  if (r.den == 0) {
    return 0.0;
  }
  return static_cast<double>(r.num) / static_cast<double>(r.den);
}


// Apply the gain map to one RGB pixel. `map_v` is the normalised [0,1] gain
// map value for channel c; the formula follows the Ultra HDR / ISO 21496-1
// reconstruction: interpolate in log2 space with the encoding gamma, then
// exponentiate.
static double apply_gain(double sdr_linear,
                         double map_v,
                         const GainMapChannelParams& ch,
                         double weight_factor,
                         double base_offset,
                         double alternate_offset)
{
  double gamma = rational_to_double(ch.gamma);
  if (gamma <= 0.0) {
    gamma = 1.0;
  }
  double log_recovery = std::pow(map_v, 1.0 / gamma);

  double gain_min = rational_to_double(ch.gain_map_min);
  double gain_max = rational_to_double(ch.gain_map_max);
  double log_boost = gain_min * (1.0 - log_recovery) + gain_max * log_recovery;

  double hdr = (sdr_linear + base_offset) *
               std::exp2(log_boost * weight_factor) - alternate_offset;
  return hdr;
}


// Compose an integer RGB base image with its gain map. Input planes may be
// 8-bit or 16-bit independently; the composed HDR output is always 16-bit RGB.
static Error compose_hdr_image(const std::shared_ptr<HeifPixelImage>& base,
                               const std::shared_ptr<HeifPixelImage>& gain,
                               const GainMapMetadata& meta,
                               std::shared_ptr<HeifPixelImage>* out)
{
  const uint32_t W = base->get_width();
  const uint32_t H = base->get_height();
  const int base_bits = base->get_bits_per_pixel(heif_channel_R);
  if (base_bits <= 0) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Unspecified,
                 "Base image has no usable R plane for HDR composition");
  }

  const int gain_bits = gain->get_bits_per_pixel(heif_channel_R);
  if (gain_bits <= 0) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Gain_map_missing,
                 "Gain map image has no usable plane");
  }

  auto hdr = std::make_shared<HeifPixelImage>();
  constexpr int out_bits = 16;
  hdr->create(W, H, heif_colorspace_RGB, heif_chroma_444);
  if (Error err = hdr->add_channel(heif_channel_R, W, H, out_bits, nullptr)) {
    return err;
  }
  if (Error err = hdr->add_channel(heif_channel_G, W, H, out_bits, nullptr)) {
    return err;
  }
  if (Error err = hdr->add_channel(heif_channel_B, W, H, out_bits, nullptr)) {
    return err;
  }

  // Generic integer plane reader that works for both 8-bit and 16-bit storage.
  auto make_reader = [](const HeifPixelImage& img, heif_channel ch, size_t* stride, int bits) {
    const uint8_t* p = img.get_channel_memory(ch, stride);
    return [p, bits](size_t idx) -> uint32_t {
      if (!p) {
        return 0;
      }
      return bits > 8 ? reinterpret_cast<const uint16_t*>(p)[idx]
                      : static_cast<uint32_t>(p[idx]);
    };
  };

  size_t stride_r = 0, stride_g = 0, stride_b = 0;
  auto read_base_r = make_reader(*base, heif_channel_R, &stride_r, base_bits);
  auto read_base_g = make_reader(*base, heif_channel_G, &stride_g, base_bits);
  auto read_base_b = make_reader(*base, heif_channel_B, &stride_b, base_bits);

  size_t gain_stride_r = 0, gain_stride_g = 0, gain_stride_b = 0;
  auto read_gain_r = make_reader(*gain, heif_channel_R, &gain_stride_r, gain_bits);
  const bool gain_is_monochrome = !gain->has_channel(heif_channel_G);
  std::function<uint32_t(size_t)> read_gain_g;
  std::function<uint32_t(size_t)> read_gain_b;
  if (!gain_is_monochrome) {
    read_gain_g = make_reader(*gain, heif_channel_G, &gain_stride_g, gain_bits);
    read_gain_b = make_reader(*gain, heif_channel_B, &gain_stride_b, gain_bits);
  }

  size_t out_stride = 0;
  uint16_t* out_r = reinterpret_cast<uint16_t*>(hdr->get_channel_memory(heif_channel_R, &out_stride));
  uint16_t* out_g = reinterpret_cast<uint16_t*>(hdr->get_channel_memory(heif_channel_G, &out_stride));
  uint16_t* out_b = reinterpret_cast<uint16_t*>(hdr->get_channel_memory(heif_channel_B, &out_stride));
  if (!out_r || !out_g || !out_b) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Nonexisting_image_channel_referenced,
                 "Missing output plane while composing HDR image");
  }

  const double base_max = static_cast<double>((1u << base_bits) - 1);
  const double gain_max_val = static_cast<double>((1u << gain_bits) - 1);
  constexpr double out_max = static_cast<double>((1u << out_bits) - 1);

  // This composition reconstructs the full alternate rendition, so the gain
  // is applied at weight 1.0 (the map's max-content-boost endpoint). A
  // display-specific boost would interpolate between the base and alternate
  // headrooms instead.
  constexpr double weight_factor = 1.0;

  const uint32_t gain_W = gain->get_width(heif_channel_R);
  const uint32_t gain_H = gain->get_height(heif_channel_R);
  const size_t base_elems_per_row_r = stride_r / (base_bits > 8 ? 2 : 1);
  const size_t gain_elems_per_row = gain_stride_r / (gain_bits > 8 ? 2 : 1);
  const size_t out_elems_per_row = out_stride / 2;

  auto sample_gain = [&](const std::function<uint32_t(size_t)>& reader,
                         uint32_t x, uint32_t y) -> double {
    uint32_t gx = gain_W ? (x * gain_W / W) : 0;
    uint32_t gy = gain_H ? (y * gain_H / H) : 0;
    size_t idx = (size_t) gy * gain_elems_per_row + gx;
    uint32_t v = reader(idx);
    return gain_max_val > 0.0 ? static_cast<double>(v) / gain_max_val : 0.0;
  };

  for (uint32_t y = 0; y < H; y++) {
    for (uint32_t x = 0; x < W; x++) {
      size_t base_idx_r = (size_t) y * base_elems_per_row_r + x;
      double sdr[3] = {
          read_base_r(base_idx_r) / base_max,
          read_base_g((size_t) y * (stride_g / (base_bits > 8 ? 2 : 1)) + x) / base_max,
          read_base_b((size_t) y * (stride_b / (base_bits > 8 ? 2 : 1)) + x) / base_max
      };

      double mv[3];
      mv[0] = sample_gain(read_gain_r, x, y);
      if (gain_is_monochrome) {
        mv[1] = mv[0];
        mv[2] = mv[0];
      } else {
        mv[1] = sample_gain(read_gain_g, x, y);
        mv[2] = sample_gain(read_gain_b, x, y);
      }

      double hdr_px[3];
      for (int c = 0; c < 3; c++) {
        const auto& ch = meta.channels[c];
        double boff = rational_to_double(ch.base_offset);
        double aoff = rational_to_double(ch.alternate_offset);
        double v = apply_gain(sdr[c], mv[c], ch, weight_factor, boff, aoff);
        hdr_px[c] = std::clamp(v, 0.0, 1.0) * out_max;
      }

      size_t oidx = (size_t) y * out_elems_per_row + x;
      out_r[oidx] = static_cast<uint16_t>(std::round(hdr_px[0]));
      out_g[oidx] = static_cast<uint16_t>(std::round(hdr_px[1]));
      out_b[oidx] = static_cast<uint16_t>(std::round(hdr_px[2]));
    }
  }

  *out = std::move(hdr);
  return Error::Ok;
}


heif_error heif_image_handle_decode_hdr_image(const heif_image_handle* handle,
                                              heif_colorspace colorspace,
                                              heif_chroma chroma,
                                              heif_image** out_img,
                                              const heif_decoding_options* options)
{
  if (!out_img || !handle) {
    return heif_error_null_pointer_argument;
  }
  *out_img = nullptr;

  return exception_guard([&]() -> heif_error {
    heif_decoding_options* dec_options = heif_decoding_options_alloc();
    heif_decoding_options_copy(dec_options, options);

    auto tmap = get_tmap_item(handle);

    // Without a gain map, fall back to an ordinary decode so SDR files keep
    // working unchanged.
    if (!tmap) {
      auto decodingResult = handle->context->decode_image(
          handle->image->get_id(), colorspace, chroma, *dec_options, false, 0, 0, {});
      heif_decoding_options_free(dec_options);
      if (!decodingResult) {
        return decodingResult.error_struct(handle->image.get());
      }
      *out_img = new heif_image();
      (*out_img)->image = *decodingResult;
      return heif_error_success;
    }

    auto metaResult = tmap->get_gain_map_metadata();
    if (metaResult.is_error()) {
      heif_decoding_options_free(dec_options);
      return metaResult.error().error_struct(tmap.get());
    }

    // Decode the base (SDR) rendition as RGB.
    heif_item_id base_id = 0;
    if (Error err = tmap->get_reference_ids(&base_id, nullptr)) {
      heif_decoding_options_free(dec_options);
      return err.error_struct(tmap.get());
    }
    auto baseResult = handle->context->decode_image(
        base_id, heif_colorspace_RGB, heif_chroma_444, *dec_options, false, 0, 0, {});
    if (!baseResult) {
      heif_decoding_options_free(dec_options);
      return baseResult.error_struct(tmap.get());
    }

    auto gainResult = tmap->decode_gain_map_image(*dec_options);
    heif_decoding_options_free(dec_options);
    if (gainResult.is_error()) {
      return gainResult.error().error_struct(tmap.get());
    }

    std::shared_ptr<HeifPixelImage> hdr;
    if (Error err = compose_hdr_image(*baseResult, *gainResult, *metaResult, &hdr)) {
      return err.error_struct(tmap.get());
    }

    *out_img = new heif_image();
    (*out_img)->image = std::move(hdr);
    return heif_error_success;
  });
}
