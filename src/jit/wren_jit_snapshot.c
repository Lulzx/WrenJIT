#include "wren_jit_snapshot.h"

#include <string.h>

void jitSnapshotInit(JitSnapshot* snap, uint8_t* resume_pc, int stack_depth)
{
    snap->resume_pc = resume_pc;
    snap->stack_depth = stack_depth;
    snap->num_entries = 0;
}

bool jitSnapshotAddEntry(JitSnapshot* snap, uint16_t slot, uint16_t ssa_ref)
{
    if (snap->num_entries >= JIT_MAX_SNAPSHOT_ENTRIES) return false;

    snap->entries[snap->num_entries].stack_slot = slot;
    snap->entries[snap->num_entries].ssa_ref = ssa_ref;
    snap->num_entries++;
    return true;
}
