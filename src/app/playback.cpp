#include "app/playback.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "audio/player.h"
#include "audio/wav.h"
#include "util/process.h"

namespace hoodie::app {
namespace {

namespace fs = std::filesystem;

}  // namespace

void play_frame(const audio::AudioFrame& frame, const std::string& device) {
  audio::AudioPlayer::Options player_options;
  player_options.device = device;
  player_options.sample_rate = 0;  // device default
  player_options.channels = 1;

  audio::AudioPlayer player(player_options);
  player.start();
  player.enqueue(frame);
  const bool drained = player.wait_drained();
  player.stop();
  if (!drained) spdlog::warn("playback did not drain within timeout");
}

void play_or_write(const audio::AudioFrame& frame, const std::string& out_file,
                   const std::string& device) {
  if (!out_file.empty()) {
    audio::write_audio_file(out_file, frame);
    spdlog::info("wrote {} ({:.2f} s, {} Hz, {} ch)", out_file, frame.duration_seconds(),
                 frame.sample_rate, frame.channels);
    return;
  }
  play_frame(frame, device);
}

void play_music_request(const agent::MusicRequest& music, const std::string& out_file,
                        const std::string& device) {
  if (music.id.empty()) return;

  std::error_code error;
  const fs::path directory = fs::temp_directory_path(error) / "hoodie_music";
  fs::create_directories(directory, error);
  const fs::path base = directory / "out";

  const util::ProcessResult result = util::run_process(
      {"yt-dlp", "--force-overwrite", "-f", "ba", "-x", "--audio-format", "vorbis", "-o",
       base.string(), music.id});
  if (result.exit_code != 0) {
    spdlog::warn("music: download failed (exit {}): {}", result.exit_code, result.output);
    return;
  }

  fs::path ogg = base;
  ogg += ".ogg";
  if (!fs::exists(ogg, error)) {
    spdlog::warn("music: expected {} but it was not produced", ogg.string());
    return;
  }
  spdlog::info("music: playing '{}' from {}", music.title, ogg.string());
  play_or_write(audio::read_audio_file(ogg.string()), out_file, device);
}

}  // namespace hoodie::app
