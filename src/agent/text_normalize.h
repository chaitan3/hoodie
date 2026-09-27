#pragma once

#include <string>

namespace hoodie::agent {

// Convert an integer to English words (e.g. 42 -> "forty two").
std::string number_to_words(long long value);

// Convert a decimal number to words (e.g. 2.5 -> "two point five").
std::string number_to_words(double value);

// Prepare model output for speech synthesis: expand digits to words, strip
// markdown/URLs/code fences, and collapse whitespace. Mirrors the intent of the
// original Python (inflect + ad-hoc cleanup) but handles markdown too.
std::string normalize_for_tts(const std::string& text);

}  // namespace hoodie::agent
