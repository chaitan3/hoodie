#include "audio/wav.h"

#include <sndfile.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace hoodie::audio {
namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

bool tag_equals(const uint8_t* p, const char* tag) {
  return std::memcmp(p, tag, 4) == 0;
}

}  // namespace

AudioFrame read_audio_file(const std::string& path) {
  SF_INFO info{};
  SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
  if (file == nullptr) {
    throw std::runtime_error("cannot open audio file '" + path + "': " + sf_strerror(nullptr));
  }

  AudioFrame frame;
  frame.sample_rate = info.samplerate;
  frame.channels = info.channels;

  constexpr sf_count_t kChunkFrames = 4096;
  std::vector<int16_t> buffer(static_cast<std::size_t>(kChunkFrames) * info.channels);
  sf_count_t read = 0;
  while ((read = sf_readf_short(file, buffer.data(), kChunkFrames)) > 0) {
    const auto count = static_cast<std::size_t>(read) * static_cast<std::size_t>(info.channels);
    frame.samples.insert(frame.samples.end(), buffer.begin(), buffer.begin() + count);
  }
  sf_close(file);
  return frame;
}

void write_audio_file(const std::string& path, const AudioFrame& frame) {
  SF_INFO info{};
  info.samplerate = frame.sample_rate;
  info.channels = frame.channels;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

  SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
  if (file == nullptr) {
    throw std::runtime_error("cannot write audio file '" + path + "': " + sf_strerror(nullptr));
  }
  sf_writef_short(file, frame.samples.data(), static_cast<sf_count_t>(frame.frames()));
  sf_close(file);
}

std::vector<uint8_t> encode_wav_pcm16(const AudioFrame& frame) {
  const uint32_t data_bytes = static_cast<uint32_t>(frame.samples.size() * sizeof(int16_t));
  const uint16_t channels = static_cast<uint16_t>(frame.channels);
  const uint32_t rate = static_cast<uint32_t>(frame.sample_rate);
  const uint16_t bits = 16;
  const uint32_t byte_rate = rate * channels * (bits / 8);
  const uint16_t block_align = static_cast<uint16_t>(channels * (bits / 8));

  std::vector<uint8_t> out;
  out.reserve(44 + data_bytes);
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  put_u32(out, 36 + data_bytes);
  out.insert(out.end(), {'W', 'A', 'V', 'E'});
  out.insert(out.end(), {'f', 'm', 't', ' '});
  put_u32(out, 16);            // PCM fmt chunk size
  put_u16(out, 1);             // audio format = PCM
  put_u16(out, channels);
  put_u32(out, rate);
  put_u32(out, byte_rate);
  put_u16(out, block_align);
  put_u16(out, bits);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  put_u32(out, data_bytes);

  const auto* bytes = reinterpret_cast<const uint8_t*>(frame.samples.data());
  out.insert(out.end(), bytes, bytes + data_bytes);
  return out;
}

AudioFrame decode_wav_pcm16(const uint8_t* data, std::size_t size) {
  if (size < 12 || !tag_equals(data, "RIFF") || !tag_equals(data + 8, "WAVE")) {
    throw std::runtime_error("decode_wav_pcm16: not a RIFF/WAVE stream");
  }

  AudioFrame frame;
  frame.channels = 1;
  frame.sample_rate = 0;
  uint16_t bits = 0;
  const uint8_t* audio = nullptr;
  std::size_t audio_bytes = 0;

  std::size_t offset = 12;
  while (offset + 8 <= size) {
    const uint8_t* id = data + offset;
    const uint32_t chunk_size = read_u32(data + offset + 4);
    const std::size_t body = offset + 8;
    if (body + chunk_size > size) break;  // truncated

    if (tag_equals(id, "fmt ")) {
      if (chunk_size < 16) throw std::runtime_error("decode_wav_pcm16: short fmt chunk");
      const uint16_t audio_format = read_u16(data + body);
      if (audio_format != 1) throw std::runtime_error("decode_wav_pcm16: not PCM");
      frame.channels = read_u16(data + body + 2);
      frame.sample_rate = static_cast<int>(read_u32(data + body + 4));
      bits = read_u16(data + body + 14);
    } else if (tag_equals(id, "data")) {
      audio = data + body;
      audio_bytes = chunk_size;
    }

    offset = body + chunk_size + (chunk_size & 1u);  // chunks are word-aligned
  }

  if (frame.channels <= 0 || frame.sample_rate <= 0) {
    throw std::runtime_error("decode_wav_pcm16: missing fmt chunk");
  }
  if (bits != 16) {
    throw std::runtime_error("decode_wav_pcm16: only 16-bit PCM is supported");
  }
  if (audio == nullptr) {
    throw std::runtime_error("decode_wav_pcm16: missing data chunk");
  }

  const std::size_t sample_count = audio_bytes / sizeof(int16_t);
  frame.samples.resize(sample_count);
  std::memcpy(frame.samples.data(), audio, sample_count * sizeof(int16_t));
  return frame;
}

