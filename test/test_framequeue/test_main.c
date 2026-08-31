#include <unity.h>

#include <string.h>

#include "render/framequeue.h"

/*
 * Frame-queue suite. The property this exists to pin is the workstream's own
 * exit criterion — frames never interleave rows — and the queue makes that
 * provable sequentially rather than by luck: commits must be exact successors
 * and pops are FIFO, so a consumer can only ever see frame N's blocks in
 * order and exhausted before frame N+1 begins. Threads would add nothing here.
 * The two-task wrapper's interleavings reduce to these call orders, because
 * the producer and the consumer touch disjoint halves of the API and the
 * queue never blocks.
 *
 * BLOCKS_PER_FRAME is the shipping 24/16 geometry: 144 source lines at two
 * lines per block.
 */

#define BLOCKS_PER_FRAME 72
#define LAST_BLOCK (BLOCKS_PER_FRAME - 1)

static framequeue_t q;

/* What the consumer actually saw, in the order it saw it. */
static framequeue_meta_t seen[3 * BLOCKS_PER_FRAME];
static unsigned n_seen;

static framequeue_meta_t meta_of(uint16_t frame, uint8_t idx)
{
    framequeue_meta_t m;

    m.frame_seq = frame;
    m.block_idx = idx;
    m.last_in_frame = (idx == LAST_BLOCK) ? 1 : 0;
    return m;
}

/* Pop one block and return its slot to the pool, recording the metadata. */
static void consume_one(framequeue_t* fq)
{
    int slot = -1;
    framequeue_meta_t m;

    memset(&m, 0, sizeof m);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(fq, &slot, &m));
    TEST_ASSERT_TRUE(slot >= 0 && slot < FRAMEQUEUE_SLOTS);
    seen[n_seen++] = m;
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_release(fq, slot));
}

/*
 * Produce one block, draining a slot first if the queue is full. This is the
 * producer's real behaviour with the wait replaced by a synchronous consume —
 * exactly the backpressure the two-slot queue is for.
 */
static void produce_one(framequeue_t* fq, uint16_t frame, uint8_t idx)
{
    framequeue_meta_t m = meta_of(frame, idx);
    int slot = -1;
    int r;

    r = framequeue_acquire(fq, &slot);
    if (r == FRAMEQUEUE_FULL) {
        consume_one(fq);
        r = framequeue_acquire(fq, &slot);
    }
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, r);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_commit(fq, slot, &m));
}

/* Acquire and commit without consuming — used to force the queue full. */
static int commit_next(framequeue_t* fq, uint16_t frame, uint8_t idx)
{
    framequeue_meta_t m = meta_of(frame, idx);
    int slot = -1;

    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_acquire(fq, &slot));
    return framequeue_commit(fq, slot, &m);
}

void setUp(void)
{
    memset(&q, 0, sizeof q);
    memset(seen, 0, sizeof seen);
    n_seen = 0;
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK,
                          framequeue_init(&q, BLOCKS_PER_FRAME));
}

void tearDown(void)
{
}

static void test_blocks_pop_in_commit_order_with_intact_metadata(void)
{
    unsigned i;

    for (i = 0; i < 6u; i++) {
        produce_one(&q, 0, (uint8_t)i);
    }
    while (!framequeue_drained(&q)) {
        consume_one(&q);
    }

    TEST_ASSERT_EQUAL_UINT(6u, n_seen);
    for (i = 0; i < n_seen; i++) {
        TEST_ASSERT_EQUAL_UINT16(0u, seen[i].frame_seq);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, seen[i].block_idx);
        TEST_ASSERT_EQUAL_UINT8(0u, seen[i].last_in_frame);
    }
}

static void test_a_new_frame_cannot_start_before_the_old_one_ends(void)
{
    framequeue_meta_t early = meta_of(1u, 0u);
    framequeue_meta_t late;
    int slot = -1;
    unsigned i;

    /* Frame 0 up to but not including its last block. */
    for (i = 0; i < LAST_BLOCK; i++) {
        produce_one(&q, 0, (uint8_t)i);
    }
    while (!framequeue_drained(&q)) {
        consume_one(&q);
    }

    /* Frame 1 block 0 while frame 0 is unfinished: rejected, and the slot is
     * still the producer's to reuse. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_acquire(&q, &slot));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_ORDER,
                          framequeue_commit(&q, slot, &early));
    TEST_ASSERT_TRUE(framequeue_drained(&q));

    /* Frame 0's last block goes in that same slot, then frame 1 opens. */
    late = meta_of(0u, LAST_BLOCK);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_commit(&q, slot, &late));
    consume_one(&q);
    TEST_ASSERT_EQUAL_UINT8(1u, seen[n_seen - 1u].last_in_frame);

    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_acquire(&q, &slot));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_commit(&q, slot, &early));
}

