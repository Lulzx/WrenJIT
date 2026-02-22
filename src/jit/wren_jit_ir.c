#include "wren_jit_ir.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Buffer init
// ---------------------------------------------------------------------------
void irBufferInit(IRBuffer* buf)
{
    memset(buf, 0, sizeof(IRBuffer));
}

// ---------------------------------------------------------------------------
// Core emit
// ---------------------------------------------------------------------------
uint16_t irEmit(IRBuffer* buf, IROp op, uint16_t op1, uint16_t op2,
                IRType type)
{
    assert(buf->count < IR_MAX_NODES);
    uint16_t id = buf->count++;
    IRNode* n = &buf->nodes[id];
    memset(n, 0, sizeof(IRNode));
    n->op   = op;
    n->id   = id;
    n->op1  = op1;
    n->op2  = op2;
    n->type = type;
    return id;
}

// ---------------------------------------------------------------------------
// Constant emitters
// ---------------------------------------------------------------------------
uint16_t irEmitConst(IRBuffer* buf, double num)
{
    uint16_t id = irEmit(buf, IR_CONST_NUM, IR_NONE, IR_NONE, IR_TYPE_NUM);
    buf->nodes[id].imm.num = num;
    return id;
}

uint16_t irEmitConstBool(IRBuffer* buf, bool val)
{
    uint16_t id = irEmit(buf, IR_CONST_BOOL, IR_NONE, IR_NONE, IR_TYPE_BOOL);
    buf->nodes[id].imm.intval = val ? 1 : 0;
    return id;
}

uint16_t irEmitConstNull(IRBuffer* buf)
{
    return irEmit(buf, IR_CONST_NULL, IR_NONE, IR_NONE, IR_TYPE_VALUE);
}

uint16_t irEmitConstObj(IRBuffer* buf, void* ptr)
{
    uint16_t id = irEmit(buf, IR_CONST_OBJ, IR_NONE, IR_NONE, IR_TYPE_PTR);
    buf->nodes[id].imm.ptr = ptr;
    return id;
}

// ---------------------------------------------------------------------------
// Stack access
// ---------------------------------------------------------------------------
uint16_t irEmitLoad(IRBuffer* buf, uint16_t slot)
{
    uint16_t id = irEmit(buf, IR_LOAD_STACK, IR_NONE, IR_NONE, IR_TYPE_VALUE);
    buf->nodes[id].imm.mem.slot = slot;
    return id;
}

uint16_t irEmitStore(IRBuffer* buf, uint16_t slot, uint16_t val)
{
    uint16_t id = irEmit(buf, IR_STORE_STACK, val, IR_NONE, IR_TYPE_VOID);
    buf->nodes[id].imm.mem.slot = slot;
    return id;
}

// ---------------------------------------------------------------------------
// Field access
// ---------------------------------------------------------------------------
uint16_t irEmitLoadField(IRBuffer* buf, uint16_t obj, uint16_t field)
{
    uint16_t id = irEmit(buf, IR_LOAD_FIELD, obj, IR_NONE, IR_TYPE_VALUE);
    buf->nodes[id].imm.mem.field = field;
    return id;
}

uint16_t irEmitStoreField(IRBuffer* buf, uint16_t obj, uint16_t field,
                          uint16_t val)
{
    uint16_t id = irEmit(buf, IR_STORE_FIELD, obj, val, IR_TYPE_VOID);
    buf->nodes[id].imm.mem.field = field;
    return id;
}

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------
uint16_t irEmitGuardNum(IRBuffer* buf, uint16_t val, uint16_t snapshot)
{
    uint16_t id = irEmit(buf, IR_GUARD_NUM, val, IR_NONE, IR_TYPE_VOID);
    buf->nodes[id].imm.snapshot_id = snapshot;
    buf->nodes[id].flags |= IR_FLAG_GUARD;
    return id;
}

uint16_t irEmitGuardClass(IRBuffer* buf, uint16_t val, void* classPtr,
                          uint16_t snapshot)
{
    uint16_t id = irEmit(buf, IR_GUARD_CLASS, val, snapshot, IR_TYPE_VOID);
    buf->nodes[id].imm.ptr = classPtr;
    buf->nodes[id].flags |= IR_FLAG_GUARD;
    return id;
}

uint16_t irEmitGuardTrue(IRBuffer* buf, uint16_t val, uint16_t snapshot)
{
    uint16_t id = irEmit(buf, IR_GUARD_TRUE, val, IR_NONE, IR_TYPE_VOID);
    buf->nodes[id].imm.snapshot_id = snapshot;
    buf->nodes[id].flags |= IR_FLAG_GUARD;
    return id;
}

