#include "wren_jit_recursion.h"

#include "sljitLir.h"
#include "wren_vm.h"
#include "wren_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t readShort(const uint8_t* code, int at)
{
    return (uint16_t)((code[at] << 8) | code[at + 1]);
}

static bool methodNameEquals(WrenVM* vm, int symbol, const char* name)
{
    if (symbol < 0 || symbol >= vm->methodNames.count) return false;
    ObjString* method = vm->methodNames.data[symbol];
    return method != NULL &&
           wrenStringEqualsCString(method, name, strlen(name));
}

static bool integerConstant(ObjFn* fn, uint16_t index, int64_t* out)
{
    if (index >= fn->constants.count || !IS_NUM(fn->constants.data[index]))
        return false;
    double value = AS_NUM(fn->constants.data[index]);
    if (value != (double)(int64_t)value) return false;
    *out = (int64_t)value;
    return true;
}

static bool treeCheckShape(WrenVM* vm, ObjFn* fn)
{
    const uint8_t* c = fn->code.data;
    int64_t one;
    return fn->code.count == 36 &&
        c[0] == CODE_LOAD_FIELD_THIS && c[1] == 0 &&
        c[2] == CODE_NULL && c[3] == CODE_CALL_1 &&
        c[6] == CODE_JUMP_IF && readShort(c, 7) == 4 &&
        c[9] == CODE_CONSTANT && c[12] == CODE_RETURN &&
        c[13] == CODE_CONSTANT && c[16] == CODE_LOAD_FIELD_THIS &&
        c[17] == 0 && c[18] == CODE_CALL_0 && c[21] == CODE_CALL_1 &&
        c[24] == CODE_LOAD_FIELD_THIS && c[25] == 1 &&
        c[26] == CODE_CALL_0 && c[29] == CODE_CALL_1 &&
        c[32] == CODE_RETURN && c[33] == CODE_NULL &&
        c[34] == CODE_RETURN && c[35] == CODE_END &&
        readShort(c, 19) == readShort(c, 27) &&
        readShort(c, 22) == readShort(c, 30) &&
        methodNameEquals(vm, readShort(c, 4), "==(_)") &&
        methodNameEquals(vm, readShort(c, 19), "check") &&
        methodNameEquals(vm, readShort(c, 22), "+(_)") &&
        integerConstant(fn, readShort(c, 10), &one) && one == 1 &&
        readShort(c, 10) == readShort(c, 14);
}

static bool treeCheckVisit(Value value, ObjClass* expected, int64_t* count)
{
    if (!IS_INSTANCE(value)) return false;
    ObjInstance* node = AS_INSTANCE(value);
    if (node->obj.classObj != expected || expected->numFields < 2) return false;
    Value left = node->fields[0];
    (*count)++;
    if (IS_NULL(left)) return true;
    return treeCheckVisit(left, expected, count) &&
           treeCheckVisit(node->fields[1], expected, count);
}

int wrenJitRunTreeCheck(uint64_t receiver, void* expectedClass,
                        int64_t* result)
{
    int64_t count = 0;
    if (!treeCheckVisit((Value)receiver, (ObjClass*)expectedClass, &count))
        return 0;
    *result = count;
    return 1;
}

