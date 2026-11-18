/*
  libheif unit tests for visual sequence encoding (AVIF/HEIF image sequences).

  MIT License

  Copyright (c) 2026 libheif contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

// End-to-end coverage of the sequence writing API:
//
//   heif_context_add_visual_sequence_track() / heif_track_encode_sequence_image()
//
// The read side already had extensive coverage; these tests pin the write side,
// in particular:
//
//   - the produced file carries the sequence structural brands (and 'avis' for
//     an AV1 track), a moov/trak and the per-frame durations and loop count that
//     were requested;
//   - frames whose size differs from the first frame fail with a distinguishable
//     Invalid_input error instead of corrupting the sample entry;
//   - encoding into a missing (NULL) track or a non-visual track fails with a
//     distinguishable Usage error instead of dereferencing NULL;
//   - the max_sequence_frames security limit is enforced while encoding;
//   - the still-image writing path is untouched.

#include "catch_amalgamated.hpp"
#include "libheif/heif.h"
#include "libheif/heif_sequences.h"
#include "libheif/heif_brands.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t WIDTH = 64;
constexpr uint32_t HEIGHT = 64;

heif_error mem_writer(heif_context*, const void* data, size_t size, void* userdata)
{
  auto* out = static_cast<std::vector<uint8_t>*>(userdata);
  const auto* p = static_cast<const uint8_t*>(data);
  out->insert(out->end(), p, p + size);
  return heif_error{heif_error_Ok, heif_suberror_Unspecified, nullptr};
}

// Monochrome 8-bit frame. Both the built-in uncompressed encoder and the AV1
// (aom) encoder accept monochrome input, so the same frame builder serves both.
heif_image* make_frame(uint32_t width, uint32_t height, uint32_t duration, uint8_t value = 0x40)
{
  heif_image* image = nullptr;
  REQUIRE(heif_image_create(width, height, heif_colorspace_monochrome,
                            heif_chroma_monochrome, &image).code == heif_error_Ok);
  REQUIRE(image != nullptr);
  REQUIRE(heif_image_add_plane(image, heif_channel_Y, width, height, 8).code == heif_error_Ok);

  size_t stride = 0;
  uint8_t* pixels = heif_image_get_plane2(image, heif_channel_Y, &stride);
  REQUIRE(pixels != nullptr);
  for (uint32_t y = 0; y < height; y++) {
    std::memset(pixels + y * stride, value, width);
  }

  heif_image_set_duration(image, duration);
  return image;
}

// Pick an encoder that can encode a visual sequence, preferring the built-in
// uncompressed codec because it is instant and deterministic. Returns
// heif_compression_undefined when nothing usable is available.
heif_compression_format pick_sequence_format()
{
  if (heif_have_encoder_for_format(heif_compression_uncompressed)) {
    return heif_compression_uncompressed;
  }
  if (heif_have_encoder_for_format(heif_compression_AV1)) {
    return heif_compression_AV1;
  }
  return heif_compression_undefined;
}

heif_encoder* get_encoder(heif_context* ctx, heif_compression_format format)
{
  heif_encoder* encoder = nullptr;
  REQUIRE(heif_context_get_encoder_for_format(ctx, format, &encoder).code == heif_error_Ok);
  REQUIRE(encoder != nullptr);
  return encoder;
}

// Encode 'frames' frames (each with the given duration) into an image-sequence
// track and write the result to memory. When 'repetitions' is given, the context
// loop count is set accordingly. The returned buffer is a complete HEIF file.
std::vector<uint8_t> encode_sequence(heif_compression_format format,
                                     uint32_t frame_count,
                                     uint32_t frame_duration,
                                     uint32_t repetitions = 1,
                                     uint32_t width = WIDTH,
                                     uint32_t height = HEIGHT)
{
  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  if (repetitions != 1) {
    heif_context_set_number_of_sequence_repetitions(ctx, repetitions);
  }

  heif_encoder* encoder = get_encoder(ctx, format);

  heif_track* track = nullptr;
  heif_error err = heif_context_add_visual_sequence_track(ctx,
                                                          static_cast<uint16_t>(width),
                                                          static_cast<uint16_t>(height),
                                                          heif_track_type_image_sequence,
                                                          nullptr, nullptr, &track);
  INFO("add_visual_sequence_track: " << (err.message ? err.message : ""));
  REQUIRE(err.code == heif_error_Ok);
  REQUIRE(track != nullptr);

  for (uint32_t i = 0; i < frame_count; i++) {
    heif_image* image = make_frame(width, height, frame_duration,
                                   static_cast<uint8_t>(0x40 + static_cast<int>(i) * 10));
    err = heif_track_encode_sequence_image(track, image, encoder, nullptr);
    INFO("encode_sequence_image frame " << i << ": " << (err.message ? err.message : ""));
    REQUIRE(err.code == heif_error_Ok);
    heif_image_release(image);
  }

  REQUIRE(heif_track_encode_end_of_sequence(track, encoder).code == heif_error_Ok);

  std::vector<uint8_t> file;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = mem_writer;
  REQUIRE(heif_context_write(ctx, &writer, &file).code == heif_error_Ok);

  heif_track_release(track);
  heif_encoder_release(encoder);
  heif_context_free(ctx);

  return file;
}

} // namespace


TEST_CASE("sequence encoding produces a readable image sequence with timed frames")
{
  const heif_compression_format format = pick_sequence_format();
  if (format == heif_compression_undefined) {
    SKIP("Skipping because no sequence encoder is compiled in.");
  }

  const uint32_t frame_count = 4;
  const uint32_t frame_duration = 1000;

  std::vector<uint8_t> file = encode_sequence(format, frame_count, frame_duration);
  REQUIRE(!file.empty());

  // The file is marked as a sequence with the generic sequence structural brand.
  REQUIRE(heif_has_compatible_brand(file.data(), static_cast<int>(file.size()), "msf1") == 1);
  REQUIRE(heif_has_compatible_brand(file.data(), static_cast<int>(file.size()), "isom") == 1);

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  REQUIRE(heif_context_read_from_memory(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok);

  REQUIRE(heif_context_has_sequence(ctx) == 1);
  REQUIRE(heif_context_number_of_sequence_tracks(ctx) == 1);

  heif_track* track = heif_context_get_track(ctx, 0);
  REQUIRE(track != nullptr);
  REQUIRE(heif_track_get_track_handler_type(track) == heif_track_type_image_sequence);

  uint16_t got_width = 0, got_height = 0;
  REQUIRE(heif_track_get_image_resolution(track, &got_width, &got_height).code == heif_error_Ok);
  REQUIRE(got_width == WIDTH);
  REQUIRE(got_height == HEIGHT);

  // Every frame must come back with the duration it was encoded with.
  std::vector<uint32_t> durations;
  for (;;) {
    heif_image* image = nullptr;
    heif_error err = heif_track_decode_next_image(track, &image,
                                                  heif_colorspace_undefined,
                                                  heif_chroma_undefined, nullptr);
    if (err.code == heif_error_End_of_sequence) {
      break;
    }
    INFO("decode_next_image: " << (err.message ? err.message : ""));
    REQUIRE(err.code == heif_error_Ok);
    REQUIRE(image != nullptr);
    durations.push_back(heif_image_get_duration(image));
    heif_image_release(image);
  }

  REQUIRE(durations.size() == frame_count);
  for (uint32_t duration : durations) {
    REQUIRE(duration == frame_duration);
  }

  // A single playback by default.
  REQUIRE(heif_track_get_number_of_repetitions(track) == 1);

  heif_track_release(track);
  heif_context_free(ctx);
}


TEST_CASE("AV1 image sequence is written with the avis brand")
{
  if (!heif_have_encoder_for_format(heif_compression_AV1)) {
    SKIP("Skipping because no AV1 encoder is compiled in.");
  }

  std::vector<uint8_t> file = encode_sequence(heif_compression_AV1, 3, 1000);
  REQUIRE(!file.empty());

  // AV1 image sequence: major/compatible 'avis', together with the generic
  // sequence brands.
  REQUIRE(heif_read_main_brand(file.data(), static_cast<int>(file.size())) == heif_brand2_avis);
  REQUIRE(heif_has_compatible_brand(file.data(), static_cast<int>(file.size()), "avis") == 1);
  REQUIRE(heif_has_compatible_brand(file.data(), static_cast<int>(file.size()), "msf1") == 1);

  // The AVIF sequence MIME type must be detected.
  const char* mime = heif_get_file_mime_type(file.data(), static_cast<int>(file.size()));
  REQUIRE(mime != nullptr);
  REQUIRE(std::strcmp(mime, "image/avif-sequence") == 0);
}


TEST_CASE("sequence loop count is written and read back through the edit list")
{
  const heif_compression_format format = pick_sequence_format();
  if (format == heif_compression_undefined) {
    SKIP("Skipping because no sequence encoder is compiled in.");
  }

  const uint32_t frame_count = 3;
  const uint32_t repetitions = 4;

  std::vector<uint8_t> file = encode_sequence(format, frame_count, 500, repetitions);

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  REQUIRE(heif_context_read_from_memory(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok);

  heif_track* track = heif_context_get_track(ctx, 0);
  REQUIRE(track != nullptr);
  REQUIRE(heif_track_get_number_of_repetitions(track) == repetitions);

  // With the edit list applied, decoding walks every repetition.
  uint32_t decoded = 0;
  for (;;) {
    heif_image* image = nullptr;
    heif_error err = heif_track_decode_next_image(track, &image,
                                                  heif_colorspace_undefined,
                                                  heif_chroma_undefined, nullptr);
    if (err.code == heif_error_End_of_sequence) {
      break;
    }
    REQUIRE(err.code == heif_error_Ok);
    REQUIRE(image != nullptr);
    heif_image_release(image);
    decoded++;
  }
  REQUIRE(decoded == frame_count * repetitions);

  // Ignoring the edit list plays the physical media timeline exactly once.
  // Decoding is single-pass per Track object, so read the file again in a fresh
  // context rather than rewinding the exhausted iterator above.
  heif_context* once_ctx = heif_context_alloc();
  REQUIRE(once_ctx != nullptr);
  REQUIRE(heif_context_read_from_memory(once_ctx, file.data(), file.size(), nullptr).code == heif_error_Ok);
  heif_track* once_track = heif_context_get_track(once_ctx, 0);
  REQUIRE(once_track != nullptr);

  heif_decoding_options* decode_options = heif_decoding_options_alloc();
  REQUIRE(decode_options != nullptr);
  decode_options->ignore_sequence_editlist = 1;

  decoded = 0;
  for (;;) {
    heif_image* image = nullptr;
    heif_error err = heif_track_decode_next_image(once_track, &image,
                                                  heif_colorspace_undefined,
                                                  heif_chroma_undefined, decode_options);
    if (err.code == heif_error_End_of_sequence) {
      break;
    }
    REQUIRE(err.code == heif_error_Ok);
    REQUIRE(image != nullptr);
    heif_image_release(image);
    decoded++;
  }
  REQUIRE(decoded == frame_count);

  heif_decoding_options_free(decode_options);
  heif_track_release(once_track);
  heif_context_free(once_ctx);
  heif_track_release(track);
  heif_context_free(ctx);
}


TEST_CASE("infinite sequence repetition round-trips")
{
  const heif_compression_format format = pick_sequence_format();
  if (format == heif_compression_undefined) {
    SKIP("Skipping because no sequence encoder is compiled in.");
  }

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  heif_context_set_number_of_sequence_repetitions(ctx, heif_sequence_maximum_number_of_repetitions);

  heif_encoder* encoder = get_encoder(ctx, format);

  heif_track* track = nullptr;
  REQUIRE(heif_context_add_visual_sequence_track(ctx, WIDTH, HEIGHT,
                                                 heif_track_type_image_sequence,
                                                 nullptr, nullptr, &track).code == heif_error_Ok);

  heif_image* image = make_frame(WIDTH, HEIGHT, 1000);
  REQUIRE(heif_track_encode_sequence_image(track, image, encoder, nullptr).code == heif_error_Ok);
  heif_image_release(image);
  REQUIRE(heif_track_encode_end_of_sequence(track, encoder).code == heif_error_Ok);

  std::vector<uint8_t> file;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = mem_writer;
  REQUIRE(heif_context_write(ctx, &writer, &file).code == heif_error_Ok);

  heif_encoder_release(encoder);
  heif_track_release(track);
  heif_context_free(ctx);

  heif_context* read_ctx = heif_context_alloc();
  REQUIRE(heif_context_read_from_memory(read_ctx, file.data(), file.size(), nullptr).code == heif_error_Ok);
  heif_track* read_track = heif_context_get_track(read_ctx, 0);
  REQUIRE(read_track != nullptr);
  REQUIRE(heif_track_get_number_of_repetitions(read_track) ==
          heif_sequence_track_number_of_repetitions_infinite);

  heif_track_release(read_track);
  heif_context_free(read_ctx);
}


TEST_CASE("frames that differ in size from the first frame are rejected")
{
  const heif_compression_format format = pick_sequence_format();
  if (format == heif_compression_undefined) {
    SKIP("Skipping because no sequence encoder is compiled in.");
  }

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);

  heif_encoder* encoder = get_encoder(ctx, format);

  heif_track* track = nullptr;
  REQUIRE(heif_context_add_visual_sequence_track(ctx, WIDTH, HEIGHT,
                                                 heif_track_type_image_sequence,
                                                 nullptr, nullptr, &track).code == heif_error_Ok);
  REQUIRE(track != nullptr);

  heif_image* first = make_frame(WIDTH, HEIGHT, 1000);
  REQUIRE(heif_track_encode_sequence_image(track, first, encoder, nullptr).code == heif_error_Ok);
  heif_image_release(first);

  // Each mismatch must fail with the same distinguishable error, independent of
  // which dimension differs. This holds even for codecs whose plugin has no size
  // check of its own (e.g. the uncompressed codec).
  for (auto [bad_width, bad_height] : {std::pair{WIDTH / 2, HEIGHT},
                                       std::pair{WIDTH, HEIGHT / 2},
                                       std::pair{WIDTH / 2, HEIGHT / 2}}) {
    heif_image* bad = make_frame(bad_width, bad_height, 1000);
    heif_error err = heif_track_encode_sequence_image(track, bad, encoder, nullptr);
    INFO("mismatched frame " << bad_width << "x" << bad_height << ": "
                             << (err.message ? err.message : ""));
    REQUIRE(err.code == heif_error_Invalid_input);
    REQUIRE(err.message != nullptr);
    REQUIRE(std::strstr(err.message, "size of the first frame") != nullptr);
    heif_image_release(bad);
  }

  // A rejected frame leaves the sequence usable: a correctly sized frame still
  // encodes and the file can be written.
  heif_image* good = make_frame(WIDTH, HEIGHT, 1000);
  REQUIRE(heif_track_encode_sequence_image(track, good, encoder, nullptr).code == heif_error_Ok);
  heif_image_release(good);

  REQUIRE(heif_track_encode_end_of_sequence(track, encoder).code == heif_error_Ok);

  std::vector<uint8_t> file;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = mem_writer;
  REQUIRE(heif_context_write(ctx, &writer, &file).code == heif_error_Ok);
  REQUIRE(!file.empty());

  heif_encoder_release(encoder);
  heif_track_release(track);
  heif_context_free(ctx);
}


TEST_CASE("encoding without a track reports a distinguishable usage error")
{
  const heif_compression_format format = pick_sequence_format();
  if (format == heif_compression_undefined) {
    SKIP("Skipping because no sequence encoder is compiled in.");
  }

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  heif_encoder* encoder = get_encoder(ctx, format);
  heif_image* image = make_frame(WIDTH, HEIGHT, 1000);

  // A NULL track (e.g. a failed track creation was ignored) must return an error
  // instead of crashing on the NULL pointer.
  heif_error err = heif_track_encode_sequence_image(nullptr, image, encoder, nullptr);
  REQUIRE(err.code == heif_error_Usage_error);
  REQUIRE(err.subcode == heif_suberror_Null_pointer_argument);

  err = heif_track_encode_end_of_sequence(nullptr, encoder);
  REQUIRE(err.code == heif_error_Usage_error);
  REQUIRE(err.subcode == heif_suberror_Null_pointer_argument);

  // A track of the wrong kind (metadata, not visual) is also a usage error, but
  // distinguishable from the NULL case by its subcode.
  heif_track* metadata_track = nullptr;
  REQUIRE(heif_context_add_uri_metadata_sequence_track(ctx, "application/x-test",
                                                       nullptr, &metadata_track).code == heif_error_Ok);
  REQUIRE(metadata_track != nullptr);

  err = heif_track_encode_sequence_image(metadata_track, image, encoder, nullptr);
  REQUIRE(err.code == heif_error_Usage_error);
  REQUIRE(err.subcode == heif_suberror_Invalid_parameter_value);

  heif_track_release(metadata_track);
  heif_image_release(image);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}


TEST_CASE("encoding enforces the max_sequence_frames security limit")
{
  const heif_compression_format format = pick_sequence_format();
  if (format == heif_compression_undefined) {
    SKIP("Skipping because no sequence encoder is compiled in.");
  }

  constexpr uint32_t kFrameLimit = 3;

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);

  heif_security_limits* limits = heif_context_get_security_limits(ctx);
  REQUIRE(limits != nullptr);
  limits->max_sequence_frames = kFrameLimit;

  heif_encoder* encoder = get_encoder(ctx, format);

  heif_track* track = nullptr;
  REQUIRE(heif_context_add_visual_sequence_track(ctx, WIDTH, HEIGHT,
                                                 heif_track_type_image_sequence,
                                                 nullptr, nullptr, &track).code == heif_error_Ok);

  // Frames 0..kFrameLimit-1 encode fine.
  for (uint32_t i = 0; i < kFrameLimit; i++) {
    heif_image* image = make_frame(WIDTH, HEIGHT, 1000);
    REQUIRE(heif_track_encode_sequence_image(track, image, encoder, nullptr).code == heif_error_Ok);
    heif_image_release(image);
  }

  // The next frame trips the limit with a distinguishable security error.
  heif_image* overflow = make_frame(WIDTH, HEIGHT, 1000);
  heif_error err = heif_track_encode_sequence_image(track, overflow, encoder, nullptr);
  REQUIRE(err.code == heif_error_Memory_allocation_error);
  REQUIRE(err.subcode == heif_suberror_Security_limit_exceeded);
  REQUIRE(err.message != nullptr);
  REQUIRE(std::strstr(err.message, "maximum number of sequence frames") != nullptr);
  heif_image_release(overflow);

  heif_encoder_release(encoder);
  heif_track_release(track);
  heif_context_free(ctx);
}


TEST_CASE("a limit of zero leaves the number of encoded frames unrestricted")
{
  const heif_compression_format format = pick_sequence_format();
  if (format == heif_compression_undefined) {
    SKIP("Skipping because no sequence encoder is compiled in.");
  }

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);

  heif_security_limits* limits = heif_context_get_security_limits(ctx);
  limits->max_sequence_frames = 0; // documented as "limit disabled"

  heif_encoder* encoder = get_encoder(ctx, format);

  heif_track* track = nullptr;
  REQUIRE(heif_context_add_visual_sequence_track(ctx, WIDTH, HEIGHT,
                                                 heif_track_type_image_sequence,
                                                 nullptr, nullptr, &track).code == heif_error_Ok);

  constexpr uint32_t kFrames = 6;
  for (uint32_t i = 0; i < kFrames; i++) {
    heif_image* image = make_frame(WIDTH, HEIGHT, 1000);
    REQUIRE(heif_track_encode_sequence_image(track, image, encoder, nullptr).code == heif_error_Ok);
    heif_image_release(image);
  }
  REQUIRE(heif_track_encode_end_of_sequence(track, encoder).code == heif_error_Ok);

  std::vector<uint8_t> file;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = mem_writer;
  REQUIRE(heif_context_write(ctx, &writer, &file).code == heif_error_Ok);
  REQUIRE(!file.empty());

  heif_encoder_release(encoder);
  heif_track_release(track);
  heif_context_free(ctx);
}


TEST_CASE("still image writing is unaffected by the sequence changes")
{
  const heif_compression_format format = pick_sequence_format();
  if (format == heif_compression_undefined) {
    SKIP("Skipping because no encoder is compiled in.");
  }

  heif_image* image = make_frame(WIDTH, HEIGHT, 1000);

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  heif_encoder* encoder = get_encoder(ctx, format);

  // The single-image path: no track, no sequence API calls.
  REQUIRE(heif_context_encode_image(ctx, image, encoder, nullptr, nullptr).code == heif_error_Ok);

  std::vector<uint8_t> file;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = mem_writer;
  REQUIRE(heif_context_write(ctx, &writer, &file).code == heif_error_Ok);
  REQUIRE(!file.empty());

  // A still image has no sequence and must not carry the sequence structural brand.
  REQUIRE(heif_context_has_sequence(ctx) == 0);
  REQUIRE(heif_has_compatible_brand(file.data(), static_cast<int>(file.size()), "msf1") == 0);

  heif_encoder_release(encoder);
  heif_context_free(ctx);
  heif_image_release(image);
}
