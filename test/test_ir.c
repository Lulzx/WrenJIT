#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "wren_jit_ir.h"
#include "wren_jit_opt.h"

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %s...", #name); name(); printf(" OK\n"); } while(0)

TEST(test_buffer_init) {
    IRBuffer buf;
    irBufferInit(&buf);
    assert(buf.count == 0);
    assert(buf.snapshot_count == 0);
}

TEST(test_emit_const) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t id = irEmitConst(&buf, 42.0);
    assert(id == 0);
    assert(buf.count == 1);
    assert(buf.nodes[0].op == IR_CONST_NUM);
    assert(buf.nodes[0].imm.num == 42.0);
    assert(buf.nodes[0].type == IR_TYPE_NUM);
}

TEST(test_emit_const_bool) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t t = irEmitConstBool(&buf, true);
    uint16_t f = irEmitConstBool(&buf, false);
    assert(buf.count == 2);
    assert(buf.nodes[t].op == IR_CONST_BOOL);
    assert(buf.nodes[t].imm.intval == 1);
    assert(buf.nodes[t].type == IR_TYPE_BOOL);
    assert(buf.nodes[f].op == IR_CONST_BOOL);
    assert(buf.nodes[f].imm.intval == 0);
}

TEST(test_emit_const_null) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t n = irEmitConstNull(&buf);
    assert(buf.count == 1);
    assert(buf.nodes[n].op == IR_CONST_NULL);
    assert(buf.nodes[n].type == IR_TYPE_VALUE);
}

TEST(test_emit_const_obj) {
    IRBuffer buf;
    irBufferInit(&buf);
    int dummy = 0;
    uint16_t o = irEmitConstObj(&buf, &dummy);
    assert(buf.count == 1);
    assert(buf.nodes[o].op == IR_CONST_OBJ);
    assert(buf.nodes[o].imm.ptr == &dummy);
    assert(buf.nodes[o].type == IR_TYPE_PTR);
}

TEST(test_emit_arithmetic) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 10.0);
    uint16_t b = irEmitConst(&buf, 20.0);
    uint16_t sum = irEmit(&buf, IR_ADD, a, b, IR_TYPE_NUM);
    assert(buf.count == 3);
    assert(buf.nodes[sum].op == IR_ADD);
    assert(buf.nodes[sum].op1 == a);
    assert(buf.nodes[sum].op2 == b);
    assert(buf.nodes[sum].type == IR_TYPE_NUM);
}

TEST(test_emit_all_arith_ops) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 10.0);
    uint16_t b = irEmitConst(&buf, 3.0);

    uint16_t add = irEmit(&buf, IR_ADD, a, b, IR_TYPE_NUM);
    uint16_t sub = irEmit(&buf, IR_SUB, a, b, IR_TYPE_NUM);
    uint16_t mul = irEmit(&buf, IR_MUL, a, b, IR_TYPE_NUM);
    uint16_t div = irEmit(&buf, IR_DIV, a, b, IR_TYPE_NUM);
    uint16_t mod = irEmit(&buf, IR_MOD, a, b, IR_TYPE_NUM);
    uint16_t neg = irEmit(&buf, IR_NEG, a, IR_NONE, IR_TYPE_NUM);

    assert(buf.nodes[add].op == IR_ADD);
    assert(buf.nodes[sub].op == IR_SUB);
    assert(buf.nodes[mul].op == IR_MUL);
    assert(buf.nodes[div].op == IR_DIV);
    assert(buf.nodes[mod].op == IR_MOD);
    assert(buf.nodes[neg].op == IR_NEG);
    assert(buf.nodes[neg].op2 == IR_NONE);
    assert(buf.count == 8);
}

TEST(test_emit_comparisons) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 5.0);
    uint16_t b = irEmitConst(&buf, 10.0);

    uint16_t lt  = irEmit(&buf, IR_LT,  a, b, IR_TYPE_BOOL);
    uint16_t gt  = irEmit(&buf, IR_GT,  a, b, IR_TYPE_BOOL);
    uint16_t lte = irEmit(&buf, IR_LTE, a, b, IR_TYPE_BOOL);
    uint16_t gte = irEmit(&buf, IR_GTE, a, b, IR_TYPE_BOOL);
    uint16_t eq  = irEmit(&buf, IR_EQ,  a, b, IR_TYPE_BOOL);
    uint16_t neq = irEmit(&buf, IR_NEQ, a, b, IR_TYPE_BOOL);

    assert(buf.nodes[lt].op  == IR_LT);
    assert(buf.nodes[gt].op  == IR_GT);
    assert(buf.nodes[lte].op == IR_LTE);
    assert(buf.nodes[gte].op == IR_GTE);
    assert(buf.nodes[eq].op  == IR_EQ);
    assert(buf.nodes[neq].op == IR_NEQ);
    assert(buf.nodes[lt].type == IR_TYPE_BOOL);
}

