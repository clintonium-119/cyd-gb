#pragma once
// Frame queue — the hand-off between the core that emulates and scales one
// block of rows at a time and the core that pushes those rows over DMA. Two
// slots: while the consumer transfers one, the producer fills the other, which
// is the entire point of the split.
//
// This module is bookkeeping and nothing else. It owns no pixels — a slot is
// an index, and the wrapper attaches a real DMA-capable buffer to each index.
// It knows no FreeRTOS — every call returns immediately, and the caller
// decides whether a FULL or EMPTY answer means yield, spin or drop.
//
// Slot lifecycle, advanced strictly in ring order:
//
//   FREE -> (acquire) PRODUCER -> (commit) COMMITTED -> (pop) CONSUMER
//        -> (release) FREE
//
// Ordering is enforced at commit time, and that is what makes "frames never
// interleave rows" a provable property rather than a hoped-for one: a commit
// must be the exact successor of the previous commit, and pops are FIFO, so
// the consumer can only ever see frame N's blocks in order and exhausted
// before frame N+1's first block appears.
//
// Pause is the menu handover. From the moment framequeue_pause() returns,
// acquire answers PAUSED instead of FULL, so a producer waiting on a full
// queue is released rather than spinning against a consumer that has stopped
// — the deadlock this design has to exclude. Work already committed still
// pops, and framequeue_drained() reports when the consumer has finished it and
// the bus is safe to take. Pause is meant to be called from the producer's own
// context at a frame boundary; drained() therefore ignores a producer-owned
// slot, because in that calling pattern there cannot be one.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation, no globals: every byte of
// state lives in the caller's framequeue_t, and two queues coexist without
// interacting.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Two is the whole design: one slot in flight, one being filled. */
#define FRAMEQUEUE_SLOTS 2

enum framequeue_result_e {
    FRAMEQUEUE_OK = 0,
    FRAMEQUEUE_FULL = -1,      /* no free slot; the overflow counter moved  */
    FRAMEQUEUE_EMPTY = -2,     /* nothing committed to pop                  */
    FRAMEQUEUE_PAUSED = -3,    /* a pause is in force; produce nothing      */
    FRAMEQUEUE_ERR_ORDER = -4, /* commit is not the successor of the last   */
    FRAMEQUEUE_ERR_STATE = -5, /* wrong slot, wrong owner, unusable pointer */
};

enum framequeue_slot_state_e {
    FRAMEQUEUE_SLOT_FREE = 0,
    FRAMEQUEUE_SLOT_PRODUCER = 1,
    FRAMEQUEUE_SLOT_COMMITTED = 2,
    FRAMEQUEUE_SLOT_CONSUMER = 3,
};

/*
 * What the producer states about a block when it commits it. The consumer
 * needs exactly this much to bracket a frame: block_idx 0 opens the address
 * window, last_in_frame closes it.
 */
typedef struct framequeue_meta_s {
    uint16_t frame_seq;    /* frame this block belongs to                  */
    uint8_t block_idx;     /* 0-based position within that frame           */
    uint8_t last_in_frame; /* non-zero on the frame's final block          */
} framequeue_meta_t;

/*
 * Caller-owned state. Opaque in practice — every field is maintained by the
 * functions below, and the counters have getters — but declared here so the
 * wrapper can place an instance in static storage.
 */
typedef struct framequeue_s {
    uint8_t state[FRAMEQUEUE_SLOTS];        /* framequeue_slot_state_e     */
    framequeue_meta_t meta[FRAMEQUEUE_SLOTS];
    uint8_t blocks_per_frame;
    uint8_t acquire_idx; /* next slot the producer may acquire  */
    uint8_t commit_idx;  /* next slot the producer must commit  */
    uint8_t pop_idx;     /* next slot the consumer may pop      */
    uint8_t release_idx; /* next slot the consumer must release */
    uint8_t depth;       /* slots not FREE right now            */
    uint8_t max_depth;   /* peak of depth since init            */
    uint32_t overflows;  /* acquire calls answered FULL         */
    bool paused;
    bool have_prev;             /* a commit has happened since init/resume */
    framequeue_meta_t prev;     /* metadata of that commit                 */
} framequeue_t;

