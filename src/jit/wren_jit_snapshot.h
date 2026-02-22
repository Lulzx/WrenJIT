#ifndef wren_jit_snapshot_h
#define wren_jit_snapshot_h

#include <stdint.h>
#include <stdbool.h>

// A snapshot entry: maps an interpreter stack slot to the IR SSA value
// that holds its current value at a side exit point
typedef struct {
    uint16_t stack_slot;    // interpreter stack slot index
    uint16_t ssa_ref;       // IR SSA value number
} JitSnapshotEntry;

#define JIT_MAX_SNAPSHOT_ENTRIES 64

// A deoptimization snapshot
typedef struct JitSnapshot {
    uint8_t* resume_pc;              // interpreter bytecode PC to resume at
    int stack_depth;                 // how deep the stack is
    JitSnapshotEntry entries[JIT_MAX_SNAPSHOT_ENTRIES];
    uint16_t num_entries;
} JitSnapshot;

// Initialize a snapshot
void jitSnapshotInit(JitSnapshot* snap, uint8_t* resume_pc, int stack_depth);

// Add an entry to a snapshot. Returns false if full.
bool jitSnapshotAddEntry(JitSnapshot* snap, uint16_t slot, uint16_t ssa_ref);

#endif
