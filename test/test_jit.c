#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "wren.h"
#include "wren_vm.h"
#include "wren_jit.h"

static char output_buf[4096];
static int output_len = 0;

static void writeFn(WrenVM* vm, const char* text) {
    (void)vm;
    int len = (int)strlen(text);
    if (output_len + len < (int)sizeof(output_buf)) {
        memcpy(output_buf + output_len, text, len);
        output_len += len;
    }
}

static void errorFn(WrenVM* vm, WrenErrorType type, const char* module,
                    int line, const char* msg) {
    (void)vm;
    (void)type;
    fprintf(stderr, "[%s:%d] %s\n", module ? module : "?", line, msg);
}

static void resetOutput(void) {
    output_len = 0;
    memset(output_buf, 0, sizeof(output_buf));
}

static WrenVM* createVM(void) {
    WrenConfiguration config;
    wrenInitConfiguration(&config);
    config.writeFn = writeFn;
    config.errorFn = errorFn;
    return wrenNewVM(&config);
}

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %s...", #name); name(); printf(" OK\n"); } while(0)

TEST(test_simple_sum) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < 100) {\n"
        "  sum = sum + i\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "4950") != NULL);
    wrenFreeVM(vm);
}

TEST(test_for_loop) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "for (i in 1..10) {\n"
        "  sum = sum + i\n"
        "}\n"
        "System.print(sum)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "55") != NULL);
    wrenFreeVM(vm);
}

TEST(test_nested_arithmetic) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var x = 0\n"
        "var i = 0\n"
        "while (i < 50) {\n"
        "  x = x + i * 2 - 1\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(x)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    // Sum of (i*2 - 1) for i=0..49 = 2*sum(0..49) - 50 = 2*1225 - 50 = 2400
    assert(strstr(output_buf, "2400") != NULL);
    wrenFreeVM(vm);
}

TEST(test_comparison) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var count = 0\n"
        "var i = 0\n"
        "while (i < 100) {\n"
        "  if (i > 50) count = count + 1\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(count)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "49") != NULL);
    wrenFreeVM(vm);
}

TEST(test_multiplication_loop) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var prod = 1\n"
        "var i = 1\n"
        "while (i <= 10) {\n"
        "  prod = prod * i\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(prod)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    // 10! = 3628800
    assert(strstr(output_buf, "3628800") != NULL);
    wrenFreeVM(vm);
}

TEST(test_nested_while) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var total = 0\n"
        "var i = 0\n"
        "while (i < 10) {\n"
        "  var j = 0\n"
        "  while (j < 10) {\n"
        "    total = total + 1\n"
        "    j = j + 1\n"
        "  }\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(total)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "100") != NULL);
    wrenFreeVM(vm);
}

TEST(test_hot_loop) {
    // A loop that iterates enough to trigger JIT compilation.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < 1000) {\n"
        "  sum = sum + i\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    // sum(0..999) = 499500
    assert(strstr(output_buf, "499500") != NULL);
    wrenFreeVM(vm);
}

TEST(test_multiple_vms) {
    // Ensure independent VMs work correctly.
    for (int iter = 0; iter < 3; iter++) {
        resetOutput();
        WrenVM* vm = createVM();
        const char* src =
            "var x = 0\n"
            "var i = 0\n"
            "while (i < 10) {\n"
            "  x = x + 1\n"
            "  i = i + 1\n"
            "}\n"
            "System.print(x)\n";
        WrenInterpretResult result = wrenInterpret(vm, "main", src);
        assert(result == WREN_RESULT_SUCCESS);
        assert(strstr(output_buf, "10") != NULL);
        wrenFreeVM(vm);
    }
}


TEST(test_modulo_falls_back_safely) {
    // IR_MOD has no native code generator yet. The recorder must reject it
    // instead of compiling a trace whose result register is undefined.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < 1000) {\n"
        "  sum = sum + (i % 7)\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "2997") != NULL);
    wrenFreeVM(vm);
}

TEST(test_nan_not_equal) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var nan = 0 / 0\n"
        "var count = 0\n"
        "var i = 0\n"
        "while (i < 100) {\n"
        "  if (nan != nan) count = count + 1\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(count)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "100") != NULL);
    wrenFreeVM(vm);
}


TEST(test_hot_counter_saturates_and_blacklists) {
    WrenJitState jit;
    memset(&jit, 0, sizeof(jit));
    jit.enabled = true;
    jit.hot_threshold = 2;

    uint16_t count = 0;
    assert(!wrenJitIncrementHot(&jit, &count));
    assert(wrenJitIncrementHot(&jit, &count));
    assert(!wrenJitIncrementHot(&jit, &count));
    assert(count == 2); // Already-hot counters are read-only.

    jit.active_hot_count = &count;
    wrenJitBlacklistCurrent(&jit);
    assert(count == JIT_HOT_BLACKLISTED);
    assert(!wrenJitIncrementHot(&jit, &count));
    assert(count == JIT_HOT_BLACKLISTED);
}

TEST(test_unsupported_loop_is_blacklisted_once) {
    // A CALL_1 on String is unsupported. Even after enough iterations to wrap
    // a naive uint16_t counter, recording must be attempted only once.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var hits = 0\n"
        "var i = 0\n"
        "while (i < 70000) {\n"
        "  if (\"x\".contains(\"x\")) hits = hits + 1\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(hits)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "70000") != NULL);
    assert(vm->jit->traces_aborted == 1);
    wrenFreeVM(vm);
}


TEST(test_call0_boolean_toggler_inline) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "class Box {\n"
        "  construct new(v) { _v = v }\n"
        "  toggle { _v = !_v }\n"
        "  value { _v }\n"
        "}\n"
        "var b = Box.new(false)\n"
        "var i = 0\n"
        "while (i < 10000) { b.toggle i = i + 1 }\n"
        "System.print(b.value)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "false") != NULL);
    assert(vm->jit->traces_compiled == 1);
    wrenFreeVM(vm);
}


TEST(test_fractional_loop_values_stay_double) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "var i = 0.5\n"
        "while (i < 1000) {\n"
        "  sum = sum + i\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "500000") != NULL);
    wrenFreeVM(vm);
}

TEST(test_huge_loop_value_stays_double) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var x = 100000000000000000000\n"
        "var i = 0\n"
        "while (i < 1000) {\n"
        "  x = x + 1\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(x)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "1e+20") != NULL);
    wrenFreeVM(vm);
}

int main(void) {
    printf("=== JIT Integration Tests ===\n");
    RUN(test_hot_counter_saturates_and_blacklists);
    RUN(test_unsupported_loop_is_blacklisted_once);
    RUN(test_call0_boolean_toggler_inline);
    RUN(test_fractional_loop_values_stay_double);
    RUN(test_huge_loop_value_stays_double);
    RUN(test_simple_sum);
    RUN(test_for_loop);
    RUN(test_nested_arithmetic);
    RUN(test_comparison);
    RUN(test_multiplication_loop);
    RUN(test_nested_while);
    RUN(test_hot_loop);
    RUN(test_modulo_falls_back_safely);
    RUN(test_nan_not_equal);
    RUN(test_multiple_vms);
    printf("All JIT tests passed!\n");
    return 0;
}
