#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/audio_format.h"

namespace hoodie::audio {

// Decode any libsndfile-supported format (WAV/OGG/FLAC, MP3 build-dependent)
// into interleaved int16 at the file's native rate/channel count.
AudioFrame read_audio_file(const std::string& path);

// Encode an AudioFrame as a 16-bit WAV file.
void write_audio_file(const std::string& path, const AudioFrame& frame);

// In-memory 16-bit PCM WAV, for multipart uploads and tests.
std::vector<uint8_t> encode_wav_pcm16(const AudioFrame& frame);

// Parse a 16-bit PCM WAV byte range. Throws std::runtime_error on malformed
// input. Extra chunks (LIST, fact, ...) are skipped.
AudioFrame decode_wav_pcm16(const uint8_t* data, std::size_t size);

inline AudioFrame decode_wav_pcm16(const std::vector<uint8_t>& bytes) {
  return decode_wav_pcm16(bytes.data(), bytes.size());
}

// Raw interleaved signed 16-bit little-endian samples with no container.
AudioFrame decode_pcm16(const uint8_t* data, std::size_t size, int sample_rate, int channels);

// Decode any libsndfile-supported container (WAV/OGG/FLAC, MP3 build-dependent)
// held in memory.
AudioFrame decode_audio_memory(const uint8_t* data, std::size_t size,
                               const std::string& content_type = "");

inline AudioFrame decode_audio_memory(const std::vector<uint8_t>& bytes,
                                      const std::string& content_type = "") {
  return decode_audio_memory(bytes.data(), bytes.size(), content_type);
}

}  // namespace hoodie::audio