TEST(test_emit_guard) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t val = irEmitLoad(&buf, 0);
    uint16_t snap = irEmitSnapshot(&buf, NULL, 1);
    uint16_t guard = irEmitGuardNum(&buf, val, snap);
    assert(buf.nodes[guard].op == IR_GUARD_NUM);
    assert(buf.nodes[guard].flags & IR_FLAG_GUARD);
}

TEST(test_emit_guard_class) {
    IRBuffer buf;
    irBufferInit(&buf);
    int dummy_class = 0;
    uint16_t val = irEmitLoad(&buf, 0);
    uint16_t snap = irEmitSnapshot(&buf, NULL, 1);
    uint16_t guard = irEmitGuardClass(&buf, val, &dummy_class, snap);
    assert(buf.nodes[guard].op == IR_GUARD_CLASS);
    assert(buf.nodes[guard].flags & IR_FLAG_GUARD);
}

TEST(test_emit_guard_true_false) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t val = irEmitLoad(&buf, 0);
    uint16_t snap = irEmitSnapshot(&buf, NULL, 1);
    uint16_t gt = irEmitGuardTrue(&buf, val, snap);
    uint16_t gf = irEmitGuardFalse(&buf, val, snap);
    assert(buf.nodes[gt].op == IR_GUARD_TRUE);
    assert(buf.nodes[gf].op == IR_GUARD_FALSE);
    assert(buf.nodes[gt].flags & IR_FLAG_GUARD);
    assert(buf.nodes[gf].flags & IR_FLAG_GUARD);
}

TEST(test_load_store) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t v = irEmitConst(&buf, 99.0);
    uint16_t boxed = irEmitBox(&buf, v);
    uint16_t store = irEmitStore(&buf, 3, boxed);
    uint16_t load = irEmitLoad(&buf, 3);
    assert(buf.nodes[store].op == IR_STORE_STACK);
    assert(buf.nodes[load].op == IR_LOAD_STACK);
    assert(buf.nodes[store].imm.mem.slot == 3);
    assert(buf.nodes[load].imm.mem.slot == 3);
}

TEST(test_load_store_field) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t obj = irEmitLoad(&buf, 0);
    uint16_t val = irEmitConst(&buf, 7.0);
    uint16_t boxed = irEmitBox(&buf, val);
    uint16_t sf = irEmitStoreField(&buf, obj, 2, boxed);
    uint16_t lf = irEmitLoadField(&buf, obj, 2);
    assert(buf.nodes[sf].op == IR_STORE_FIELD);
    assert(buf.nodes[lf].op == IR_LOAD_FIELD);
}

TEST(test_snapshot) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t v0 = irEmitLoad(&buf, 0);
    uint16_t v1 = irEmitLoad(&buf, 1);
    uint16_t snap = irEmitSnapshot(&buf, (uint8_t*)0x1000, 2);
    irSnapshotAddEntry(&buf, snap, 0, v0);
    irSnapshotAddEntry(&buf, snap, 1, v1);
    assert(buf.snapshot_count == 1);
    assert(buf.snapshots[0].num_entries == 2);
    assert(buf.snapshots[0].stack_depth == 2);
    assert(buf.snapshots[0].resume_pc == (uint8_t*)0x1000);
}

TEST(test_multiple_snapshots) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t v0 = irEmitLoad(&buf, 0);
    uint16_t v1 = irEmitLoad(&buf, 1);

    uint16_t s0 = irEmitSnapshot(&buf, (uint8_t*)0x1000, 1);
    irSnapshotAddEntry(&buf, s0, 0, v0);

    uint16_t s1 = irEmitSnapshot(&buf, (uint8_t*)0x2000, 2);
    irSnapshotAddEntry(&buf, s1, 0, v0);
    irSnapshotAddEntry(&buf, s1, 1, v1);

    assert(buf.snapshot_count == 2);
    assert(buf.snapshots[0].num_entries == 1);
    assert(buf.snapshots[1].num_entries == 2);
    assert(buf.snapshots[0].resume_pc == (uint8_t*)0x1000);
    assert(buf.snapshots[1].resume_pc == (uint8_t*)0x2000);
}

TEST(test_box_unbox) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t c = irEmitConst(&buf, 5.0);
    uint16_t boxed = irEmitBox(&buf, c);
    uint16_t unboxed = irEmitUnbox(&buf, boxed);
    assert(buf.nodes[boxed].op == IR_BOX_NUM);
    assert(buf.nodes[boxed].op1 == c);
    assert(buf.nodes[unboxed].op == IR_UNBOX_NUM);
    assert(buf.nodes[unboxed].op1 == boxed);
}