/*
 * Reset `fq` to empty, unpaused, with counters at zero, for a frame of exactly
 * `blocks_per_frame` blocks. That count is not decoration: commit uses it to
 * insist that last_in_frame marks the frame's final block and no other.
 * Returns FRAMEQUEUE_ERR_STATE for a NULL queue or a zero block count.
 */
int framequeue_init(framequeue_t* fq, uint8_t blocks_per_frame);

/*
 * Producer: take the next slot in ring order. Writes its index to `slot` and
 * returns FRAMEQUEUE_OK, or answers FRAMEQUEUE_PAUSED while a pause is in
 * force, or FRAMEQUEUE_FULL when the slot is still in use — the FULL case also
 * bumps the overflow counter and changes nothing else. PAUSED takes precedence
 * over FULL, which is what releases a producer that is spinning on a full
 * queue when the menu asks for the bus.
 */
int framequeue_acquire(framequeue_t* fq, int* slot);

/*
 * Producer: hand `slot` to the consumer with the metadata in `meta`. The slot
 * must be the one acquired least recently and still producer-owned, or the
 * call is FRAMEQUEUE_ERR_STATE.
 *
 * FRAMEQUEUE_ERR_ORDER rejects any commit that is not the exact successor of
 * the previous one: within a frame, the same frame_seq and the next block_idx;
 * after a last_in_frame block, block_idx 0 of a different frame_seq (the
 * sequence may jump, because frameskip drops whole frames, but it may not
 * repeat). last_in_frame must be set on block blocks_per_frame - 1 and clear
 * everywhere else. A rejected commit leaves the slot producer-owned and the
 * stored metadata untouched, so the caller may retry with corrected metadata.
 */
int framequeue_commit(framequeue_t* fq, int slot,
                      const framequeue_meta_t* meta);

/*
 * Consumer: take the oldest committed slot. Writes its index to `slot` and, if
 * `meta` is non-NULL, a copy of its metadata. FRAMEQUEUE_EMPTY when nothing is
 * committed — including while a pause drains, so the consumer's loop needs no
 * pause-specific branch.
 */
int framequeue_pop(framequeue_t* fq, int* slot, framequeue_meta_t* meta);

/*
 * Consumer: return `slot` to the free pool once its buffer has been fully
 * transferred. Slots must be released in the order they were popped;
 * anything else is FRAMEQUEUE_ERR_STATE.
 */
int framequeue_release(framequeue_t* fq, int slot);

/*
 * Ask the producer to stop. Takes effect on its next acquire; nothing already
 * committed is discarded.
 */
void framequeue_pause(framequeue_t* fq);

/*
 * Let the producer run again. The commit sequence restarts: the first commit
 * after a resume must be block 0, of any frame, exactly as after init. A pause
 * can land mid-frame, and the partial frame it left behind must not constrain
 * what comes next.
 */
void framequeue_resume(framequeue_t* fq);

/*
 * True when no slot is committed or consumer-owned — the consumer has finished
 * everything the producer handed over, and the display bus is free. Pair it
 * with framequeue_pause(): pause, then wait for drained, then take the bus.
 */
bool framequeue_drained(const framequeue_t* fq);

/* Acquire calls answered FULL since init — the producer stalled behind the
 * display, which is the number that says whether the split is paying off. */
uint32_t framequeue_overflows(const framequeue_t* fq);

/* Peak number of slots in use at once since init. Reaching FRAMEQUEUE_SLOTS
 * means the pipeline genuinely overlapped; staying at 1 means it never did. */
uint8_t framequeue_max_depth(const framequeue_t* fq);

#ifdef __cplusplus
}
#endif
