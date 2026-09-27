#pragma once

#include <string>

#include "agent/tool_registry.h"
#include "audio/audio_format.h"

namespace hoodie::app {

// Play a frame on `device` ("" = system default) and wait for it to drain.
void play_frame(const audio::AudioFrame& frame, const std::string& device);

// Write the frame to `out_file` when non-empty, otherwise play it.
void play_or_write(const audio::AudioFrame& frame, const std::string& out_file,
                   const std::string& device);

// Resolve a play_music request with yt-dlp and play/download the result. No-op
// when `music.id` is empty.
void play_music_request(const agent::MusicRequest& music, const std::string& out_file,
                        const std::string& device);

}  // namespace hoodie::app