AudioFrame decode_pcm16(const uint8_t* data, std::size_t size, int sample_rate, int channels) {
  if (sample_rate <= 0 || channels <= 0) {
    throw std::runtime_error("decode_pcm16: invalid sample rate or channel count");
  }
  AudioFrame frame;
  frame.sample_rate = sample_rate;
  frame.channels = channels;
  const std::size_t sample_count = size / sizeof(int16_t);
  frame.samples.resize(sample_count);
  std::memcpy(frame.samples.data(), data, sample_count * sizeof(int16_t));
  return frame;
}

namespace {

struct MemoryReader {
  const uint8_t* data = nullptr;
  std::size_t size = 0;
  std::size_t position = 0;
};

sf_count_t memory_get_filelen(void* user) {
  return static_cast<sf_count_t>(static_cast<MemoryReader*>(user)->size);
}

sf_count_t memory_seek(sf_count_t offset, int whence, void* user) {
  auto* reader = static_cast<MemoryReader*>(user);
  std::int64_t base = 0;
  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<std::int64_t>(reader->position); break;
    case SEEK_END: base = static_cast<std::int64_t>(reader->size); break;
    default: return -1;
  }
  const std::int64_t target = base + offset;
  if (target < 0) return -1;
  reader->position = static_cast<std::size_t>(target);
  return static_cast<sf_count_t>(reader->position);
}

sf_count_t memory_read(void* ptr, sf_count_t count, void* user) {
  auto* reader = static_cast<MemoryReader*>(user);
  const std::size_t remaining = reader->size - std::min(reader->position, reader->size);
  const auto to_read = static_cast<std::size_t>(count) < remaining ? static_cast<std::size_t>(count)
                                                                    : remaining;
  std::memcpy(ptr, reader->data + reader->position, to_read);
  reader->position += to_read;
  return static_cast<sf_count_t>(to_read);
}

sf_count_t memory_write(const void*, sf_count_t, void*) { return 0; }

sf_count_t memory_tell(void* user) {
  return static_cast<sf_count_t>(static_cast<MemoryReader*>(user)->position);
}

}  // namespace

AudioFrame decode_audio_memory(const uint8_t* data, std::size_t size,
                               const std::string& content_type) {
  MemoryReader reader{data, size, 0};
  const SF_VIRTUAL_IO virtual_io{memory_get_filelen, memory_seek, memory_read, memory_write,
                                 memory_tell};

  SF_INFO info{};
  SNDFILE* file = sf_open_virtual(const_cast<SF_VIRTUAL_IO*>(&virtual_io), SFM_READ, &info,
                                  &reader);
  if (file == nullptr) {
    throw std::runtime_error("decode_audio_memory: unsupported audio (" +
                             std::string(sf_strerror(nullptr)) +
                             (content_type.empty() ? ")" : ", content-type=" + content_type + ")"));
  }

  AudioFrame frame;
  frame.sample_rate = info.samplerate;
  frame.channels = info.channels;

  constexpr sf_count_t kChunkFrames = 4096;
  std::vector<int16_t> buffer(static_cast<std::size_t>(kChunkFrames) * info.channels);
  sf_count_t read = 0;
  while ((read = sf_readf_short(file, buffer.data(), kChunkFrames)) > 0) {
    const auto count = static_cast<std::size_t>(read) * static_cast<std::size_t>(info.channels);
    frame.samples.insert(frame.samples.end(), buffer.begin(), buffer.begin() + count);
  }
  sf_close(file);
  return frame;
}

}  // namespace hoodie::audio
