// Phase 2d — direction scoring against a target phrase.
//
// Compares the platform-pushed transcript text against
// config.target_phrase and emits a normalized similarity in [0, 1].
// Phase 2's pick is a byte-level Levenshtein ratio over normalized
// text (lowercased, punctuation stripped, whitespace collapsed).
//
// Algorithm choice rationale:
//   - Deterministic and language-agnostic (no per-language stemmer).
//   - O(n*m) time, OK for the short phrases this is designed to
//     compare (typically <= 200 chars on each side).
//   - Captures both insertions and deletions — token-set similarity
//     would miss filler words; pure substring would miss small typos.
//   - UTF-8: we operate on bytes after normalization. ASCII is fine.
//     Non-ASCII (accented chars, ideographs) survive as multi-byte
//     sequences; matched characters still count, but a single
//     codepoint diff may register as 2-3 byte diffs. Acceptable
//     trade-off for the early phase; Unicode-aware diff lands when
//     we have multilingual ASR.
//
// See whitepaper §6.3 (direction) and the spec's Transcript shape.

#ifndef SENTLOG_INTERNAL_TEXT_MATCH_H
#define SENTLOG_INTERNAL_TEXT_MATCH_H

#include <string>
#include <string_view>

namespace sentlog::internal {

/// Lowercase the input, strip ASCII punctuation, collapse internal
/// whitespace to single spaces, trim leading/trailing whitespace.
std::string normalize_for_match(std::string_view s);

/// Byte-level Levenshtein edit distance.
size_t levenshtein_distance(std::string_view a, std::string_view b);

/// Normalized similarity in [0, 1]. 1.0 = identical (after
/// normalization), 0.0 = no shared bytes. Empty + empty = 1.0
/// (vacuously identical). Empty + non-empty = 0.0.
float target_match_ratio(std::string_view transcript_text,
                         std::string_view target_phrase);

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_TEXT_MATCH_H