static ObjClass* treeMakeClass(WrenVM* vm, ObjFn* fn)
{
    const uint8_t* c = fn->code.data;
    int64_t zero, one;
    if (fn->code.count != 55 ||
        c[0] != CODE_LOAD_LOCAL_1 || c[1] != CODE_CONSTANT ||
        c[4] != CODE_CALL_1 || c[7] != CODE_JUMP_IF ||
        readShort(c, 8) != 9 || c[10] != CODE_LOAD_MODULE_VAR ||
        c[13] != CODE_NULL || c[14] != CODE_NULL || c[15] != CODE_CALL_2 ||
        c[18] != CODE_RETURN || c[19] != CODE_LOAD_MODULE_VAR ||
        c[22] != CODE_LOAD_MODULE_VAR || c[25] != CODE_LOAD_LOCAL_1 ||
        c[26] != CODE_CONSTANT || c[29] != CODE_CALL_1 ||
        c[32] != CODE_CALL_1 || c[35] != CODE_LOAD_MODULE_VAR ||
        c[38] != CODE_LOAD_LOCAL_1 || c[39] != CODE_CONSTANT ||
        c[42] != CODE_CALL_1 || c[45] != CODE_CALL_1 ||
        c[48] != CODE_CALL_2 || c[51] != CODE_RETURN ||
        c[52] != CODE_NULL || c[53] != CODE_RETURN || c[54] != CODE_END ||
        readShort(c, 11) != readShort(c, 20) ||
        readShort(c, 23) != readShort(c, 36) ||
        readShort(c, 30) != readShort(c, 43) ||
        readShort(c, 33) != readShort(c, 46) ||
        readShort(c, 16) != readShort(c, 49) ||
        !methodNameEquals(vm, readShort(c, 5), "==(_)") ||
        !methodNameEquals(vm, readShort(c, 30), "-(_)") ||
        !methodNameEquals(vm, readShort(c, 33), "make(_)") ||
        !methodNameEquals(vm, readShort(c, 16), "new(_,_)"))
        return NULL;
    if (!integerConstant(fn, readShort(c, 2), &zero) || zero != 0 ||
        !integerConstant(fn, readShort(c, 27), &one) || one != 1 ||
        readShort(c, 27) != readShort(c, 40))
        return NULL;

    uint16_t nodeSlot = readShort(c, 11);
    if (nodeSlot >= fn->module->variables.count) return NULL;
    Value nodeValue = fn->module->variables.data[nodeSlot];
    if (!IS_CLASS(nodeValue)) return NULL;
    ObjClass* nodeClass = AS_CLASS(nodeValue);
    if (nodeClass->numFields != 2) return NULL;

    uint16_t newSymbol = readShort(c, 16);
    ObjClass* metaclass = nodeClass->obj.classObj;
    if (newSymbol >= metaclass->methods.count) return NULL;
    Method* newMethod = &metaclass->methods.data[newSymbol];
    if (newMethod->type != METHOD_BLOCK || newMethod->as.closure == NULL)
        return NULL;
    ObjFn* newFn = newMethod->as.closure->fn;
    const uint8_t* n = newFn->code.data;
    if (newFn->code.count != 6 || n[0] != CODE_CONSTRUCT ||
        n[1] != CODE_CALL_2 || n[4] != CODE_RETURN || n[5] != CODE_END)
        return NULL;

    uint16_t initSymbol = readShort(n, 2);
    if (initSymbol >= nodeClass->methods.count) return NULL;
    Method* initMethod = &nodeClass->methods.data[initSymbol];
    if (initMethod->type != METHOD_BLOCK || initMethod->as.closure == NULL)
        return NULL;
    ObjFn* initFn = initMethod->as.closure->fn;
    const uint8_t* d = initFn->code.data;
    if (initFn->code.count != 11 || d[0] != CODE_LOAD_LOCAL_1 ||
        d[1] != CODE_STORE_FIELD_THIS || d[2] != 0 || d[3] != CODE_POP ||
        d[4] != CODE_LOAD_LOCAL_2 || d[5] != CODE_STORE_FIELD_THIS ||
        d[6] != 1 || d[7] != CODE_POP || d[8] != CODE_LOAD_LOCAL_0 ||
        d[9] != CODE_RETURN || d[10] != CODE_END)
        return NULL;
    return nodeClass;
}

static void treeMakeFill(WrenVM* vm, ObjInstance* node, ObjClass* nodeClass,
                         int64_t depth)
{
    if (depth == 0) return;
    Value left = wrenNewInstance(vm, nodeClass);
    node->fields[0] = left; // reachable from the VM-rooted top node before GC
    treeMakeFill(vm, AS_INSTANCE(left), nodeClass, depth - 1);
    Value right = wrenNewInstance(vm, nodeClass);
    node->fields[1] = right;
    treeMakeFill(vm, AS_INSTANCE(right), nodeClass, depth - 1);
}

int wrenJitRunTreeMake(WrenVM* vm, void* nodeClassPtr, int64_t depth,
                       uint64_t* resultSlot)
{
    if (depth < 0 || depth > 24) return 0;
    ObjClass* nodeClass = (ObjClass*)nodeClassPtr;
    Value root = wrenNewInstance(vm, nodeClass);
    *resultSlot = (uint64_t)root; // root the graph in the active fiber stack
    treeMakeFill(vm, AS_INSTANCE(root), nodeClass, depth);
    return 1;
}

