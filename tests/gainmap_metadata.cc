/*
 * libheif ISO/IEC 21496-1 gain map metadata unit tests.
 *
 * MIT License
 *
 * Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "catch_amalgamated.hpp"
#include <bitstream.h>
#include <box.h>
#include <gainmap.h>
#include "error.h"

#include <cstdint>
#include <memory>
#include <vector>


// Build an ISO/IEC 21496-1 payload with the given flag byte and otherwise
// fixed, valid rational values. With the multi-channel flag (bit 7) set,
// three identical channel blocks are emitted.
static std::vector<uint8_t> make_payload(uint8_t flags)
{
  std::vector<uint8_t> d;
  auto u16 = [&](uint16_t v) {
    d.push_back(uint8_t(v >> 8));
    d.push_back(uint8_t(v));
  };
  auto u8 = [&](uint8_t v) { d.push_back(v); };
  auto u32 = [&](uint32_t v) {
    d.push_back(uint8_t(v >> 24));
    d.push_back(uint8_t(v >> 16));
    d.push_back(uint8_t(v >> 8));
    d.push_back(uint8_t(v));
  };
  auto s32 = [&](int32_t v) { u32(static_cast<uint32_t>(v)); };

  u16(0);             // minimum_version
  u16(0);             // writer_version
  u8(flags);          // flags
  u32(0); u32(1);     // base_hdr_headroom 0/1
  u32(1); u32(1);     // alternate_hdr_headroom 1/1

  const int n = (flags & 0x80) ? 3 : 1;
  for (int i = 0; i < n; i++) {
    s32(0); u32(1);   // gain_map_min 0/1
    s32(1); u32(1);   // gain_map_max 1/1
    u32(1); u32(1);   // gamma 1/1
    s32(1); u32(64);  // base_offset 1/64
    s32(1); u32(64);  // alternate_offset 1/64
  }
  return d;
}


TEST_CASE("gainmap metadata parse single channel")
{
  auto payload = make_payload(0x40); // use_base_color_space, single channel
  GainMapMetadata m;
  Error err = parse_gain_map_metadata(payload, &m);
  REQUIRE(err == Error::Ok);
  REQUIRE(m.minimum_version == 0);
  REQUIRE(m.nb_channels == 1);
  REQUIRE(m.multi_channel == false);
  REQUIRE(m.use_base_color_space == true);
  REQUIRE(m.base_hdr_headroom.num == 0);
  REQUIRE(m.alternate_hdr_headroom.num == 1);
  // single stored channel is replicated over all three logical channels.
  for (int c = 0; c < 3; c++) {
    REQUIRE(m.channels[c].gamma.num == 1);
    REQUIRE(m.channels[c].gamma.den == 1);
    REQUIRE(m.channels[c].base_offset.den == 64);
    REQUIRE(m.channels[c].gain_map_max.num == 1);
  }
}


TEST_CASE("gainmap metadata round trip")
{
  auto payload = make_payload(0xC0); // multi-channel flag set, base cg flag set
  GainMapMetadata m;
  REQUIRE(parse_gain_map_metadata(payload, &m) == Error::Ok);
  REQUIRE(m.nb_channels == 3);

  std::vector<uint8_t> out;
  REQUIRE(write_gain_map_metadata(m, &out) == Error::Ok);
  REQUIRE(out == payload);

  // Re-parse the serialized bytes.
  GainMapMetadata m2;
  REQUIRE(parse_gain_map_metadata(out, &m2) == Error::Ok);
  REQUIRE(m2.nb_channels == 3);
  REQUIRE(m2.use_base_color_space == true);
  REQUIRE(m2.channels[2].alternate_offset.den == 64);
}


TEST_CASE("hdrgm box factory and round trip")
{
  auto payload = make_payload(0x40);
  auto hdrgm_in = std::make_shared<Box_hdrgm>();
  REQUIRE(hdrgm_in->parse_payload(payload) == Error::Ok);

  StreamWriter writer;
  REQUIRE(hdrgm_in->write(writer) == Error::Ok);
  auto bytes = writer.get_data();
  REQUIRE(bytes.size() == 8 + payload.size());
  REQUIRE(bytes[4] == 'h');
  REQUIRE(bytes[5] == 'd');
  REQUIRE(bytes[6] == 'r');
  REQUIRE(bytes[7] == 'g');

  auto reader = std::make_shared<StreamReader_memory>(bytes.data(), bytes.size(), false);
  BitstreamRange range(reader, bytes.size());
  std::shared_ptr<Box> box;
  REQUIRE(Box::read(range, &box, heif_get_global_security_limits()) == Error::Ok);
  REQUIRE(box->get_short_type() == fourcc("hdrgm"));
  auto hdrgm_out = std::dynamic_pointer_cast<Box_hdrgm>(box);
  REQUIRE(hdrgm_out);
  REQUIRE(hdrgm_out->get_metadata().nb_channels == 1);
  REQUIRE(hdrgm_out->get_metadata().channels[0].gamma.den == 1);
}


TEST_CASE("gainmap metadata unsupported version is distinguishable")
{
  auto payload = make_payload(0x40);
  payload[0] = 0x00;
  payload[1] = 0x01; // minimum_version = 1, unsupported
  GainMapMetadata m;
  Error err = parse_gain_map_metadata(payload, &m);
  REQUIRE(err.error_code == heif_error_Unsupported_feature);
  REQUIRE(err.sub_error_code == heif_suberror_Unsupported_data_version);
}


TEST_CASE("gainmap metadata channel count mismatch is distinguishable")
{
  // A three-channel metadata block applied to a monochrome (1-component)
  // gain map image must be rejected with the dedicated subcode.
  auto payload = make_payload(0xC0); // multi-channel
  GainMapMetadata m;
  REQUIRE(parse_gain_map_metadata(payload, &m) == Error::Ok);
  REQUIRE(m.nb_channels == 3);

  Error err = check_gain_map_channel_count_matches(m, 1);
  REQUIRE(err.error_code == heif_error_Invalid_input);
  REQUIRE(err.sub_error_code == heif_suberror_Invalid_gain_map_channel_count);

  // Matching counts succeed.
  REQUIRE(check_gain_map_channel_count_matches(m, 3) == Error::Ok);

  // Single-channel metadata against a 3-component image also fails.
  auto single = make_payload(0x40);
  GainMapMetadata m1;
  REQUIRE(parse_gain_map_metadata(single, &m1) == Error::Ok);
  REQUIRE(m1.nb_channels == 1);
  Error err2 = check_gain_map_channel_count_matches(m1, 3);
  REQUIRE(err2.sub_error_code == heif_suberror_Invalid_gain_map_channel_count);
}


TEST_CASE("gainmap metadata missing/truncated is distinguishable")
{
  // A payload shorter than the minimum single-channel size.
  std::vector<uint8_t> tiny(20, 0);
  GainMapMetadata m;
  Error err = parse_gain_map_metadata(tiny, &m);
  REQUIRE(err.error_code == heif_error_Invalid_input);
  REQUIRE(err.sub_error_code == heif_suberror_End_of_data);

  // A multi-channel payload truncated to a single channel block is also
  // reported as truncated, not as a channel-count error.
  auto multi = make_payload(0xC0);
  multi.resize(5 + 2 * 8 + 1 * 5 * 8);
  Error err_trunc = parse_gain_map_metadata(multi, &m);
  REQUIRE(err_trunc.sub_error_code == heif_suberror_End_of_data);

  // Reserved flag bits set by a version-0 writer.
  auto payload = make_payload(0x01);
  Error err2 = parse_gain_map_metadata(payload, &m);
  REQUIRE(err2.error_code == heif_error_Invalid_input);
  REQUIRE(err2.sub_error_code == heif_suberror_Invalid_gain_map_metadata);
}


TEST_CASE("gainmap metadata zero denominator rejected")
{
  auto payload = make_payload(0x40);
  // Corrupt the headroom denominator (bytes 7..10 -> 0).
  payload[10] = 0;
  payload[11] = 0;
  payload[12] = 0;
  payload[13] = 0;
  GainMapMetadata m;
  Error err = parse_gain_map_metadata(payload, &m);
  REQUIRE(err.error_code == heif_error_Invalid_input);
  REQUIRE(err.sub_error_code == heif_suberror_Invalid_gain_map_metadata);
}


TEST_CASE("gainmap dedicated error subcodes are distinct")
{
  // The three distinguishable failure conditions the task requires must map
  // to three different subcodes, none colliding with each other.
  REQUIRE(heif_suberror_Gain_map_missing != heif_suberror_Invalid_gain_map_metadata);
  REQUIRE(heif_suberror_Invalid_gain_map_metadata !=
          heif_suberror_Invalid_gain_map_channel_count);
  REQUIRE(heif_suberror_Gain_map_missing !=
          heif_suberror_Invalid_gain_map_channel_count);
  // And all are Invalid_input subcodes distinct from the version subcode.
  REQUIRE(heif_suberror_Unsupported_data_version != heif_suberror_Gain_map_missing);
}