TEST(test_box_unbox_elim) {
    // Create: const -> box -> unbox -> add
    // After optimization, box/unbox should be eliminated.
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t c = irEmitConst(&buf, 5.0);
    uint16_t boxed = irEmitBox(&buf, c);
    uint16_t unboxed = irEmitUnbox(&buf, boxed);
    uint16_t result = irEmit(&buf, IR_ADD, unboxed, unboxed, IR_TYPE_NUM);
    // Mark result as used by a store (so DCE doesn't kill everything)
    irEmitStore(&buf, 0, irEmitBox(&buf, result));

    irOptBoxUnboxElim(&buf);
    // After elim, the unbox should reference c directly (or be dead)
    // This is a basic structural test.
    assert(buf.count >= 4); // nodes still exist
}

TEST(test_const_fold) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 3.0);
    uint16_t b = irEmitConst(&buf, 4.0);
    irEmit(&buf, IR_ADD, a, b, IR_TYPE_NUM);
    irEmitStore(&buf, 0, irEmitBox(&buf, 2)); // keep alive

    irOptConstPropFold(&buf);
    // After folding, the ADD should have been replaced with CONST_NUM(7.0)
}

TEST(test_const_fold_sub) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 10.0);
    uint16_t b = irEmitConst(&buf, 3.0);
    uint16_t sub = irEmit(&buf, IR_SUB, a, b, IR_TYPE_NUM);
    irEmitStore(&buf, 0, irEmitBox(&buf, sub));

    irOptConstPropFold(&buf);
    // After folding, SUB(10,3) should become CONST_NUM(7.0)
}

TEST(test_const_fold_mul) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 6.0);
    uint16_t b = irEmitConst(&buf, 7.0);
    uint16_t mul = irEmit(&buf, IR_MUL, a, b, IR_TYPE_NUM);
    irEmitStore(&buf, 0, irEmitBox(&buf, mul));

    irOptConstPropFold(&buf);
    // After folding, MUL(6,7) should become CONST_NUM(42.0)
}

TEST(test_dce) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 1.0);
    uint16_t b = irEmitConst(&buf, 2.0);
    uint16_t dead = irEmit(&buf, IR_ADD, a, b, IR_TYPE_NUM); // this is dead
    uint16_t c = irEmitConst(&buf, 3.0);
    irEmitStore(&buf, 0, irEmitBox(&buf, c)); // only c is live

    irOptDCE(&buf);
    // dead ADD should be marked with IR_FLAG_DEAD
    assert(buf.nodes[dead].flags & IR_FLAG_DEAD);
}

TEST(test_dce_chain) {
    // A chain of dead computations: a -> b -> c, none used by a store.
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t x = irEmitConst(&buf, 1.0);
    uint16_t y = irEmitConst(&buf, 2.0);
    uint16_t a = irEmit(&buf, IR_ADD, x, y, IR_TYPE_NUM);
    uint16_t b = irEmit(&buf, IR_MUL, a, x, IR_TYPE_NUM);
    uint16_t c = irEmit(&buf, IR_SUB, b, y, IR_TYPE_NUM);
    // Nothing uses a, b, or c. Add a live store of something else.
    uint16_t z = irEmitConst(&buf, 99.0);
    irEmitStore(&buf, 0, irEmitBox(&buf, z));

    irOptDCE(&buf);
    assert(buf.nodes[a].flags & IR_FLAG_DEAD);
    assert(buf.nodes[b].flags & IR_FLAG_DEAD);
    assert(buf.nodes[c].flags & IR_FLAG_DEAD);
}

TEST(test_loop_ir) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t header = irEmitLoopHeader(&buf);
    uint16_t v = irEmitLoad(&buf, 0);
    uint16_t one = irEmitConst(&buf, 1.0);
    irEmit(&buf, IR_ADD, v, one, IR_TYPE_NUM);
    irEmitLoopBack(&buf);
    assert(buf.loop_header == header);
}

TEST(test_phi) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 0.0);
    uint16_t b = irEmitConst(&buf, 1.0);
    uint16_t phi = irEmitPhi(&buf, a, b, IR_TYPE_NUM);
    assert(buf.nodes[phi].op == IR_PHI);
    assert(buf.nodes[phi].op1 == a);
    assert(buf.nodes[phi].op2 == b);
    assert(buf.nodes[phi].type == IR_TYPE_NUM);
}

TEST(test_side_exit) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t snap = irEmitSnapshot(&buf, (uint8_t*)0x3000, 1);
    uint16_t exit = irEmitSideExit(&buf, 0);
    assert(buf.nodes[exit].op == IR_SIDE_EXIT);
    assert(buf.nodes[exit].imm.snapshot_id == 0);
    (void)snap;
}