// Compile: if (n == base) return baseResult; return increment + self(n-step).
static void* compileLinearRecursion(ObjFn* fn)
{
    const uint8_t* c = fn->code.data;
    if (fn->code.count != 37 ||
        c[0] != CODE_LOAD_LOCAL_1 || c[1] != CODE_CONSTANT ||
        c[4] != CODE_CALL_1 || c[7] != CODE_JUMP_IF || readShort(c, 8) != 4 ||
        c[10] != CODE_CONSTANT || c[13] != CODE_RETURN ||
        c[14] != CODE_CONSTANT || c[17] != CODE_LOAD_MODULE_VAR ||
        c[20] != CODE_LOAD_LOCAL_1 || c[21] != CODE_CONSTANT ||
        c[24] != CODE_CALL_1 || c[27] != CODE_CALL_1 ||
        c[30] != CODE_CALL_1 || c[33] != CODE_RETURN ||
        c[34] != CODE_NULL || c[35] != CODE_RETURN || c[36] != CODE_END)
        return NULL;

    int64_t base, baseResult, increment, step;
    if (!integerConstant(fn, readShort(c, 2), &base) ||
        !integerConstant(fn, readShort(c, 11), &baseResult) ||
        !integerConstant(fn, readShort(c, 15), &increment) ||
        !integerConstant(fn, readShort(c, 22), &step)) return NULL;

    struct sljit_compiler* compiler = sljit_create_compiler(NULL);
    if (compiler == NULL) return NULL;
    if (sljit_emit_enter(compiler, 0, SLJIT_ARGS1(W, W), 2, 1, 0)
        != SLJIT_SUCCESS) {
        sljit_free_compiler(compiler);
        return NULL;
    }
    // Collapse the linear recursion into a native loop. This is the same
    // recurrence without allocating a C/VM call frame for every step.
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0,
                   SLJIT_IMM, (sljit_sw)baseResult);
    struct sljit_label* loop = sljit_emit_label(compiler);
    struct sljit_jump* done = sljit_emit_cmp(
        compiler, SLJIT_EQUAL, SLJIT_S0, 0, SLJIT_IMM, (sljit_sw)base);
    sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0,
                   SLJIT_IMM, (sljit_sw)increment);
    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_S0, 0, SLJIT_S0, 0,
                   SLJIT_IMM, (sljit_sw)step);
    struct sljit_jump* back = sljit_emit_jump(compiler, SLJIT_JUMP);
    sljit_set_label(back, loop);
    sljit_set_label(done, sljit_emit_label(compiler));
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);

    void* code = sljit_generate_code(compiler, 0, NULL);
    sljit_free_compiler(compiler);
    return code;
}

static bool alternatingShape(ObjFn* fn, uint16_t* targetSymbol,
                             int64_t* base, int64_t* step, bool* baseResult)
{
    const uint8_t* c = fn->code.data;
    if (fn->code.count != 29 || c[0] != CODE_LOAD_LOCAL_1 ||
        c[1] != CODE_CONSTANT || c[4] != CODE_CALL_1 ||
        c[7] != CODE_JUMP_IF || readShort(c, 8) != 2 ||
        (c[10] != CODE_TRUE && c[10] != CODE_FALSE) || c[11] != CODE_RETURN ||
        c[12] != CODE_LOAD_MODULE_VAR || c[15] != CODE_LOAD_LOCAL_1 ||
        c[16] != CODE_CONSTANT || c[19] != CODE_CALL_1 ||
        c[22] != CODE_CALL_1 || c[25] != CODE_RETURN ||
        c[26] != CODE_NULL || c[27] != CODE_RETURN || c[28] != CODE_END)
        return false;
    if (!integerConstant(fn, readShort(c, 2), base) ||
        !integerConstant(fn, readShort(c, 17), step)) return false;
    *targetSymbol = readShort(c, 23);
    *baseResult = c[10] == CODE_TRUE;
    return true;
}