static void test_two_whole_frames_pop_in_exact_order(void)
{
    unsigned i;

    for (i = 0; i < BLOCKS_PER_FRAME; i++) {
        produce_one(&q, 0, (uint8_t)i);
    }
    for (i = 0; i < BLOCKS_PER_FRAME; i++) {
        produce_one(&q, 1, (uint8_t)i);
    }
    while (!framequeue_drained(&q)) {
        consume_one(&q);
    }

    TEST_ASSERT_EQUAL_UINT(2u * BLOCKS_PER_FRAME, n_seen);
    for (i = 0; i < n_seen; i++) {
        uint16_t frame = (uint16_t)(i / BLOCKS_PER_FRAME);
        uint8_t idx = (uint8_t)(i % BLOCKS_PER_FRAME);

        TEST_ASSERT_EQUAL_UINT16(frame, seen[i].frame_seq);
        TEST_ASSERT_EQUAL_UINT8(idx, seen[i].block_idx);
        TEST_ASSERT_EQUAL_UINT8(idx == LAST_BLOCK ? 1u : 0u,
                                seen[i].last_in_frame);
    }
    /* The pipeline genuinely overlapped rather than running one at a time. */
    TEST_ASSERT_EQUAL_UINT8(FRAMEQUEUE_SLOTS, framequeue_max_depth(&q));
}

static void test_a_full_queue_reports_full_and_overwrites_nothing(void)
{
    int slot = -1;
    framequeue_meta_t m;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, commit_next(&q, 0, 0));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, commit_next(&q, 0, 1));

    for (i = 0; i < 3u; i++) {
        TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_FULL, framequeue_acquire(&q, &slot));
    }
    TEST_ASSERT_EQUAL_UINT32(3u, framequeue_overflows(&q));

    /* Both committed blocks survived the refused acquires intact. */
    memset(&m, 0, sizeof m);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(&q, &slot, &m));
    TEST_ASSERT_EQUAL_UINT8(0u, m.block_idx);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_release(&q, slot));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(&q, &slot, &m));
    TEST_ASSERT_EQUAL_UINT8(1u, m.block_idx);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_release(&q, slot));

    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_EMPTY, framequeue_pop(&q, &slot, &m));
}

static void test_frame_seq_may_jump_but_block_idx_must_restart(void)
{
    framequeue_meta_t m;
    int slot = -1;
    unsigned i;

    /* A whole frame 0, so the sequence is at a frame boundary. */
    for (i = 0; i < BLOCKS_PER_FRAME; i++) {
        produce_one(&q, 0, (uint8_t)i);
    }
    while (!framequeue_drained(&q)) {
        consume_one(&q);
    }

    /* Frameskip dropped frames 1 and 2; frame 3 is fine — but only from
     * block 0. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_acquire(&q, &slot));
    m = meta_of(3u, 1u);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_ORDER,
                          framequeue_commit(&q, slot, &m));
    m = meta_of(3u, 0u);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_commit(&q, slot, &m));

    /* Re-using the frame just finished is not a jump, it is a repeat: the
     * consumer would reopen a window it has already closed. */
    consume_one(&q);
    for (i = 1u; i < BLOCKS_PER_FRAME; i++) {
        produce_one(&q, 3u, (uint8_t)i);
    }
    while (!framequeue_drained(&q)) {
        consume_one(&q);
    }
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_acquire(&q, &slot));
    m = meta_of(3u, 0u);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_ORDER,
                          framequeue_commit(&q, slot, &m));
}

static void test_last_in_frame_must_agree_with_the_block_count(void)
{
    framequeue_meta_t m;
    int slot = -1;

    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_acquire(&q, &slot));

    /* Ending the frame early. */
    m = meta_of(0u, 0u);
    m.last_in_frame = 1u;
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_ORDER,
                          framequeue_commit(&q, slot, &m));

    /* Past the end of the frame. */
    m = meta_of(0u, (uint8_t)BLOCKS_PER_FRAME);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_ORDER,
                          framequeue_commit(&q, slot, &m));

    /* The honest version of the same block goes through. */
    m = meta_of(0u, 0u);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_commit(&q, slot, &m));
}

static void test_pause_releases_the_producer_and_drains_the_consumer(void)
{
    int slot = -1;
    framequeue_meta_t m;

    /* Fill both slots so the producer is in the state a naive pause would
     * deadlock: waiting on FULL. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, commit_next(&q, 0, 0));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, commit_next(&q, 0, 1));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_FULL, framequeue_acquire(&q, &slot));

    framequeue_pause(&q);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_PAUSED, framequeue_acquire(&q, &slot));
    TEST_ASSERT_FALSE(framequeue_drained(&q));

    /* Committed work still pops, and only the last release drains. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(&q, &slot, &m));
    TEST_ASSERT_EQUAL_UINT8(0u, m.block_idx);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_release(&q, slot));
    TEST_ASSERT_FALSE(framequeue_drained(&q));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(&q, &slot, &m));
    TEST_ASSERT_EQUAL_UINT8(1u, m.block_idx);
    TEST_ASSERT_FALSE(framequeue_drained(&q));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_release(&q, slot));
    TEST_ASSERT_TRUE(framequeue_drained(&q));

    /* Still paused after draining. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_PAUSED, framequeue_acquire(&q, &slot));

    /* Resume restores acquire, and the interrupted frame does not constrain
     * what comes next: the first commit is block 0 of any frame. */
    framequeue_resume(&q);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, commit_next(&q, 9u, 0u));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(&q, &slot, &m));
    TEST_ASSERT_EQUAL_UINT16(9u, m.frame_seq);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_release(&q, slot));
}