uint16_t irEmitGuardFalse(IRBuffer* buf, uint16_t val, uint16_t snapshot)
{
    uint16_t id = irEmit(buf, IR_GUARD_FALSE, val, IR_NONE, IR_TYPE_VOID);
    buf->nodes[id].imm.snapshot_id = snapshot;
    buf->nodes[id].flags |= IR_FLAG_GUARD;
    return id;
}

// ---------------------------------------------------------------------------
// NaN-boxing
// ---------------------------------------------------------------------------
uint16_t irEmitBox(IRBuffer* buf, uint16_t val)
{
    return irEmit(buf, IR_BOX_NUM, val, IR_NONE, IR_TYPE_VALUE);
}

uint16_t irEmitUnbox(IRBuffer* buf, uint16_t val)
{
    return irEmit(buf, IR_UNBOX_NUM, val, IR_NONE, IR_TYPE_NUM);
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
uint16_t irEmitSnapshot(IRBuffer* buf, uint8_t* resume_pc, int stack_depth)
{
    assert(buf->snapshot_count < IR_MAX_SNAPSHOTS);
    uint16_t snap_id = buf->snapshot_count++;
    IRSnapshot* snap = &buf->snapshots[snap_id];
    snap->resume_pc = resume_pc;
    snap->num_entries = 0;
    snap->entry_start = buf->snapshot_entry_count;
    snap->stack_depth = stack_depth;

    uint16_t id = irEmit(buf, IR_SNAPSHOT, IR_NONE, IR_NONE, IR_TYPE_VOID);
    buf->nodes[id].imm.snapshot_id = snap_id;
    return snap_id;
}

void irSnapshotAddEntry(IRBuffer* buf, uint16_t snapshot_id, uint16_t slot,
                        uint16_t ssa_ref)
{
    IRSnapshot* snap = &buf->snapshots[snapshot_id];
    uint16_t idx = buf->snapshot_entry_count++;
    buf->snapshot_entries[idx].slot = slot;
    buf->snapshot_entries[idx].ssa_ref = ssa_ref;
    snap->num_entries++;
}

// ---------------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------------
uint16_t irEmitLoopHeader(IRBuffer* buf)
{
    uint16_t id = irEmit(buf, IR_LOOP_HEADER, IR_NONE, IR_NONE, IR_TYPE_VOID);
    buf->loop_header = id;
    return id;
}

uint16_t irEmitLoopBack(IRBuffer* buf)
{
    return irEmit(buf, IR_LOOP_BACK, buf->loop_header, IR_NONE, IR_TYPE_VOID);
}

uint16_t irEmitSideExit(IRBuffer* buf, uint16_t snapshot_id)
{
    uint16_t id = irEmit(buf, IR_SIDE_EXIT, IR_NONE, IR_NONE, IR_TYPE_VOID);
    buf->nodes[id].imm.snapshot_id = snapshot_id;
    return id;
}

uint16_t irEmitPhi(IRBuffer* buf, uint16_t op1, uint16_t op2, IRType type)
{
    return irEmit(buf, IR_PHI, op1, op2, type);
}

// ---------------------------------------------------------------------------
// Debug printing
// ---------------------------------------------------------------------------
static const char* typeName(IRType t)
{
    switch (t) {
        case IR_TYPE_VOID:  return "void";
        case IR_TYPE_NUM:   return "num";
        case IR_TYPE_BOOL:  return "bool";
        case IR_TYPE_VALUE: return "val";
        case IR_TYPE_PTR:   return "ptr";
        case IR_TYPE_INT:   return "int";
    }
    return "?";
}

const char* irOpName(IROp op)
{
    switch (op) {
    case IR_NOP:            return "NOP";
    case IR_CONST_NUM:      return "CONST_NUM";
    case IR_CONST_BOOL:     return "CONST_BOOL";
    case IR_CONST_NULL:     return "CONST_NULL";
    case IR_CONST_OBJ:      return "CONST_OBJ";
    case IR_CONST_INT:      return "CONST_INT";
    case IR_ADD:            return "ADD";
    case IR_SUB:            return "SUB";
    case IR_MUL:            return "MUL";
    case IR_DIV:            return "DIV";
    case IR_MOD:            return "MOD";
    case IR_NEG:            return "NEG";
    case IR_LT:             return "LT";
    case IR_GT:             return "GT";
    case IR_LTE:            return "LTE";
    case IR_GTE:            return "GTE";
    case IR_EQ:             return "EQ";
    case IR_NEQ:            return "NEQ";
    case IR_BAND:           return "BAND";
    case IR_BOR:            return "BOR";
    case IR_BXOR:           return "BXOR";
    case IR_BNOT:           return "BNOT";
    case IR_LSHIFT:         return "LSHIFT";
    case IR_RSHIFT:         return "RSHIFT";
    case IR_LOAD_STACK:     return "LOAD_STACK";
    case IR_STORE_STACK:    return "STORE_STACK";
    case IR_LOAD_FIELD:     return "LOAD_FIELD";
    case IR_STORE_FIELD:    return "STORE_FIELD";
    case IR_LOAD_MODULE_VAR:  return "LOAD_MODULE_VAR";
    case IR_STORE_MODULE_VAR: return "STORE_MODULE_VAR";
    case IR_BOX_NUM:        return "BOX_NUM";
    case IR_UNBOX_NUM:      return "UNBOX_NUM";
    case IR_BOX_OBJ:        return "BOX_OBJ";
    case IR_UNBOX_OBJ:      return "UNBOX_OBJ";
    case IR_BOX_BOOL:       return "BOX_BOOL";
    case IR_GUARD_NUM:      return "GUARD_NUM";
    case IR_GUARD_CLASS:    return "GUARD_CLASS";
    case IR_GUARD_TRUE:     return "GUARD_TRUE";
    case IR_GUARD_FALSE:    return "GUARD_FALSE";
    case IR_GUARD_NOT_NULL: return "GUARD_NOT_NULL";
    case IR_PHI:            return "PHI";
    case IR_LOOP_HEADER:    return "LOOP_HEADER";
    case IR_LOOP_BACK:      return "LOOP_BACK";
    case IR_SIDE_EXIT:      return "SIDE_EXIT";
    case IR_SNAPSHOT:       return "SNAPSHOT";
    case IR_CALL_C:         return "CALL_C";
    case IR_CALL_WREN:      return "CALL_WREN";
    default:                return "UNKNOWN";
    }
}

void irBufferDump(const IRBuffer* buf)
{
    printf("---- IR Trace (%d nodes, %d snapshots) ----\n",
           buf->count, buf->snapshot_count);

    for (uint16_t i = 0; i < buf->count; i++) {
        const IRNode* n = &buf->nodes[i];
        printf("  %04d %-16s", n->id, irOpName(n->op));

        switch (n->op) {
        case IR_CONST_NUM:
            printf(" %.6g", n->imm.num);
            break;
        case IR_CONST_BOOL:
            printf(" %s", n->imm.intval ? "true" : "false");
            break;
        case IR_CONST_NULL:
            break;
        case IR_CONST_OBJ:
            printf(" %p", n->imm.ptr);
            break;
        case IR_LOAD_STACK:
        case IR_STORE_STACK:
            printf(" slot=%d", n->imm.mem.slot);
            if (n->op1 != IR_NONE) printf(" %%%04d", n->op1);
            break;
        case IR_LOAD_FIELD:
        case IR_STORE_FIELD:
            printf(" %%%04d.%d", n->op1, n->imm.mem.field);
            if (n->op2 != IR_NONE) printf(" %%%04d", n->op2);
            break;
        case IR_SIDE_EXIT:
            printf(" snap=%d", n->imm.snapshot_id);
            break;
        case IR_GUARD_NUM:
        case IR_GUARD_TRUE:
        case IR_GUARD_FALSE:
        case IR_GUARD_NOT_NULL:
            printf(" %%%04d snap=%d", n->op1, n->imm.snapshot_id);
            break;
        case IR_GUARD_CLASS:
            printf(" %%%04d class=%p snap=%d", n->op1, n->imm.ptr, n->op2);
            break;
        case IR_SNAPSHOT:
            printf(" #%d", n->imm.snapshot_id);
            break;
        default:
            if (n->op1 != IR_NONE) printf(" %%%04d", n->op1);
            if (n->op2 != IR_NONE) printf(" %%%04d", n->op2);
            break;
        }

        printf("  -> %s", typeName(n->type));
        if (n->flags) {
            printf(" [");
            if (n->flags & IR_FLAG_DEAD) printf("dead ");
            if (n->flags & IR_FLAG_INVARIANT) printf("inv ");
            if (n->flags & IR_FLAG_HOISTED) printf("hoist ");
            if (n->flags & IR_FLAG_GUARD) printf("guard ");
            printf("]");
        }
        printf("\n");
    }

    for (uint16_t i = 0; i < buf->snapshot_count; i++) {
        const IRSnapshot* s = &buf->snapshots[i];
        printf("  snap#%d pc=%p depth=%d entries=[", i, (void*)s->resume_pc,
               s->stack_depth);
        for (uint16_t j = 0; j < s->num_entries; j++) {
            const IRSnapshotEntry* e =
                &buf->snapshot_entries[s->entry_start + j];
            printf(" %d:%%%04d", e->slot, e->ssa_ref);
        }
        printf(" ]\n");
    }
}
