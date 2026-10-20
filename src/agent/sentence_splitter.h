#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace hoodie::agent {

// Turns a stream of LLM tokens into speakable chunks for TTS.
//
// Flush policy:
//   - always at sentence-ending punctuation (. ! ?), unless it looks like an
//     abbreviation or an ellipsis;
//   - at a clause boundary (, ; :) once at least min_words have accumulated;
//   - forced at max_words so a long unpunctuated sentence cannot stall TTS.
class SentenceSplitter {
 public:
  struct Options {
    std::size_t min_words = 6;
    std::size_t max_words = 12;
  };

  using Callback = std::function<void(const std::string&)>;

  SentenceSplitter();
  explicit SentenceSplitter(Options options);

  void push(const std::string& text, const Callback& on_chunk);
  void flush(const Callback& on_chunk);
  void reset();

 private:
  std::optional<std::size_t> find_cut() const;

  Options options_;
  std::string buffer_;
};

}  // namespace hoodie::agent
