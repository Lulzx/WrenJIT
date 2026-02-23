#ifndef wren_jit_ir_h
#define wren_jit_ir_h

#include <stdint.h>
#include <stdbool.h>

// Sentinel for "no operand".
#define IR_NONE 0xFFFF

// ---------------------------------------------------------------------------
// IR opcodes
// ---------------------------------------------------------------------------
typedef enum {
    IR_NOP,

    // Constants
    IR_CONST_NUM,        // immediate double
    IR_CONST_BOOL,       // boolean constant (0 or 1)
    IR_CONST_NULL,       // null constant
    IR_CONST_OBJ,        // object pointer constant (class ptr, etc.)
    IR_CONST_INT,        // immediate int64 (loop counters)

    // Arithmetic (operate on raw doubles)
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_DIV,
    IR_MOD,
    IR_NEG,

    // Comparison (raw doubles -> bool)
    IR_LT,
    IR_GT,
    IR_LTE,
    IR_GTE,
    IR_EQ,
    IR_NEQ,

    // Bitwise (after converting to int)
    IR_BAND,
    IR_BOR,
    IR_BXOR,
    IR_BNOT,
    IR_LSHIFT,
    IR_RSHIFT,

    // Stack access
    IR_LOAD_STACK,       // load from interpreter stack slot
    IR_STORE_STACK,      // store to interpreter stack slot

    // Field access
    IR_LOAD_FIELD,       // load object field
    IR_STORE_FIELD,      // store object field

    // Module variable access
    IR_LOAD_MODULE_VAR,
    IR_STORE_MODULE_VAR,

    // NaN-boxing
    IR_BOX_NUM,          // double -> Value (NaN-tagged)
    IR_UNBOX_NUM,        // Value (NaN-tagged) -> double
    IR_BOX_OBJ,          // Obj* -> Value
    IR_UNBOX_OBJ,        // Value -> Obj*
    IR_BOX_BOOL,         // native bool (0/1) -> Wren Value (FALSE_VAL or TRUE_VAL)
    IR_UNBOX_INT,        // NaN-tagged Value -> raw int64 (for integer IVs)
    IR_BOX_INT,          // raw int64 -> NaN-tagged Value (for integer IVs)

    // Guards (type checks with side exit)
    IR_GUARD_NUM,        // assert value is a number
    IR_GUARD_CLASS,      // assert value's class matches expected
    IR_GUARD_TRUE,       // assert value is truthy (not false/null)
    IR_GUARD_FALSE,      // assert value is falsy
    IR_GUARD_NOT_NULL,   // assert value is not null

    // Control flow
    IR_PHI,              // SSA phi node (at loop header)
    IR_LOOP_HEADER,      // marks the start of the loop
    IR_LOOP_BACK,        // backward jump to loop header
    IR_SIDE_EXIT,        // exit trace to interpreter

    // Snapshot (for deoptimization)
    IR_SNAPSHOT,         // captures live state for side exit

    // Calls
    IR_CALL_C,           // call a C function (primitive)
    IR_CALL_WREN,        // call a Wren method (may side-exit)

    IR_OPCODE_COUNT
} IROp;

// ---------------------------------------------------------------------------
// IR types
// ---------------------------------------------------------------------------
typedef enum {
    IR_TYPE_VOID,
    IR_TYPE_NUM,         // raw double (unboxed)
    IR_TYPE_BOOL,        // C bool
    IR_TYPE_VALUE,       // NaN-tagged Wren Value (uint64_t)
    IR_TYPE_PTR,         // generic pointer
    IR_TYPE_INT,         // integer (for bitwise ops, indices)
} IRType;

// ---------------------------------------------------------------------------
// Single IR node (SSA form)
// ---------------------------------------------------------------------------
typedef struct {
    IROp op;
    uint16_t id;         // SSA value number
    uint16_t op1;        // first operand (SSA ref or IR_NONE)
    uint16_t op2;        // second operand (SSA ref or IR_NONE)
    IRType type;         // result type

    union {
        double num;              // for IR_CONST_NUM
        int32_t intval;          // for IR_CONST_BOOL, slot indices
        int64_t i64;             // for IR_CONST_INT
        void* ptr;               // for IR_CONST_OBJ, class pointers
        uint16_t snapshot_id;    // for IR_SIDE_EXIT -> which snapshot
        struct {
            uint16_t slot;       // stack slot index
            uint16_t field;      // field index (for field ops)
        } mem;
    } imm;

    // Optimization metadata.
    uint8_t flags;
    #define IR_FLAG_DEAD      0x01   // marked dead by DCE
    #define IR_FLAG_INVARIANT 0x02   // loop-invariant (can hoist)
    #define IR_FLAG_HOISTED   0x04   // already hoisted
    #define IR_FLAG_GUARD     0x08   // is a guard instruction
} IRNode;

