#include "internal/text_match.h"

#include <gtest/gtest.h>

using sentlog::internal::normalize_for_match;
using sentlog::internal::levenshtein_distance;
using sentlog::internal::target_match_ratio;

// --- normalize_for_match ---

TEST(NormalizeForMatchTest, LowercasesAscii) {
    EXPECT_EQ(normalize_for_match("Hello WORLD"), "hello world");
}

TEST(NormalizeForMatchTest, StripsLeadingTrailingWhitespace) {
    EXPECT_EQ(normalize_for_match("  hello  "), "hello");
}

TEST(NormalizeForMatchTest, CollapsesInternalWhitespace) {
    EXPECT_EQ(normalize_for_match("hello   world\t\n  there"),
              "hello world there");
}

TEST(NormalizeForMatchTest, StripsPunctuationAsSoftBoundary) {
    EXPECT_EQ(normalize_for_match("Hello, world!"), "hello world");
    EXPECT_EQ(normalize_for_match("don't"),         "don t");
}

TEST(NormalizeForMatchTest, EmptyInputReturnsEmpty) {
    EXPECT_EQ(normalize_for_match(""),          "");
    EXPECT_EQ(normalize_for_match("   ..."),    "");
}

// --- levenshtein_distance ---

TEST(LevenshteinDistanceTest, IdenticalIsZero) {
    EXPECT_EQ(levenshtein_distance("hello", "hello"), 0u);
}

TEST(LevenshteinDistanceTest, EmptySideEqualsOtherLength) {
    EXPECT_EQ(levenshtein_distance("",      "hello"), 5u);
    EXPECT_EQ(levenshtein_distance("hello", ""),      5u);
}

TEST(LevenshteinDistanceTest, SingleSubstitution) {
    EXPECT_EQ(levenshtein_distance("kitten", "sitten"), 1u);
}

TEST(LevenshteinDistanceTest, ClassicKittenSitting) {
    // Canonical example: kitten → sitting via 3 edits
    // (k→s, e→i, +g).
    EXPECT_EQ(levenshtein_distance("kitten", "sitting"), 3u);
}

// --- target_match_ratio ---

TEST(TargetMatchRatioTest, ExactMatchIsOne) {
    EXPECT_FLOAT_EQ(target_match_ratio("hello world", "hello world"), 1.0f);
}

TEST(TargetMatchRatioTest, MatchAfterNormalization) {
    EXPECT_FLOAT_EQ(target_match_ratio("Hello, World!", "hello world"),
                    1.0f);
}

TEST(TargetMatchRatioTest, EmptyTargetReturnsZero) {
    EXPECT_FLOAT_EQ(target_match_ratio("hello", ""), 0.0f);
}

TEST(TargetMatchRatioTest, EmptyTranscriptReturnsZero) {
    EXPECT_FLOAT_EQ(target_match_ratio("", "hello"), 0.0f);
}

TEST(TargetMatchRatioTest, BothEmptyReturnsOne) {
    // Vacuously identical — a degenerate case but defined so callers
    // don't have to special-case it.
    EXPECT_FLOAT_EQ(target_match_ratio("", ""), 1.0f);
}

TEST(TargetMatchRatioTest, SmallTypoScoresHigh) {
    // "helo world" vs "hello world" — one char deletion in 11 chars.
    const float r = target_match_ratio("helo world", "hello world");
    EXPECT_GT(r, 0.85f);
    EXPECT_LT(r, 1.0f);
}

TEST(TargetMatchRatioTest, NoOverlapScoresLow) {
    const float r = target_match_ratio("the cat sat on the mat",
                                       "completely unrelated text here");
    EXPECT_LT(r, 0.5f);
}

TEST(TargetMatchRatioTest, ResultIsInUnitInterval) {
    const float r = target_match_ratio("anything at all",
                                       "totally different content x");
    EXPECT_GE(r, 0.0f);
    EXPECT_LE(r, 1.0f);
}

TEST(TargetMatchRatioTest, PunctuationDoesNotPenalize) {
    EXPECT_FLOAT_EQ(
        target_match_ratio("Please confirm your name.",
                           "please confirm your name"),
        1.0f);
}