static void test_slots_are_released_only_by_their_consumer(void)
{
    int slot = -1;
    int popped = -1;
    framequeue_meta_t m;

    /* Free, then producer-owned: neither is releasable. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_STATE, framequeue_release(&q, 0));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_acquire(&q, &slot));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_STATE, framequeue_release(&q, slot));

    /* Committed but not yet popped: still not releasable. */
    m = meta_of(0u, 0u);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_commit(&q, slot, &m));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_STATE, framequeue_release(&q, slot));

    /* Popped: releasable exactly once. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(&q, &popped, &m));
    TEST_ASSERT_EQUAL_INT(slot, popped);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_release(&q, popped));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_STATE, framequeue_release(&q, popped));

    /* Out-of-range indices are rejected, not indexed. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_STATE, framequeue_release(&q, -1));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_STATE,
                          framequeue_release(&q, FRAMEQUEUE_SLOTS));
}

static void test_two_queues_do_not_share_state(void)
{
    framequeue_t other;
    int slot = -1;
    framequeue_meta_t m;
    unsigned i;

    memset(&other, 0, sizeof other);
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK,
                          framequeue_init(&other, BLOCKS_PER_FRAME));

    for (i = 0; i < 5u; i++) {
        produce_one(&q, 0, (uint8_t)i);
    }
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_FULL, framequeue_acquire(&q, &slot));
    framequeue_pause(&q);

    /* The other queue is untouched by all of that. */
    TEST_ASSERT_TRUE(framequeue_drained(&other));
    TEST_ASSERT_EQUAL_UINT32(0u, framequeue_overflows(&other));
    TEST_ASSERT_EQUAL_UINT8(0u, framequeue_max_depth(&other));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_EMPTY, framequeue_pop(&other, &slot, &m));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, commit_next(&other, 0, 0));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(&other, &slot, &m));
    TEST_ASSERT_EQUAL_UINT8(0u, m.block_idx);

    /* And the paused queue is still paused. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_PAUSED, framequeue_acquire(&q, &slot));
}

static void test_counters_report_depth_and_stalls(void)
{
    int slot = -1;
    framequeue_meta_t m;

    TEST_ASSERT_EQUAL_UINT8(0u, framequeue_max_depth(&q));
    TEST_ASSERT_EQUAL_UINT32(0u, framequeue_overflows(&q));

    /* produce, produce, pop: depth peaks at both slots in use. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, commit_next(&q, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(1u, framequeue_max_depth(&q));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, commit_next(&q, 0, 1));
    TEST_ASSERT_EQUAL_UINT8(2u, framequeue_max_depth(&q));

    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_FULL, framequeue_acquire(&q, &slot));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_FULL, framequeue_acquire(&q, &slot));
    TEST_ASSERT_EQUAL_UINT32(2u, framequeue_overflows(&q));

    /* Releasing lowers the live depth but never the recorded peak. */
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_pop(&q, &slot, &m));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_release(&q, slot));
    TEST_ASSERT_EQUAL_UINT8(2u, framequeue_max_depth(&q));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_OK, framequeue_acquire(&q, &slot));
    TEST_ASSERT_EQUAL_UINT8(2u, framequeue_max_depth(&q));
    TEST_ASSERT_EQUAL_UINT32(2u, framequeue_overflows(&q));
}

static void test_init_rejects_an_impossible_frame(void)
{
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_STATE, framequeue_init(&q, 0));
    TEST_ASSERT_EQUAL_INT(FRAMEQUEUE_ERR_STATE, framequeue_init(NULL, 72));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blocks_pop_in_commit_order_with_intact_metadata);
    RUN_TEST(test_a_new_frame_cannot_start_before_the_old_one_ends);
    RUN_TEST(test_two_whole_frames_pop_in_exact_order);
    RUN_TEST(test_a_full_queue_reports_full_and_overwrites_nothing);
    RUN_TEST(test_frame_seq_may_jump_but_block_idx_must_restart);
    RUN_TEST(test_last_in_frame_must_agree_with_the_block_count);
    RUN_TEST(test_pause_releases_the_producer_and_drains_the_consumer);
    RUN_TEST(test_slots_are_released_only_by_their_consumer);
    RUN_TEST(test_two_queues_do_not_share_state);
    RUN_TEST(test_counters_report_depth_and_stalls);
    RUN_TEST(test_init_rejects_an_impossible_frame);
    return UNITY_END();
}