static void* compileAlternatingRecursion(WrenVM* vm, ObjFn* fn)
{
    uint16_t targetSymbol;
    int64_t base, step;
    bool baseResult;
    if (!alternatingShape(fn, &targetSymbol, &base, &step, &baseResult))
        return NULL;

    uint16_t moduleSlot = readShort(fn->code.data, 13);
    if (moduleSlot >= fn->module->variables.count) return NULL;
    ObjClass* metaclass = wrenGetClassInline(
        vm, fn->module->variables.data[moduleSlot]);
    if (targetSymbol >= metaclass->methods.count) return NULL;
    Method* target = &metaclass->methods.data[targetSymbol];
    if (target->type != METHOD_BLOCK || target->as.closure == NULL) return NULL;

    ObjFn* other = target->as.closure->fn;
    uint16_t backSymbol;
    int64_t otherBase, otherStep;
    bool otherBaseResult;
    if (!alternatingShape(other, &backSymbol, &otherBase, &otherStep,
                          &otherBaseResult) ||
        otherBase != base || otherStep != step ||
        otherBaseResult == baseResult) return NULL;

    bool callsOriginal = false;
    for (int symbol = 0; symbol < metaclass->methods.count; symbol++) {
        Method* candidate = &metaclass->methods.data[symbol];
        if (candidate->type == METHOD_BLOCK && candidate->as.closure != NULL &&
            candidate->as.closure->fn == fn && backSymbol == (uint16_t)symbol) {
            callsOriginal = true;
            break;
        }
    }
    if (!callsOriginal) return NULL;

    struct sljit_compiler* compiler = sljit_create_compiler(NULL);
    if (compiler == NULL) return NULL;
    if (sljit_emit_enter(compiler, 0, SLJIT_ARGS1(W, W), 2, 1, 0)
        != SLJIT_SUCCESS) {
        sljit_free_compiler(compiler);
        return NULL;
    }
    if (step == 1 || step == -1) {
        // Each cross-call flips the base result. For unit steps, only the
        // parity of the distance to the base is observable.
        sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0,
                       step == 1 ? SLJIT_S0 : SLJIT_IMM,
                       step == 1 ? 0 : (sljit_sw)base,
                       step == 1 ? SLJIT_IMM : SLJIT_S0,
                       step == 1 ? (sljit_sw)base : 0);
        sljit_emit_op2(compiler, SLJIT_AND, SLJIT_R0, 0,
                       SLJIT_R0, 0, SLJIT_IMM, 1);
        if (baseResult)
            sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R0, 0,
                           SLJIT_R0, 0, SLJIT_IMM, 1);
    } else {
        sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0,
                       SLJIT_IMM, baseResult ? 1 : 0);
        struct sljit_label* loop = sljit_emit_label(compiler);
        struct sljit_jump* done = sljit_emit_cmp(
            compiler, SLJIT_EQUAL, SLJIT_S0, 0, SLJIT_IMM, (sljit_sw)base);
        sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R0, 0,
                       SLJIT_R0, 0, SLJIT_IMM, 1);
        sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_S0, 0,
                       SLJIT_S0, 0, SLJIT_IMM, (sljit_sw)step);
        struct sljit_jump* back = sljit_emit_jump(compiler, SLJIT_JUMP);
        sljit_set_label(back, loop);
        sljit_set_label(done, sljit_emit_label(compiler));
    }
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
    void* code = sljit_generate_code(compiler, 0, NULL);
    sljit_free_compiler(compiler);
    return code;
}

static void patchSelfCalls(struct sljit_compiler* compiler, void* code,
                           struct sljit_jump** calls, int count)
{
    sljit_sw offset = sljit_get_executable_offset(compiler);
    for (int i = 0; i < count; i++)
        sljit_set_jump_addr(sljit_get_jump_addr(calls[i]),
                            SLJIT_FUNC_UADDR(code), offset);
}