// ---------------------------------------------------------------------------
// IR buffer limits
// ---------------------------------------------------------------------------
#define IR_MAX_NODES 4096
#define IR_MAX_SNAPSHOTS 256
#define IR_MAX_SNAPSHOT_ENTRIES 64

// ---------------------------------------------------------------------------
// Snapshots (for deoptimisation)
// ---------------------------------------------------------------------------

// A snapshot entry: maps a stack slot to an SSA value.
typedef struct {
    uint16_t slot;      // interpreter stack slot
    uint16_t ssa_ref;   // IR SSA value that holds the current value
} IRSnapshotEntry;

// A snapshot: captures interpreter state at a potential side exit.
typedef struct {
    uint8_t* resume_pc;           // bytecode PC to resume at
    uint16_t num_entries;
    uint16_t entry_start;         // index into shared entry array
    int stack_depth;              // fiber stack depth at this point
} IRSnapshot;

// ---------------------------------------------------------------------------
// The IR buffer for one trace
// ---------------------------------------------------------------------------
typedef struct {
    IRNode nodes[IR_MAX_NODES];
    uint16_t count;                   // number of nodes

    IRSnapshot snapshots[IR_MAX_SNAPSHOTS];
    uint16_t snapshot_count;

    IRSnapshotEntry snapshot_entries[IR_MAX_NODES]; // shared pool
    uint16_t snapshot_entry_count;

    uint16_t loop_header;             // node index of IR_LOOP_HEADER
} IRBuffer;

// ---------------------------------------------------------------------------
// Construction API
// ---------------------------------------------------------------------------
void irBufferInit(IRBuffer* buf);

// Emit a new IR node. Returns the SSA id.
uint16_t irEmit(IRBuffer* buf, IROp op, uint16_t op1, uint16_t op2,
                IRType type);

// Emit with immediate values.
uint16_t irEmitConst(IRBuffer* buf, double num);
uint16_t irEmitConstBool(IRBuffer* buf, bool val);
uint16_t irEmitConstNull(IRBuffer* buf);
uint16_t irEmitConstObj(IRBuffer* buf, void* ptr);

uint16_t irEmitLoad(IRBuffer* buf, uint16_t slot);
uint16_t irEmitStore(IRBuffer* buf, uint16_t slot, uint16_t val);
uint16_t irEmitLoadField(IRBuffer* buf, uint16_t obj, uint16_t field);
uint16_t irEmitStoreField(IRBuffer* buf, uint16_t obj, uint16_t field,
                          uint16_t val);

uint16_t irEmitGuardNum(IRBuffer* buf, uint16_t val, uint16_t snapshot);
uint16_t irEmitGuardClass(IRBuffer* buf, uint16_t val, void* classPtr,
                          uint16_t snapshot);
uint16_t irEmitGuardTrue(IRBuffer* buf, uint16_t val, uint16_t snapshot);
uint16_t irEmitGuardFalse(IRBuffer* buf, uint16_t val, uint16_t snapshot);

uint16_t irEmitBox(IRBuffer* buf, uint16_t val);
uint16_t irEmitUnbox(IRBuffer* buf, uint16_t val);

uint16_t irEmitSnapshot(IRBuffer* buf, uint8_t* resume_pc, int stack_depth);
void irSnapshotAddEntry(IRBuffer* buf, uint16_t snapshot_id, uint16_t slot,
                        uint16_t ssa_ref);

uint16_t irEmitLoopHeader(IRBuffer* buf);
uint16_t irEmitLoopBack(IRBuffer* buf);
uint16_t irEmitSideExit(IRBuffer* buf, uint16_t snapshot_id);
uint16_t irEmitPhi(IRBuffer* buf, uint16_t op1, uint16_t op2, IRType type);

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
const char* irOpName(IROp op);
void irBufferDump(const IRBuffer* buf);

#endif // wren_jit_ir_h
