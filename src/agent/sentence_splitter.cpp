#include "agent/sentence_splitter.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace hoodie::agent {
namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

void trim_left(std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    text.clear();
  } else if (begin > 0) {
    text.erase(0, begin);
  }
}

bool is_abbreviation(const std::string& word) {
  std::string lower = word;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static const char* const kAbbreviations[] = {
      "mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs", "etc",
      "e.g", "i.e", "no", "fig", "approx", "dept", "inc", "ltd", "co",
      "corp", "u.s", "u.k", "a.m", "p.m",
  };
  for (const char* candidate : kAbbreviations) {
    if (lower == candidate) return true;
  }
  return false;
}

}  // namespace

SentenceSplitter::SentenceSplitter() : SentenceSplitter(Options{}) {}

SentenceSplitter::SentenceSplitter(Options options) : options_(options) {}

void SentenceSplitter::push(const std::string& text, const Callback& on_chunk) {
  buffer_ += text;
  for (;;) {
    const std::optional<std::size_t> cut = find_cut();
    if (!cut.has_value()) break;
    std::string chunk = trim(buffer_.substr(0, *cut));
    buffer_.erase(0, *cut);
    trim_left(buffer_);
    if (!chunk.empty() && on_chunk) on_chunk(chunk);
  }
}

void SentenceSplitter::flush(const Callback& on_chunk) {
  std::string chunk = trim(buffer_);
  buffer_.clear();
  if (!chunk.empty() && on_chunk) on_chunk(chunk);
}

void SentenceSplitter::reset() { buffer_.clear(); }

std::optional<std::size_t> SentenceSplitter::find_cut() const {
  std::size_t words = 0;
  bool in_word = false;
  std::size_t last_space = std::string::npos;

  for (std::size_t i = 0; i < buffer_.size(); ++i) {
    const char c = buffer_[i];
    if (is_space(c)) {
      in_word = false;
      last_space = i;
    } else if (!in_word) {
      in_word = true;
      ++words;
    }

    const bool at_end_or_space = (i + 1 == buffer_.size()) || is_space(buffer_[i + 1]);

    if (c == '!' || c == '?') {
      if (at_end_or_space) return i + 1;
    } else if (c == '.') {
      if (!at_end_or_space) continue;
      if (i > 0 && buffer_[i - 1] == '.') continue;  // ellipsis / "..." run
      std::size_t begin = i;
      while (begin > 0 && !is_space(buffer_[begin - 1]) && buffer_[begin - 1] != '(' &&
             buffer_[begin - 1] != '"' && buffer_[begin - 1] != '\'') {
        --begin;
      }
      if (is_abbreviation(buffer_.substr(begin, i - begin))) continue;
      return i + 1;
    } else if (c == ',' || c == ';' || c == ':') {
      if (at_end_or_space && words >= options_.min_words) return i + 1;
    }
  }

  if (words >= options_.max_words) {
    if (last_space != std::string::npos) return last_space;
    return buffer_.size();
  }
  return std::nullopt;
}

}  // namespace hoodie::agent