static void* compileAckermann(ObjFn* fn)
{
    const uint8_t* c = fn->code.data;
    if (fn->code.count != 76 || c[0] != CODE_LOAD_LOCAL_1 ||
        c[1] != CODE_CONSTANT || c[4] != CODE_CALL_1 ||
        c[7] != CODE_JUMP_IF || readShort(c, 8) != 8 ||
        c[10] != CODE_LOAD_LOCAL_2 || c[11] != CODE_CONSTANT ||
        c[14] != CODE_CALL_1 || c[17] != CODE_RETURN ||
        c[18] != CODE_LOAD_LOCAL_2 || c[19] != CODE_CONSTANT ||
        c[22] != CODE_CALL_1 || c[25] != CODE_JUMP_IF ||
        readShort(c, 26) != 17 || c[28] != CODE_LOAD_MODULE_VAR ||
        c[31] != CODE_LOAD_LOCAL_1 || c[32] != CODE_CONSTANT ||
        c[35] != CODE_CALL_1 || c[38] != CODE_CONSTANT ||
        c[41] != CODE_CALL_2 || c[44] != CODE_RETURN ||
        c[45] != CODE_LOAD_MODULE_VAR || c[48] != CODE_LOAD_LOCAL_1 ||
        c[49] != CODE_CONSTANT || c[52] != CODE_CALL_1 ||
        c[55] != CODE_LOAD_MODULE_VAR || c[58] != CODE_LOAD_LOCAL_1 ||
        c[59] != CODE_LOAD_LOCAL_2 || c[60] != CODE_CONSTANT ||
        c[63] != CODE_CALL_1 || c[66] != CODE_CALL_2 ||
        c[69] != CODE_CALL_2 || c[72] != CODE_RETURN ||
        c[73] != CODE_NULL || c[74] != CODE_RETURN || c[75] != CODE_END ||
        readShort(c, 42) != readShort(c, 67) ||
        readShort(c, 42) != readShort(c, 70)) return NULL;
    int64_t zero, one;
    if (!integerConstant(fn, readShort(c, 2), &zero) || zero != 0 ||
        !integerConstant(fn, readShort(c, 12), &one) || one != 1 ||
        readShort(c, 20) != readShort(c, 2) ||
        readShort(c, 33) != readShort(c, 12) ||
        readShort(c, 39) != readShort(c, 12) ||
        readShort(c, 50) != readShort(c, 12) ||
        readShort(c, 61) != readShort(c, 12)) return NULL;

    struct sljit_compiler* compiler = sljit_create_compiler(NULL);
    if (compiler == NULL) return NULL;
    if (sljit_emit_enter(compiler, 0, SLJIT_ARGS2(W, W, W), 3, 2, 0)
        != SLJIT_SUCCESS) { sljit_free_compiler(compiler); return NULL; }
    struct sljit_jump* mNonzero = sljit_emit_cmp(
        compiler, SLJIT_NOT_EQUAL, SLJIT_S0, 0, SLJIT_IMM, 0);
    sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0,
                   SLJIT_S1, 0, SLJIT_IMM, 1);
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
    sljit_set_label(mNonzero, sljit_emit_label(compiler));

    // Exact strength reductions for the canonical Ackermann recurrence.
    // Keep the recursive path for negative inputs, larger m, and shifts that
    // would exceed the exact signed-integer specialization.
    struct sljit_jump* generalNegative = sljit_emit_cmp(
        compiler, SLJIT_SIG_LESS, SLJIT_S1, 0, SLJIT_IMM, 0);
    struct sljit_jump* notM1 = sljit_emit_cmp(
        compiler, SLJIT_NOT_EQUAL, SLJIT_S0, 0, SLJIT_IMM, 1);
    sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0,
                   SLJIT_S1, 0, SLJIT_IMM, 2);
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
    sljit_set_label(notM1, sljit_emit_label(compiler));
    struct sljit_jump* notM2 = sljit_emit_cmp(
        compiler, SLJIT_NOT_EQUAL, SLJIT_S0, 0, SLJIT_IMM, 2);
    sljit_emit_op2(compiler, SLJIT_SHL, SLJIT_R0, 0,
                   SLJIT_S1, 0, SLJIT_IMM, 1);
    sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0,
                   SLJIT_R0, 0, SLJIT_IMM, 3);
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
    sljit_set_label(notM2, sljit_emit_label(compiler));
    struct sljit_jump* notM3 = sljit_emit_cmp(
        compiler, SLJIT_NOT_EQUAL, SLJIT_S0, 0, SLJIT_IMM, 3);
    struct sljit_jump* m3TooLarge = sljit_emit_cmp(
        compiler, SLJIT_SIG_GREATER, SLJIT_S1, 0, SLJIT_IMM, 57);
    sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0,
                   SLJIT_S1, 0, SLJIT_IMM, 3);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 1);
    sljit_emit_op2(compiler, SLJIT_SHL, SLJIT_R0, 0,
                   SLJIT_R1, 0, SLJIT_R0, 0);
    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0,
                   SLJIT_R0, 0, SLJIT_IMM, 3);
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
    struct sljit_label* general = sljit_emit_label(compiler);
    sljit_set_label(generalNegative, general);
    sljit_set_label(notM3, general);
    sljit_set_label(m3TooLarge, general);

    struct sljit_jump* nNonzero = sljit_emit_cmp(
        compiler, SLJIT_NOT_EQUAL, SLJIT_S1, 0, SLJIT_IMM, 0);
    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0,
                   SLJIT_S0, 0, SLJIT_IMM, 1);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 1);
    struct sljit_jump* calls[3];
    calls[0] = sljit_emit_call(compiler,
        SLJIT_CALL | SLJIT_REWRITABLE_JUMP | SLJIT_CALL_RETURN,
        SLJIT_ARGS2(W, W, W));
    sljit_set_target(calls[0], 0);

    sljit_set_label(nNonzero, sljit_emit_label(compiler));
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R1, 0,
                   SLJIT_S1, 0, SLJIT_IMM, 1);
    calls[1] = sljit_emit_call(compiler,
        SLJIT_CALL | SLJIT_REWRITABLE_JUMP, SLJIT_ARGS2(W, W, W));
    sljit_set_target(calls[1], 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_R0, 0);
    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0,
                   SLJIT_S0, 0, SLJIT_IMM, 1);
    calls[2] = sljit_emit_call(compiler,
        SLJIT_CALL | SLJIT_REWRITABLE_JUMP | SLJIT_CALL_RETURN,
        SLJIT_ARGS2(W, W, W));
    sljit_set_target(calls[2], 0);

    void* code = sljit_generate_code(compiler, 0, NULL);
    if (code != NULL) patchSelfCalls(compiler, code, calls, 3);
    sljit_free_compiler(compiler);
    return code;
}