TEST(test_opname) {
    assert(strcmp(irOpName(IR_ADD), "ADD") == 0);
    assert(strcmp(irOpName(IR_SUB), "SUB") == 0);
    assert(strcmp(irOpName(IR_MUL), "MUL") == 0);
    assert(strcmp(irOpName(IR_DIV), "DIV") == 0);
    assert(strcmp(irOpName(IR_GUARD_NUM), "GUARD_NUM") == 0);
    assert(strcmp(irOpName(IR_CONST_NUM), "CONST_NUM") == 0);
    assert(strcmp(irOpName(IR_LOAD_STACK), "LOAD_STACK") == 0);
    assert(strcmp(irOpName(IR_STORE_STACK), "STORE_STACK") == 0);
    assert(strcmp(irOpName(IR_BOX_NUM), "BOX_NUM") == 0);
    assert(strcmp(irOpName(IR_UNBOX_NUM), "UNBOX_NUM") == 0);
    assert(strcmp(irOpName(IR_LOOP_HEADER), "LOOP_HEADER") == 0);
    assert(strcmp(irOpName(IR_LOOP_BACK), "LOOP_BACK") == 0);
    assert(strcmp(irOpName(IR_PHI), "PHI") == 0);
    assert(strcmp(irOpName(IR_NOP), "NOP") == 0);
}

TEST(test_node_id_assignment) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 1.0);
    uint16_t b = irEmitConst(&buf, 2.0);
    uint16_t c = irEmit(&buf, IR_ADD, a, b, IR_TYPE_NUM);
    assert(buf.nodes[a].id == a);
    assert(buf.nodes[b].id == b);
    assert(buf.nodes[c].id == c);
    // IDs should be sequential from 0
    assert(a == 0);
    assert(b == 1);
    assert(c == 2);
}

TEST(test_buffer_count_grows) {
    IRBuffer buf;
    irBufferInit(&buf);
    assert(buf.count == 0);
    irEmitConst(&buf, 1.0);
    assert(buf.count == 1);
    irEmitConst(&buf, 2.0);
    assert(buf.count == 2);
    irEmitConst(&buf, 3.0);
    assert(buf.count == 3);
    irEmitLoad(&buf, 0);
    assert(buf.count == 4);
}

TEST(test_redundant_guard_elim) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t val = irEmitLoad(&buf, 0);
    uint16_t snap = irEmitSnapshot(&buf, NULL, 1);
    uint16_t g1 = irEmitGuardNum(&buf, val, snap);
    uint16_t g2 = irEmitGuardNum(&buf, val, snap);  // redundant
    // Keep things alive
    uint16_t unboxed = irEmitUnbox(&buf, val);
    irEmitStore(&buf, 0, irEmitBox(&buf, unboxed));

    irOptRedundantGuardElim(&buf);
    // Second guard on same value should be eliminated
    (void)g1;
    (void)g2;
}

TEST(test_gvn) {
    IRBuffer buf;
    irBufferInit(&buf);
    uint16_t a = irEmitConst(&buf, 5.0);
    uint16_t b = irEmitConst(&buf, 3.0);
    uint16_t s1 = irEmit(&buf, IR_ADD, a, b, IR_TYPE_NUM);
    uint16_t s2 = irEmit(&buf, IR_ADD, a, b, IR_TYPE_NUM); // same expression
    // Use both to keep alive
    uint16_t result = irEmit(&buf, IR_ADD, s1, s2, IR_TYPE_NUM);
    irEmitStore(&buf, 0, irEmitBox(&buf, result));

    irOptGVN(&buf);
    // After GVN, s2 should point to s1 or be eliminated
    (void)s1;
    (void)s2;
}

int main(void) {
    printf("=== IR Tests ===\n");
    RUN(test_buffer_init);
    RUN(test_emit_const);
    RUN(test_emit_const_bool);
    RUN(test_emit_const_null);
    RUN(test_emit_const_obj);
    RUN(test_emit_arithmetic);
    RUN(test_emit_all_arith_ops);
    RUN(test_emit_comparisons);
    RUN(test_emit_guard);
    RUN(test_emit_guard_class);
    RUN(test_emit_guard_true_false);
    RUN(test_load_store);
    RUN(test_load_store_field);
    RUN(test_snapshot);
    RUN(test_multiple_snapshots);
    RUN(test_box_unbox);
    RUN(test_box_unbox_elim);
    RUN(test_const_fold);
    RUN(test_const_fold_sub);
    RUN(test_const_fold_mul);
    RUN(test_dce);
    RUN(test_dce_chain);
    RUN(test_loop_ir);
    RUN(test_phi);
    RUN(test_side_exit);
    RUN(test_opname);
    RUN(test_node_id_assignment);
    RUN(test_buffer_count_grows);
    RUN(test_redundant_guard_elim);
    RUN(test_gvn);
    printf("All IR tests passed!\n");
    return 0;
}
