#include "internal/text_match.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace sentlog::internal {

std::string normalize_for_match(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool last_was_space = true;  // skip leading whitespace
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c)) {
            if (!last_was_space) {
                out += ' ';
                last_was_space = true;
            }
            continue;
        }
        if (std::ispunct(c)) {
            // Treat punctuation as a soft word boundary, like whitespace.
            if (!last_was_space) {
                out += ' ';
                last_was_space = true;
            }
            continue;
        }
        out += static_cast<char>(std::tolower(c));
        last_was_space = false;
    }
    // Trim trailing space.
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

size_t levenshtein_distance(std::string_view a, std::string_view b) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0) return m;
    if (m == 0) return n;

    // Two-row rolling buffer — O(min(n,m)) space.
    std::vector<size_t> prev(m + 1);
    std::vector<size_t> curr(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;

    for (size_t i = 1; i <= n; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0u : 1u;
            const size_t del = prev[j]     + 1;
            const size_t ins = curr[j - 1] + 1;
            const size_t sub = prev[j - 1] + cost;
            curr[j] = std::min({del, ins, sub});
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

float target_match_ratio(std::string_view transcript_text,
                         std::string_view target_phrase) {
    const std::string a = normalize_for_match(transcript_text);
    const std::string b = normalize_for_match(target_phrase);
    if (a.empty() && b.empty()) return 1.0f;          // both empty → trivially match
    if (a.empty() || b.empty()) return 0.0f;          // one side empty → no match

    const size_t dist = levenshtein_distance(a, b);
    const size_t denom = std::max(a.size(), b.size());
    // Guard against division by zero (denom can't actually be 0 here
    // because both strings are non-empty, but be defensive).
    if (denom == 0) return 0.0f;

    const float ratio = 1.0f - (static_cast<float>(dist) /
                                static_cast<float>(denom));
    if (ratio < 0.0f) return 0.0f;
    if (ratio > 1.0f) return 1.0f;
    return ratio;
}

}  // namespace sentlog::internal