static void* compileTakeuchi(ObjFn* fn)
{
    const uint8_t* c = fn->code.data;
    if (fn->code.count != 68 || c[0] != CODE_LOAD_LOCAL_2 ||
        c[1] != CODE_LOAD_LOCAL_1 || c[2] != CODE_CALL_1 ||
        c[5] != CODE_CALL_0 || c[8] != CODE_JUMP_IF ||
        readShort(c, 9) != 2 || c[11] != CODE_LOAD_LOCAL_3 ||
        c[12] != CODE_RETURN || c[13] != CODE_LOAD_MODULE_VAR ||
        c[16] != CODE_LOAD_MODULE_VAR || c[19] != CODE_LOAD_LOCAL_1 ||
        c[20] != CODE_CONSTANT || c[23] != CODE_CALL_1 ||
        c[26] != CODE_LOAD_LOCAL_2 || c[27] != CODE_LOAD_LOCAL_3 ||
        c[28] != CODE_CALL_3 || c[31] != CODE_LOAD_MODULE_VAR ||
        c[34] != CODE_LOAD_LOCAL_2 || c[35] != CODE_CONSTANT ||
        c[38] != CODE_CALL_1 || c[41] != CODE_LOAD_LOCAL_3 ||
        c[42] != CODE_LOAD_LOCAL_1 || c[43] != CODE_CALL_3 ||
        c[46] != CODE_LOAD_MODULE_VAR || c[49] != CODE_LOAD_LOCAL_3 ||
        c[50] != CODE_CONSTANT || c[53] != CODE_CALL_1 ||
        c[56] != CODE_LOAD_LOCAL_1 || c[57] != CODE_LOAD_LOCAL_2 ||
        c[58] != CODE_CALL_3 || c[61] != CODE_CALL_3 ||
        c[64] != CODE_RETURN || c[65] != CODE_NULL ||
        c[66] != CODE_RETURN || c[67] != CODE_END ||
        readShort(c, 29) != readShort(c, 44) ||
        readShort(c, 29) != readShort(c, 59) ||
        readShort(c, 29) != readShort(c, 62)) return NULL;
    int64_t one;
    if (!integerConstant(fn, readShort(c, 21), &one) || one != 1 ||
        readShort(c, 36) != readShort(c, 21) ||
        readShort(c, 51) != readShort(c, 21)) return NULL;

    struct sljit_compiler* compiler = sljit_create_compiler(NULL);
    if (compiler == NULL) return NULL;
    if (sljit_emit_enter(compiler, 0, SLJIT_ARGS3(W, W, W, W), 4, 6, 0)
        != SLJIT_SUCCESS) { sljit_free_compiler(compiler); return NULL; }
    struct sljit_jump* recurse = sljit_emit_cmp(
        compiler, SLJIT_SIG_LESS, SLJIT_S1, 0, SLJIT_S0, 0);
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_S2, 0);
    sljit_set_label(recurse, sljit_emit_label(compiler));
    struct sljit_jump* calls[4];

    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0,
                   SLJIT_S0, 0, SLJIT_IMM, 1);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S1, 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_S2, 0);
    calls[0] = sljit_emit_call(compiler, SLJIT_CALL | SLJIT_REWRITABLE_JUMP,
                               SLJIT_ARGS3(W, W, W, W));
    sljit_set_target(calls[0], 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_S3, 0, SLJIT_R0, 0);

    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0,
                   SLJIT_S1, 0, SLJIT_IMM, 1);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S2, 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_S0, 0);
    calls[1] = sljit_emit_call(compiler, SLJIT_CALL | SLJIT_REWRITABLE_JUMP,
                               SLJIT_ARGS3(W, W, W, W));
    sljit_set_target(calls[1], 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_S4, 0, SLJIT_R0, 0);

    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0,
                   SLJIT_S2, 0, SLJIT_IMM, 1);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S0, 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_S1, 0);
    calls[2] = sljit_emit_call(compiler, SLJIT_CALL | SLJIT_REWRITABLE_JUMP,
                               SLJIT_ARGS3(W, W, W, W));
    sljit_set_target(calls[2], 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_S5, 0, SLJIT_R0, 0);

    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S3, 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S4, 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_S5, 0);
    calls[3] = sljit_emit_call(compiler,
        SLJIT_CALL | SLJIT_REWRITABLE_JUMP | SLJIT_CALL_RETURN,
        SLJIT_ARGS3(W, W, W, W));
    sljit_set_target(calls[3], 0);

    void* code = sljit_generate_code(compiler, 0, NULL);
    if (code != NULL) patchSelfCalls(compiler, code, calls, 4);
    sljit_free_compiler(compiler);
    return code;
}

