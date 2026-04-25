/*
 * test_lset.c — label_set bit operations and the SplitMix64 hash.
 *
 * The label_set is a 512-bit bitvector (8 × uint64_t). lset_set,
 * lset_test, lset_clear, lset_eq are inline in ast.h; lset_hash is
 * an XOR-fold + SplitMix64 finalize. Boundary bits to exercise:
 * 0, 63, 64, 127, 128, 511 — i.e. word boundaries and both ends.
 */
#include "test.h"
#include "../ast.h"

static void lset_clear_zeroes_all_words(void) {
    label_set s;
    /* Force a non-zero starting state so clear is observable. */
    for (int i = 0; i < LABEL_WORDS; i++) s.w[i] = 0xdeadbeefcafebabeULL;
    lset_clear(&s);
    for (int i = 0; i < LABEL_WORDS; i++)
        ASSERT_EQ_U64(s.w[i], 0ULL);
}

static void lset_set_and_test_low_bit(void) {
    label_set s; lset_clear(&s);
    lset_set(&s, 1);
    ASSERT_EQ_INT(lset_test(&s, 1), 1);
    ASSERT_EQ_INT(lset_test(&s, 0), 0);
    ASSERT_EQ_INT(lset_test(&s, 2), 0);
}

static void lset_set_word_boundaries(void) {
    /* Bits 63, 64, 127, 128 — every word edge. */
    label_set s; lset_clear(&s);
    lset_set(&s, 63);
    lset_set(&s, 64);
    lset_set(&s, 127);
    lset_set(&s, 128);
    ASSERT_EQ_INT(lset_test(&s, 63),  1);
    ASSERT_EQ_INT(lset_test(&s, 64),  1);
    ASSERT_EQ_INT(lset_test(&s, 127), 1);
    ASSERT_EQ_INT(lset_test(&s, 128), 1);
    ASSERT_EQ_INT(lset_test(&s, 62),  0);
    ASSERT_EQ_INT(lset_test(&s, 65),  0);
    ASSERT_EQ_INT(lset_test(&s, 126), 0);
    ASSERT_EQ_INT(lset_test(&s, 129), 0);
}

static void lset_set_max_label_id(void) {
    label_set s; lset_clear(&s);
    lset_set(&s, MAX_LABEL_ID);
    ASSERT_EQ_INT(lset_test(&s, MAX_LABEL_ID), 1);
    /* Top word should have exactly one bit set. */
    ASSERT_EQ_U64(s.w[LABEL_WORDS - 1], (uint64_t)1 << 63);
    /* Other words should still be zero. */
    for (int i = 0; i < LABEL_WORDS - 1; i++)
        ASSERT_EQ_U64(s.w[i], 0ULL);
}

static void lset_eq_empty_sets(void) {
    label_set a, b;
    lset_clear(&a); lset_clear(&b);
    ASSERT_EQ_INT(lset_eq(&a, &b), 1);
}

static void lset_eq_same_bits(void) {
    label_set a, b;
    lset_clear(&a); lset_clear(&b);
    lset_set(&a, 1); lset_set(&a, 200);
    lset_set(&b, 200); lset_set(&b, 1);   /* order-independent */
    ASSERT_EQ_INT(lset_eq(&a, &b), 1);
}

static void lset_eq_different_bits(void) {
    label_set a, b;
    lset_clear(&a); lset_clear(&b);
    lset_set(&a, 1);
    lset_set(&b, 2);
    ASSERT_EQ_INT(lset_eq(&a, &b), 0);
}

static void lset_eq_same_word_diff_bit(void) {
    /* Differs only in bit 0 of a non-first word — confirms the loop
     * actually walks every word, not just word[0]. */
    label_set a, b;
    lset_clear(&a); lset_clear(&b);
    lset_set(&a, 64);
    lset_set(&b, 65);
    ASSERT_EQ_INT(lset_eq(&a, &b), 0);
}

/* ---------- hashing ------------------------------------------------ */

static void hash_deterministic(void) {
    label_set a, b;
    lset_clear(&a); lset_clear(&b);
    lset_set(&a, 1); lset_set(&a, 64); lset_set(&a, 200);
    lset_set(&b, 200); lset_set(&b, 1); lset_set(&b, 64);
    ASSERT_EQ_U64(lset_hash(&a), lset_hash(&b));
}

static void hash_distinguishes_bitsets(void) {
    /* Not a strict guarantee (any hash can collide), but for these
     * specific small sets SplitMix64 produces distinct outputs. */
    label_set a, b;
    lset_clear(&a); lset_clear(&b);
    lset_set(&a, 1);
    lset_set(&b, 2);
    ASSERT_FALSE(lset_hash(&a) == lset_hash(&b));
}

static void finalize_64_zero_is_zero(void) {
    /* SplitMix64 finalizer maps 0 → 0 (multiply by anything is 0,
     * xor-shift of 0 is 0). Documenting this so a future change to
     * the mixer doesn't silently shift the empty-EID hash. */
    ASSERT_EQ_U64(finalize_64(0), 0ULL);
}

static void finalize_64_known_value(void) {
    /* Pin one known input/output pair so the finalizer can't drift
     * silently. SplitMix64 finalizer of 1 is a fixed value. */
    /* x = 1
     * step1: (1 ^ (1>>30)) * 0xbf58476d1ce4e5b9 = 0xbf58476d1ce4e5b9
     * step2: (x ^ (x>>27)) * 0x94d049bb133111eb
     * step3: x ^ (x>>31)
     * The exact value here is computed once and pinned — any future
     * deviation means the EID hash space changed. */
    uint64_t got = finalize_64(1ULL);
    /* Recompute reference inline to keep this self-contained. */
    uint64_t x = 1ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x =  x ^ (x >> 31);
    ASSERT_EQ_U64(got, x);
}

void suite_lset(void) {
    RUN(lset_clear_zeroes_all_words);
    RUN(lset_set_and_test_low_bit);
    RUN(lset_set_word_boundaries);
    RUN(lset_set_max_label_id);
    RUN(lset_eq_empty_sets);
    RUN(lset_eq_same_bits);
    RUN(lset_eq_different_bits);
    RUN(lset_eq_same_word_diff_bit);
    RUN(hash_deterministic);
    RUN(hash_distinguishes_bitsets);
    RUN(finalize_64_zero_is_zero);
    RUN(finalize_64_known_value);
}
