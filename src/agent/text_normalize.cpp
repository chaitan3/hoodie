#include "agent/text_normalize.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace hoodie::agent {
namespace {

const char* kOnes[] = {"zero", "one", "two", "three", "four", "five", "six", "seven",
                       "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
                       "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
const char* kTens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
                       "eighty", "ninety"};

std::string under_thousand(int value) {
  std::string out;
  if (value >= 100) {
    out += std::string(kOnes[value / 100]) + " hundred";
    value %= 100;
    if (value > 0) out += " ";
  }
  if (value >= 20) {
    out += kTens[value / 10];
    if (value % 10 != 0) out += std::string(" ") + kOnes[value % 10];
  } else if (value > 0) {
    out += kOnes[value];
  }
  return out;
}

std::string integer_to_words(long long value) {
  if (value == 0) return "zero";
  if (value < 0) return "minus " + integer_to_words(-value);

  static const struct {
    long long scale;
    const char* name;
  } kScales[] = {{1000000000LL, "billion"},
                 {1000000LL, "million"},
                 {1000LL, "thousand"},
                 {1LL, ""}};

  std::string out;
  for (const auto& scale : kScales) {
    if (value >= scale.scale) {
      const long long chunk = value / scale.scale;
      value %= scale.scale;
      if (!out.empty()) out += " ";
      out += under_thousand(static_cast<int>(chunk));
      if (scale.name[0] != '\0') out += std::string(" ") + scale.name;
    }
  }
  return out;
}

std::string collapse_spaces(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool in_space = false;
  for (char c : s) {
    const bool is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
    if (is_space) {
      if (!in_space && !out.empty()) out.push_back(' ');
      in_space = true;
    } else {
      out.push_back(c);
      in_space = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string expand_numbers(const std::string& text) {
  const std::regex pattern(R"((\d+(?:\.\d+)?))");
  std::string out;
  auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
  auto end = std::sregex_iterator();
  std::size_t last = 0;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& match = *it;
    out.append(text, last, static_cast<std::size_t>(match.position()) - last);
    const std::string token = match.str();
    try {
      if (token.find('.') != std::string::npos) {
        out += number_to_words(std::stod(token));
      } else {
        out += number_to_words(std::stoll(token));
      }
    } catch (const std::exception&) {
      out += token;
    }
    last = static_cast<std::size_t>(match.position() + match.length());
  }
  out.append(text, last, std::string::npos);
  return out;
}

}  // namespace

std::string number_to_words(long long value) { return integer_to_words(value); }

std::string number_to_words(double value) {
  const bool negative = value < 0;
  value = std::fabs(value);
  const long long whole = static_cast<long long>(value);
  std::string out = integer_to_words(whole);

  // Up to three fractional digits, trimmed of trailing zeros.
  std::string fraction;
  double frac = value - static_cast<double>(whole);
  for (int i = 0; i < 3 && frac > 1e-9; ++i) {
    frac *= 10.0;
    const int digit = static_cast<int>(frac);
    fraction.push_back(static_cast<char>('0' + digit));
    frac -= digit;
  }
  while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
  if (!fraction.empty()) {
    out += " point";
    for (char digit : fraction) {
      out += std::string(" ") + kOnes[digit - '0'];
    }
  }
  return negative ? "minus " + out : out;
}

std::string normalize_for_tts(const std::string& text) {
  std::string s = text;
  // Markdown links: [label](url) -> label
  s = std::regex_replace(s, std::regex(R"(\[([^\]]*)\]\([^)]*\))"), "$1");
  // Bare URLs
  s = std::regex_replace(s, std::regex(R"((https?://|www\.)\S+)"), "");
  // Fenced and inline code
  s = std::regex_replace(s, std::regex(R"(```[\s\S]*?```)"), " ");
  s = std::regex_replace(s, std::regex(R"(`([^`]*)`)"), "$1");
  // Markdown emphasis/heading/quote markers
  s = std::regex_replace(s, std::regex(R"([*_#>])"), "");
  // List bullets ("- item"), keeping the item text
  s = std::regex_replace(s, std::regex(R"((^|\n)\s*[-+]\s+)"), "$1");
  // Digits to words
  s = expand_numbers(s);
  return collapse_spaces(s);
}

}  // namespace hoodie::agent
