#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "wren.h"

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

int main(void) {
    printf("=== JIT Integration Tests ===\n");
    RUN(test_simple_sum);
    RUN(test_for_loop);
    RUN(test_nested_arithmetic);
    RUN(test_comparison);
    RUN(test_multiplication_loop);
    RUN(test_nested_while);
    RUN(test_hot_loop);
    RUN(test_multiple_vms);
    printf("All JIT tests passed!\n");
    return 0;
}
