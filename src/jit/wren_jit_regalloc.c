#include "wren_jit_regalloc.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define GP_SCRATCH_COUNT  6
#define FP_SCRATCH_COUNT  6
#define FP_SAVED_COUNT    4

// Encode register pool origin in the reg index stored in RegAlloc so that
// freeReg can return it to the correct pool.
// GP scratch:  0 ..  5
// FP scratch:  100 .. 105
// FP saved:    200 .. 203
#define FP_SCRATCH_BASE 100
#define FP_SAVED_BASE   200

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Classify which register class an IR type needs.
static RegClass classifyRegClass(IRType type)
{
    if (type == IR_TYPE_NUM)
        return REG_CLASS_FP;
    return REG_CLASS_GP;
}

// Try to allocate a GP scratch register. Returns encoded index or -1.
static int allocGPReg(RegAllocState* state)
{
    for (int i = 0; i < GP_SCRATCH_COUNT; i++) {
        if (state->gp_scratch_free[i]) {
            state->gp_scratch_free[i] = false;
            return i;
        }
    }
    return -1;
}

// Try to allocate an FP register (scratch first, then saved).
// Returns encoded index or -1.
static int allocFPReg(RegAllocState* state)
{
    for (int i = 0; i < FP_SCRATCH_COUNT; i++) {
        if (state->fp_scratch_free[i]) {
            state->fp_scratch_free[i] = false;
            return FP_SCRATCH_BASE + i;
        }
    }
    for (int i = 0; i < FP_SAVED_COUNT; i++) {
        if (state->fp_saved_free[i]) {
            state->fp_saved_free[i] = false;
            return FP_SAVED_BASE + i;
        }
    }
    return -1;
}

// Return a register to its free pool based on the encoded index.
static void freeReg(RegAllocState* state, const RegAlloc* alloc)
{
    if (alloc->is_spill)
        return;

    int r = alloc->loc.reg;

    if (alloc->reg_class == REG_CLASS_GP) {
        if (r >= 0 && r < GP_SCRATCH_COUNT)
            state->gp_scratch_free[r] = true;
    } else {
        // FP
        if (r >= FP_SAVED_BASE && r < FP_SAVED_BASE + FP_SAVED_COUNT)
            state->fp_saved_free[r - FP_SAVED_BASE] = true;
        else if (r >= FP_SCRATCH_BASE && r < FP_SCRATCH_BASE + FP_SCRATCH_COUNT)
            state->fp_scratch_free[r - FP_SCRATCH_BASE] = true;
    }
}

// Allocate a spill slot and return a RegAlloc for it.
static RegAlloc makeSpill(RegAllocState* state, RegClass rc)
{
    RegAlloc a;
    a.is_spill = true;
    a.loc.spill_slot = state->next_spill_slot++;
    a.reg_class = rc;

    if (a.loc.spill_slot + 1 > state->max_spill_slots)
        state->max_spill_slots = a.loc.spill_slot + 1;

    return a;
}

