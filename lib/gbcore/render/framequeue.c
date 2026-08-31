#include "render/framequeue.h"

/*
 * Ring successor. FRAMEQUEUE_SLOTS is 2, but the modulo is written out rather
 * than assumed, so the file stays correct if the slot count ever changes.
 */
static uint8_t next_idx(uint8_t idx)
{
    return (uint8_t)((idx + 1u) % FRAMEQUEUE_SLOTS);
}

/*
 * Is `m` the exact successor of the last committed block? Everything the
 * queue promises about frame integrity is decided here.
 */
static bool commit_in_order(const framequeue_t* fq,
                            const framequeue_meta_t* m)
{
    uint8_t last = (uint8_t)(fq->blocks_per_frame - 1u);

    if (m->block_idx > last) {
        return false;
    }
    /* last_in_frame is a statement about position, so it has exactly one
     * correct value for a given block_idx and disagreeing with it is an
     * ordering error, not a matter of taste. */
    if ((m->block_idx == last) != (m->last_in_frame != 0)) {
        return false;
    }
    if (!fq->have_prev || fq->prev.last_in_frame) {
        /* Start of a frame. The sequence number may jump — frameskip drops
         * whole frames — but repeating the frame just finished would let the
         * consumer reopen a window it has already closed. */
        if (m->block_idx != 0) {
            return false;
        }
        if (fq->have_prev && m->frame_seq == fq->prev.frame_seq) {
            return false;
        }
        return true;
    }
    return m->frame_seq == fq->prev.frame_seq &&
           m->block_idx == (uint8_t)(fq->prev.block_idx + 1u);
}

int framequeue_init(framequeue_t* fq, uint8_t blocks_per_frame)
{
    unsigned i;

    if (!fq || blocks_per_frame == 0) {
        return FRAMEQUEUE_ERR_STATE;
    }
    for (i = 0; i < FRAMEQUEUE_SLOTS; i++) {
        fq->state[i] = FRAMEQUEUE_SLOT_FREE;
        fq->meta[i].frame_seq = 0;
        fq->meta[i].block_idx = 0;
        fq->meta[i].last_in_frame = 0;
    }
    fq->blocks_per_frame = blocks_per_frame;
    fq->acquire_idx = 0;
    fq->commit_idx = 0;
    fq->pop_idx = 0;
    fq->release_idx = 0;
    fq->depth = 0;
    fq->max_depth = 0;
    fq->overflows = 0;
    fq->paused = false;
    fq->have_prev = false;
    fq->prev.frame_seq = 0;
    fq->prev.block_idx = 0;
    fq->prev.last_in_frame = 0;
    return FRAMEQUEUE_OK;
}

int framequeue_acquire(framequeue_t* fq, int* slot)
{
    uint8_t idx;

    if (!fq || !slot) {
        return FRAMEQUEUE_ERR_STATE;
    }
    /* Before the FULL test, not after: a paused consumer will never free a
     * slot, so answering FULL here is the deadlock. */
    if (fq->paused) {
        return FRAMEQUEUE_PAUSED;
    }
    idx = fq->acquire_idx;
    if (fq->state[idx] != FRAMEQUEUE_SLOT_FREE) {
        fq->overflows++;
        return FRAMEQUEUE_FULL;
    }
    fq->state[idx] = FRAMEQUEUE_SLOT_PRODUCER;
    fq->acquire_idx = next_idx(idx);
    fq->depth++;
    if (fq->depth > fq->max_depth) {
        fq->max_depth = fq->depth;
    }
    *slot = (int)idx;
    return FRAMEQUEUE_OK;
}

int framequeue_commit(framequeue_t* fq, int slot,
                      const framequeue_meta_t* meta)
{
    if (!fq || !meta) {
        return FRAMEQUEUE_ERR_STATE;
    }
    if (slot < 0 || slot >= FRAMEQUEUE_SLOTS) {
        return FRAMEQUEUE_ERR_STATE;
    }
    if ((uint8_t)slot != fq->commit_idx ||
        fq->state[slot] != FRAMEQUEUE_SLOT_PRODUCER) {
        return FRAMEQUEUE_ERR_STATE;
    }
    if (!commit_in_order(fq, meta)) {
        /* The slot stays producer-owned and nothing already committed is
         * touched, so a caller that can correct the metadata may retry. */
        return FRAMEQUEUE_ERR_ORDER;
    }
    fq->meta[slot] = *meta;
    fq->state[slot] = FRAMEQUEUE_SLOT_COMMITTED;
    fq->commit_idx = next_idx((uint8_t)slot);
    fq->prev = *meta;
    fq->have_prev = true;
    return FRAMEQUEUE_OK;
}

int framequeue_pop(framequeue_t* fq, int* slot, framequeue_meta_t* meta)
{
    uint8_t idx;

    if (!fq || !slot) {
        return FRAMEQUEUE_ERR_STATE;
    }
    idx = fq->pop_idx;
    if (fq->state[idx] != FRAMEQUEUE_SLOT_COMMITTED) {
        return FRAMEQUEUE_EMPTY;
    }
    fq->state[idx] = FRAMEQUEUE_SLOT_CONSUMER;
    fq->pop_idx = next_idx(idx);
    *slot = (int)idx;
    if (meta) {
        *meta = fq->meta[idx];
    }
    return FRAMEQUEUE_OK;
}

int framequeue_release(framequeue_t* fq, int slot)
{
    if (!fq) {
        return FRAMEQUEUE_ERR_STATE;
    }
    if (slot < 0 || slot >= FRAMEQUEUE_SLOTS) {
        return FRAMEQUEUE_ERR_STATE;
    }
    if ((uint8_t)slot != fq->release_idx ||
        fq->state[slot] != FRAMEQUEUE_SLOT_CONSUMER) {
        return FRAMEQUEUE_ERR_STATE;
    }
    fq->state[slot] = FRAMEQUEUE_SLOT_FREE;
    fq->release_idx = next_idx((uint8_t)slot);
    fq->depth--;
    return FRAMEQUEUE_OK;
}

void framequeue_pause(framequeue_t* fq)
{
    if (!fq) {
        return;
    }
    fq->paused = true;
}

void framequeue_resume(framequeue_t* fq)
{
    if (!fq) {
        return;
    }
    fq->paused = false;
    /* A pause can land mid-frame. Whatever partial frame it left behind must
     * not constrain the next commit, so the sequence restarts. */
    fq->have_prev = false;
}

bool framequeue_drained(const framequeue_t* fq)
{
    unsigned i;

    if (!fq) {
        return false;
    }
    for (i = 0; i < FRAMEQUEUE_SLOTS; i++) {
        if (fq->state[i] == FRAMEQUEUE_SLOT_COMMITTED ||
            fq->state[i] == FRAMEQUEUE_SLOT_CONSUMER) {
            return false;
        }
    }
    return true;
}

uint32_t framequeue_overflows(const framequeue_t* fq)
{
    return fq ? fq->overflows : 0u;
}

uint8_t framequeue_max_depth(const framequeue_t* fq)
{
    return fq ? fq->max_depth : 0u;
}