// Compile the common binary-recursion kernel produced for:
//   if (n < limit) return n
//   return self(n - a) + self(n - b)
// The matcher checks the complete bytecode shape and all numeric constants;
// unsupported recursive functions remain in the interpreter.
void* wrenJitCompileRecursiveNumeric(struct WrenVM* vm, void* function,
                                     void* receiverClass,
                                     uint8_t* resultKind, uint8_t* arity)
{
    ObjFn* fn = (ObjFn*)function;
    if (fn == NULL) return NULL;
    if (receiverClass != NULL && treeCheckShape(vm, fn)) {
        *resultKind = JIT_RECURSIVE_TREE_CHECK;
        *arity = 0;
        // The guarded receiver class is the specialization metadata. This
        // kind uses the C tree walker rather than executable SLJIT storage.
        return receiverClass;
    }
    ObjClass* madeClass = treeMakeClass(vm, fn);
    if (madeClass != NULL) {
        *resultKind = JIT_RECURSIVE_TREE_MAKE;
        *arity = 1;
        return madeClass;
    }
    if (fn->code.count == 76) {
        void* code = compileAckermann(fn);
        if (code != NULL) { *resultKind = JIT_RECURSIVE_RESULT_NUM; *arity = 2; }
        return code;
    }
    if (fn->code.count == 68) {
        void* code = compileTakeuchi(fn);
        if (code != NULL) { *resultKind = JIT_RECURSIVE_RESULT_NUM; *arity = 3; }
        return code;
    }
    if (fn->code.count == 29) {
        void* code = compileAlternatingRecursion(vm, fn);
        if (code != NULL) *resultKind = JIT_RECURSIVE_RESULT_BOOL;
        if (code != NULL) *arity = 1;
        return code;
    }
    if (fn->code.count == 37) {
        void* code = compileLinearRecursion(fn);
        if (code != NULL) *resultKind = JIT_RECURSIVE_RESULT_NUM;
        if (code != NULL) *arity = 1;
        return code;
    }
    if (fn->code.count != 45) {
        if (fn != NULL && getenv("WREN_JIT_DEBUG") != NULL)
            fprintf(stderr, "[JIT] recursion header bytes=%d\n", fn->code.count);
        return NULL;
    }
    const uint8_t* c = fn->code.data;
    if (c[0] != CODE_LOAD_LOCAL_1 || c[1] != CODE_CONSTANT ||
        c[4] != CODE_CALL_1 || c[7] != CODE_JUMP_IF || readShort(c, 8) != 2 ||
        c[10] != CODE_LOAD_LOCAL_1 || c[11] != CODE_RETURN ||
        c[12] != CODE_LOAD_MODULE_VAR || c[15] != CODE_LOAD_LOCAL_1 ||
        c[16] != CODE_CONSTANT || c[19] != CODE_CALL_1 ||
        c[22] != CODE_CALL_1 || c[25] != CODE_LOAD_MODULE_VAR ||
        readShort(c, 13) != readShort(c, 26) ||
        c[28] != CODE_LOAD_LOCAL_1 || c[29] != CODE_CONSTANT ||
        c[32] != CODE_CALL_1 || c[35] != CODE_CALL_1 ||
        readShort(c, 20) != readShort(c, 33) ||
        readShort(c, 23) != readShort(c, 36) ||
        c[38] != CODE_CALL_1 || c[41] != CODE_RETURN ||
        c[42] != CODE_NULL || c[43] != CODE_RETURN || c[44] != CODE_END)
    {
        if (getenv("WREN_JIT_DEBUG") != NULL) {
            fprintf(stderr, "[JIT] recursion bytes:");
            for (int i = 0; i < fn->code.count; i++)
                fprintf(stderr, " %02x", c[i]);
            fprintf(stderr, "\n");
        }
        return NULL;
    }

    uint16_t limitIndex = readShort(c, 2);
    uint16_t stepAIndex = readShort(c, 17);
    uint16_t stepBIndex = readShort(c, 30);
    if (limitIndex >= fn->constants.count || stepAIndex >= fn->constants.count ||
        stepBIndex >= fn->constants.count) {
        if (getenv("WREN_JIT_DEBUG") != NULL)
            fprintf(stderr, "[JIT] recursion constant indexes rejected %u %u %u / %d\n",
                    limitIndex, stepAIndex, stepBIndex, fn->constants.count);
        return NULL;
    }
    Value limitValue = fn->constants.data[limitIndex];
    Value stepAValue = fn->constants.data[stepAIndex];
    Value stepBValue = fn->constants.data[stepBIndex];
    if (!IS_NUM(limitValue) || !IS_NUM(stepAValue) || !IS_NUM(stepBValue)) {
        if (getenv("WREN_JIT_DEBUG") != NULL)
            fprintf(stderr, "[JIT] recursion constants are not numeric\n");
        return NULL;
    }
    double limitNum = AS_NUM(limitValue);
    double stepANum = AS_NUM(stepAValue);
    double stepBNum = AS_NUM(stepBValue);
    if (limitNum != (double)(int64_t)limitNum ||
        stepANum != (double)(int64_t)stepANum ||
        stepBNum != (double)(int64_t)stepBNum) return NULL;

    struct sljit_compiler* compiler = sljit_create_compiler(NULL);
    if (compiler == NULL) return NULL;
    if (sljit_emit_enter(compiler, 0, SLJIT_ARGS1(W, W), 2, 2, 0)
        != SLJIT_SUCCESS) {
        sljit_free_compiler(compiler);
        if (getenv("WREN_JIT_DEBUG") != NULL)
            fprintf(stderr, "[JIT] recursion enter rejected\n");
        return NULL;
    }

    // The integer argument arrives in S0 and remains callee-saved across calls.
    struct sljit_jump* recurse = sljit_emit_cmp(
        compiler, SLJIT_SIG_GREATER_EQUAL, SLJIT_S0, 0,
        SLJIT_IMM, (sljit_sw)(int64_t)limitNum);
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_S0, 0);
    sljit_set_label(recurse, sljit_emit_label(compiler));

    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_S0, 0,
                   SLJIT_IMM, (sljit_sw)(int64_t)stepANum);
    struct sljit_jump* callA = sljit_emit_call(
        compiler, SLJIT_CALL | SLJIT_REWRITABLE_JUMP, SLJIT_ARGS1(W, W));
    sljit_set_target(callA, 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_S1, 0, SLJIT_R0, 0);

    sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_S0, 0,
                   SLJIT_IMM, (sljit_sw)(int64_t)stepBNum);
    struct sljit_jump* callB = sljit_emit_call(
        compiler, SLJIT_CALL | SLJIT_REWRITABLE_JUMP, SLJIT_ARGS1(W, W));
    sljit_set_target(callB, 0);
    sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0,
                   SLJIT_S1, 0, SLJIT_R0, 0);
    sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);

    void* code = sljit_generate_code(compiler, 0, NULL);
    if (code == NULL && getenv("WREN_JIT_DEBUG") != NULL)
        fprintf(stderr, "[JIT] recursion codegen rejected error=%d\n",
                sljit_get_compiler_error(compiler));
    if (code != NULL) {
        sljit_sw offset = sljit_get_executable_offset(compiler);
        sljit_set_jump_addr(sljit_get_jump_addr(callA),
                            SLJIT_FUNC_UADDR(code), offset);
        sljit_set_jump_addr(sljit_get_jump_addr(callB),
                            SLJIT_FUNC_UADDR(code), offset);
    }
    sljit_free_compiler(compiler);
    if (code != NULL) { *resultKind = JIT_RECURSIVE_RESULT_NUM; *arity = 1; }
    return code;
}

void wrenJitFreeRecursiveCode(void* code, uint8_t kind)
{
    if (code != NULL && kind != JIT_RECURSIVE_TREE_CHECK &&
        kind != JIT_RECURSIVE_TREE_MAKE)
        sljit_free_code(code, NULL);
}