// Compare live ranges by start point (for qsort).
static int cmpRangeStart(const void* a, const void* b)
{
    const LiveRange* ra = (const LiveRange*)a;
    const LiveRange* rb = (const LiveRange*)b;
    if (ra->start != rb->start)
        return (int)ra->start - (int)rb->start;
    return (int)ra->end - (int)rb->end;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void regAllocInit(RegAllocState* state, int ssa_count)
{
    memset(state, 0, sizeof(*state));

    // All registers start free, except R0/R1 and FR0/FR1 which are reserved
    // as scratch for the codegen (guards, box/unbox, loads/stores use them).
    for (int i = 0; i < GP_SCRATCH_COUNT; i++)
        state->gp_scratch_free[i] = true;
    state->gp_scratch_free[0] = false;  // R0 reserved as scratch
    state->gp_scratch_free[1] = false;  // R1 reserved as scratch

    for (int i = 0; i < FP_SCRATCH_COUNT; i++)
        state->fp_scratch_free[i] = true;
    state->fp_scratch_free[0] = false;  // FR0 reserved as scratch
    state->fp_scratch_free[1] = false;  // FR1 reserved as scratch

    for (int i = 0; i < FP_SAVED_COUNT; i++)
        state->fp_saved_free[i] = true;

    state->next_spill_slot = 0;
    state->max_spill_slots = 0;

    state->ssa_count = ssa_count;
    if (ssa_count > 0) {
        state->ssa_to_reg = (RegAlloc*)calloc((size_t)ssa_count, sizeof(RegAlloc));
    } else {
        state->ssa_to_reg = NULL;
    }
}

void regAllocComputeRanges(RegAllocState* state, const IRBuffer* buf)
{
    // Temporary per-SSA-id tracking. Stack-allocated to avoid malloc overhead
    // on every trace compilation (IR_MAX_NODES=4096, total ~36KB on stack).
    bool defined[IR_MAX_NODES];
    uint16_t range_end[IR_MAX_NODES];
    uint16_t range_start[IR_MAX_NODES];
    RegClass rclass[IR_MAX_NODES];

    memset(defined,      0, sizeof(bool)    * buf->count);
    memset(range_end,    0, sizeof(uint16_t) * buf->count);
    memset(range_start,  0, sizeof(uint16_t) * buf->count);
    memset(rclass,       0, sizeof(RegClass) * buf->count);

    // Pass 1: Walk IR nodes forward, record definition points and extend
    //         live ranges for operand uses.
    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];

        // Skip dead nodes.
        if (n->flags & IR_FLAG_DEAD)
            continue;

        // Skip nodes that don't produce a usable value.
        if (n->op == IR_NOP || n->op == IR_STORE_STACK ||
            n->op == IR_STORE_FIELD || n->op == IR_STORE_MODULE_VAR ||
            n->op == IR_LOOP_HEADER || n->op == IR_LOOP_BACK ||
            n->op == IR_SIDE_EXIT || n->op == IR_SNAPSHOT)
            continue;

        uint16_t id = n->id;
        if (id >= buf->count)
            continue;

        // Record definition.
        if (!defined[id]) {
            defined[id] = true;
            range_start[id] = i;
            range_end[id] = i;
            rclass[id] = classifyRegClass(n->type);
        }

        // Extend live ranges for operands.
        if (n->op1 != IR_NONE && n->op1 < buf->count && defined[n->op1]) {
            if (i > range_end[n->op1])
                range_end[n->op1] = i;
        }
        if (n->op2 != IR_NONE && n->op2 < buf->count && defined[n->op2]) {
            if (i > range_end[n->op2])
                range_end[n->op2] = i;
        }
    }

    // Pass 2: Extend snapshot entries' live ranges.
    // Pre-compute the last side-exit index for each snapshot in one forward
    // scan instead of O(snapshot_count × count) nested loops.
    uint16_t last_exit_for_snap[IR_MAX_SNAPSHOTS];
    memset(last_exit_for_snap, 0, sizeof(uint16_t) * buf->snapshot_count);

    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->op == IR_SIDE_EXIT) {
            uint16_t sid = n->imm.snapshot_id;
            if (sid < buf->snapshot_count && i > last_exit_for_snap[sid])
                last_exit_for_snap[sid] = i;
        }
    }

    for (uint16_t si = 0; si < buf->snapshot_count; si++) {
        const IRSnapshot* snap = &buf->snapshots[si];
        uint16_t last_exit = last_exit_for_snap[si];

        // Extend all SSA refs in this snapshot to that exit point.
        for (uint16_t e = 0; e < snap->num_entries; e++) {
            uint16_t entry_idx = snap->entry_start + e;
            if (entry_idx >= buf->snapshot_entry_count)
                break;
            uint16_t ref = buf->snapshot_entries[entry_idx].ssa_ref;
            if (ref < buf->count && defined[ref]) {
                if (last_exit > range_end[ref])
                    range_end[ref] = last_exit;
            }
        }
    }

    // Pass 3: Handle PHI nodes - their live ranges span from the loop header
    //         to the loop back edge (or end of buffer).
    uint16_t loop_end = buf->count > 0 ? buf->count - 1 : 0;
    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->op == IR_LOOP_BACK) {
            loop_end = i;
            break;
        }
    }

    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        if (n->op == IR_PHI && defined[n->id]) {
            // PHI must be live from its definition through the loop back edge.
            if (loop_end > range_end[n->id])
                range_end[n->id] = loop_end;

            // The PHI's operands must also be live up to the PHI.
            if (n->op1 != IR_NONE && n->op1 < buf->count && defined[n->op1]) {
                if (i > range_end[n->op1])
                    range_end[n->op1] = i;
            }
            if (n->op2 != IR_NONE && n->op2 < buf->count && defined[n->op2]) {
                if (loop_end > range_end[n->op2])
                    range_end[n->op2] = loop_end;
            }
        }
    }

    // Compact into the ranges array.
    state->num_ranges = 0;
    for (uint16_t id = 0; id < buf->count; id++) {
        if (!defined[id])
            continue;

        LiveRange* lr = &state->ranges[state->num_ranges];
        lr->ssa_id = id;
        lr->start = range_start[id];
        lr->end = range_end[id];
        lr->reg_class = rclass[id];
        memset(&lr->alloc, 0, sizeof(lr->alloc));
        state->num_ranges++;

        if (state->num_ranges >= MAX_LIVE_RANGES)
            break;
    }

    // Sort by start point.
    qsort(state->ranges, (size_t)state->num_ranges, sizeof(LiveRange),
          cmpRangeStart);

}

// ---------------------------------------------------------------------------
// Linear scan
// ---------------------------------------------------------------------------

// Active set: live ranges currently occupying registers, kept sorted by end.
typedef struct {
    int indices[MAX_LIVE_RANGES]; // indices into state->ranges[]
    int count;
} ActiveSet;

static void activeInsert(ActiveSet* active, int range_idx,
                         const LiveRange* ranges)
{
    // Insert keeping sorted by end point (ascending).
    int pos = active->count;
    for (int i = 0; i < active->count; i++) {
        if (ranges[range_idx].end < ranges[active->indices[i]].end) {
            pos = i;
            break;
        }
    }
    // Shift right.
    for (int i = active->count; i > pos; i--)
        active->indices[i] = active->indices[i - 1];

    active->indices[pos] = range_idx;
    active->count++;
}

static void activeRemove(ActiveSet* active, int pos)
{
    for (int i = pos; i < active->count - 1; i++)
        active->indices[i] = active->indices[i + 1];
    active->count--;
}

// Expire intervals that end before `current_start`.
static void expireOldIntervals(RegAllocState* state, ActiveSet* active,
                               uint16_t current_start)
{
    int i = 0;
    while (i < active->count) {
        int ri = active->indices[i];
        LiveRange* lr = &state->ranges[ri];
        if (lr->end < current_start) {
            freeReg(state, &lr->alloc);
            activeRemove(active, i);
            // Don't increment i; the next element shifted into this position.
        } else {
            // Active is sorted by end, so once we find one that hasn't
            // expired, all subsequent ones are also still alive.
            break;
        }
    }
}

// Spill the active range with the furthest end point to make room.
// The range with the furthest end is the last element (sorted ascending).
static void spillAtInterval(RegAllocState* state, ActiveSet* active,
                            LiveRange* current)
{
    if (active->count == 0) {
        // No active range to spill; spill current.
        current->alloc = makeSpill(state, current->reg_class);
        return;
    }

    // Find the active range with the furthest end point in the same class.
    int spill_pos = -1;
    for (int i = active->count - 1; i >= 0; i--) {
        int ri = active->indices[i];
        LiveRange* candidate = &state->ranges[ri];
        if (candidate->reg_class == current->reg_class) {
            spill_pos = i;
            break;
        }
    }

    if (spill_pos == -1) {
        // No active range in the same class; just spill current.
        current->alloc = makeSpill(state, current->reg_class);
        return;
    }

    int spill_ri = active->indices[spill_pos];
    LiveRange* spill = &state->ranges[spill_ri];

    if (spill->end > current->end) {
        // Spill the active range; give its register to current.
        current->alloc = spill->alloc;
        state->ssa_to_reg[current->ssa_id] = current->alloc;

        // The spilled range gets a stack slot.
        spill->alloc = makeSpill(state, spill->reg_class);
        state->ssa_to_reg[spill->ssa_id] = spill->alloc;

        activeRemove(active, spill_pos);
        activeInsert(active, -1, NULL); // We'll insert current below.
        // Actually, we need to insert current properly. Remove the bad insert.
        active->count--; // undo the dummy insert above
    } else {
        // Current has a further or equal end; spill current.
        current->alloc = makeSpill(state, current->reg_class);
    }
}

void regAllocRun(RegAllocState* state)
{
    ActiveSet active;
    active.count = 0;

    for (int i = 0; i < state->num_ranges; i++) {
        LiveRange* lr = &state->ranges[i];

        // Expire old intervals.
        expireOldIntervals(state, &active, lr->start);

        // Try to allocate a register.
        int reg = -1;
        if (lr->reg_class == REG_CLASS_GP) {
            reg = allocGPReg(state);
        } else {
            reg = allocFPReg(state);
        }

        if (reg >= 0) {
            // Got a register.
            lr->alloc.is_spill = false;
            lr->alloc.loc.reg = reg;
            lr->alloc.reg_class = lr->reg_class;
        } else {
            // Need to spill.
            spillAtInterval(state, &active, lr);

            // If spillAtInterval gave current a register (by stealing from
            // another range), we should add it to active.
            if (!lr->alloc.is_spill) {
                // Already has a register from the spill exchange.
            } else {
                // Spilled to stack; don't add to active set.
                if (lr->ssa_id < (uint16_t)state->ssa_count)
                    state->ssa_to_reg[lr->ssa_id] = lr->alloc;
                continue;
            }
        }

        // Record allocation and add to active set.
        if (lr->ssa_id < (uint16_t)state->ssa_count)
            state->ssa_to_reg[lr->ssa_id] = lr->alloc;

        activeInsert(&active, i, state->ranges);
    }
}

RegAlloc regAllocGet(const RegAllocState* state, uint16_t ssa_id)
{
    RegAlloc empty;
    memset(&empty, 0, sizeof(empty));

    if (state->ssa_to_reg == NULL || ssa_id >= (uint16_t)state->ssa_count)
        return empty;

    return state->ssa_to_reg[ssa_id];
}

void regAllocFree(RegAllocState* state)
{
    free(state->ssa_to_reg);
    state->ssa_to_reg = NULL;
    state->ssa_count = 0;
    state->num_ranges = 0;
}
