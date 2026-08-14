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
    assert(vm->jit->traces_compiled >= 1);
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


TEST(test_integer_modulo) {
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
    assert(vm->jit->traces_compiled == 1);
    wrenFreeVM(vm);
}

TEST(test_fractional_modulo_side_exits) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < 100) {\n"
        "  sum = sum + ((i + 0.5) % 7)\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "345") != NULL);
    assert(vm->jit->traces_compiled == 1);
    assert(vm->jit->total_exits > 0);
    wrenFreeVM(vm);
}

TEST(test_native_sqrt_and_floor) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var roots = 0\n"
        "var i = 1\n"
        "while (i <= 1000) {\n"
        "  roots = roots + (i * i).sqrt\n"
        "  i = i + 1\n"
        "}\n"
        "var floors = 0\n"
        "i = 0\n"
        "while (i < 1000) {\n"
        "  floors = floors + (i + 0.75).floor\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(\"%(roots),%(floors)\")\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "500500,499500") != NULL);
    assert(vm->jit->traces_compiled == 2);
    wrenFreeVM(vm);
}

TEST(test_native_bitwise_ops) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < 1000) {\n"
        "  sum = sum + ((1 << (i % 4)) & 15)\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "3750") != NULL);
    assert(vm->jit->traces_compiled == 1);
    wrenFreeVM(vm);
}

TEST(test_list_index_load_store) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var values = []\n"
        "for (i in 0...256) values.add(i)\n"
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < 1000) {\n"
        "  var index = i % 256\n"
        "  values[index] = values[index] + 1\n"
        "  sum = sum + values[index]\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(\"%(sum),%(values[0]),%(values[255])\")\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "127180,4,258") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    wrenFreeVM(vm);
}

TEST(test_list_length_bound_stays_numeric) {
    // A promoted integer loop counter compared with an invariant numeric list
    // length must not make codegen read the counter from an FP allocation.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var values = []\n"
        "for (i in 0...10000) values.add(i % 7)\n"
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < values.count) {\n"
        "  sum = sum + values[i]\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "29994") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    wrenFreeVM(vm);
}

TEST(test_list_bounds_hoisted_out_of_counted_loop) {
    // A while loop with an invariant list, an ascending integral index PHI,
    // and an invariant numeric limit must hoist the list bounds check to a
    // single IR_LIST_BOUNDS_GUARD at loop entry.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var values = []\n"
        "for (i in 0...1000) values.add(i % 7)\n"
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < 1000) {\n"
        "  sum = sum + values[i]\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "2997") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    bool hoisted = false;
    for (uint32_t i = 0; i < vm->jit->trace_capacity; i++) {
        JitTrace* trace = &vm->jit->traces[i];
        if (trace->anchor_pc != NULL && trace->bounds_hoisted_guards > 0) {
            hoisted = true;
            break;
        }
    }
    assert(hoisted);
    wrenFreeVM(vm);
}

TEST(test_list_bounds_hoisted_guard_fires_out_of_bounds) {
    // A trace compiled against a long list must still throw when the same
    // loop runs against a shorter list: the hoisted count guard side-exits
    // and the interpreter reproduces the out-of-bounds error.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "var run = Fn.new { |values, n|\n"
        "  var i = 0\n"
        "  while (i < n) {\n"
        "    sum = sum + values[i]\n"
        "    i = i + 1\n"
        "  }\n"
        "}\n"
        "var long = []\n"
        "for (i in 0...1000) long.add(i % 7)\n"
        "run.call(long, 1000)\n"
        "System.print(sum)\n"
        "var short = [1, 2, 3]\n"
        "run.call(short, 1000)\n"
        "System.print(sum)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_RUNTIME_ERROR);
    wrenFreeVM(vm);
}

TEST(test_nested_loop_exit_stays_in_trace) {
    // The recorder opens the inner loop with an IR_LOOP_HEADER and patches its
    // exit tests to jump in-trace past the loop (outer closure). If that patch
    // regressed, the inner loop's termination would deopt to the interpreter on
    // every outer iteration -- ~200k exits instead of a handful.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var total = 0\n"
        "var i = 0\n"
        "while (i < 200000) {\n"
        "  var j = 0\n"
        "  while (j < 4) {\n"
        "    total = total + 1\n"
        "    j = j + 1\n"
        "  }\n"
        "  total = total + i\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(total)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "20000700000") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    assert(vm->jit->total_exits < 1000);
    wrenFreeVM(vm);
}

TEST(test_nested_loop_conditionally_skipped_stays_in_trace) {
    // The inner loop is entered on 3 of every 4 outer iterations and is empty
    // on the fourth. Its entry branch (recorded as a deopt-capable IR_LOOP_EXIT
    // until a back edge proves it a loop) must be patched to jump in-trace past
    // the loop so an empty inner loop skips its body without deopting.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var total = 0\n"
        "var i = 0\n"
        "while (i < 200000) {\n"
        "  var r = i % 4\n"
        "  var j = 0\n"
        "  while (j < r) {\n"
        "    total = total + 1\n"
        "    j = j + 1\n"
        "  }\n"
        "  total = total + i\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(total)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "20000200000") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    assert(vm->jit->total_exits < 1000);
    wrenFreeVM(vm);
}

TEST(test_degenerate_trace_re_recorded) {
    // A trace recorded on a path that skipped a nested loop deopts on the
    // common path (the fasta pattern). After enough such handoff exits the
    // trace is retired, its loop's hot counter is re-armed, and the next pass
    // re-records through the loop. The re-record must yield a healthy trace:
    // bounded exits for 300k outer iterations, not an exit per iteration.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var cumulative = [0.27, 0.39, 0.51, 0.78, 0.80, 0.82, 0.84,\n"
        "    0.86, 0.88, 0.90, 0.92, 0.94, 0.96, 0.98, 1]\n"
        "var seed = 42\n"
        "var total = 0\n"
        "var i = 0\n"
        "while (i < 300000) {\n"
        "  seed = (seed * 3877 + 29573) % 139968\n"
        "  var r = seed / 139968\n"
        "  var j = 0\n"
        "  while (r >= cumulative[j]) { j = j + 1 }\n"
        "  total = total + j\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(total)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "945958") != NULL);
    assert(vm->jit->re_records_done >= 1);
    assert(vm->jit->total_exits < 100000);
    // A later re-record can overwrite the disabled trace's table slot, so the
    // retire itself (re_records_done) is the durable signal; the disabled flag
    // on a specific slot is not guaranteed to survive.
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
        "while (i < 10000) {\n"
        "  b.toggle\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(b.value)\n";
    WrenInterpretResult result = wrenInterpret(vm, "main", src);
    assert(result == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "false") != NULL);
    assert(vm->jit->traces_compiled == 1);
    bool hasTraceRoot = false;
    for (uint32_t i = 0; i < vm->jit->trace_capacity; i++) {
        JitTrace* trace = &vm->jit->traces[i];
        if (trace->anchor_pc != NULL && trace->num_gc_roots > 0) {
            hasTraceRoot = true;
            break;
        }
    }
    assert(hasTraceRoot);
    wrenCollectGarbage(vm); // Native embedded pointers participate in marking.
    wrenFreeVM(vm);
}


TEST(test_call0_toggler_returning_this_inline) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "class Toggle {\n"
        "  construct new(v) { _v = v }\n"
        "  value { _v }\n"
        "  activate {\n    _v = !_v\n    return this\n  }\n"
        "}\n"
        "var t = Toggle.new(true)\n"
        "var count = 0\n"
        "var i = 0\n"
        "while (i < 10000) {\n"
        "  if (t.activate.value) count = count + 1\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(count)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "5000") != NULL);
    assert(vm->jit->traces_compiled == 1);
    wrenFreeVM(vm);
}

TEST(test_toggle_counter_fast_forward_odd) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "class Toggle {\n"
        "  construct new(v) { _v = v }\n"
        "  value { _v }\n"
        "  activate {\n    _v = !_v\n    return this\n  }\n"
        "}\n"
        "var t = Toggle.new(false)\n"
        "var count = 0\n"
        "for (i in 1..10001) {\n"
        "  if (t.activate.value) count = count + 1\n"
        "}\n"
        "System.print(count)\n"
        "System.print(t.value)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "5001\ntrue") != NULL);
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

// One loop PC is reached with many different ranges, and the receiver guard
// only proves the receiver is a Range. A trace that baked the recorded range's
// bounds, direction or inclusivity into constants reused them for every later
// range: a shorter range overran its end, an exclusive range overshot by one,
// and a reversed range stepped away from its limit and never terminated.
TEST(test_range_trace_reused_across_shapes) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "class T {\n"
        "  static count(r) {\n"
        "    var n = 0\n"
        "    for (x in r) { n = n + 1 }\n"
        "    return n\n"
        "  }\n"
        "}\n"
        // Compiles a trace for an ascending, inclusive range of 100.
        "var warm = T.count(1..100)\n"
        // Shorter bound: must not run on to the recorded limit.
        "var shorter = T.count(1..60)\n"
        // Longer bound: must not stop at the recorded limit.
        "var longer = T.count(1..200)\n"
        // Exclusive: must not include the endpoint.
        "var exclusive = T.count(1...100)\n"
        // Descending: baking step=+1 here spins forever.
        "var descending = T.count(100..1)\n"
        "System.print(\"%(warm),%(shorter),%(longer),%(exclusive),%(descending)\")\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "100,60,200,99,100") != NULL);
    wrenFreeVM(vm);
}

TEST(test_range_loop_stack_promotion) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var fractional = 0\n"
        "for (i in 0.5..3.5) fractional = fractional + i\n"
        "var descending = 0\n"
        "for (i in 3..1) descending = descending + i\n"
        "var stopped = 0\n"
        "for (i in 1..100) {\n"
        "  if (i == 60) break\n"
        "  stopped = stopped + i\n"
        "}\n"
        "System.print(\"%(fractional),%(descending),%(stopped)\")\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "8,6,1770") != NULL);
    wrenFreeVM(vm);
}

TEST(test_integer_comparison_specialization) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var i = 0\n"
        "var count = 0\n"
        "while (i <= 1000) {\n"
        "  if (i == 500) count = count + 1\n"
        "  if (i != 500) count = count + 1\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(count)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "1001") != NULL);
    assert(vm->jit->traces_compiled == 1);
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

TEST(test_map_null_count_select_covers_both_arms) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var counts = {}\n"
        "var i = 0\n"
        "while (i < 20000) {\n"
        "  var key = i % 97\n"
        "  var old = counts[key]\n"
        "  counts[key] = old == null ? 1 : old + 1\n"
        "  i = i + 1\n"
        "}\n"
        "var sum = 0\n"
        "for (value in counts.values) sum = sum + value\n"
        "System.print(\"%(sum),%(counts.count)\")\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "20000,97") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    // Resize exits are logarithmic. A branch-specialized ternary would exit
    // on nearly every repeated key after the first 97 insertions.
    assert(vm->jit->total_exits < 200);
    wrenFreeVM(vm);
}

TEST(test_nested_list_iterators_cross_inner_loop) {
    // A list-for iterator PHI may be created before an intervening inner-loop
    // header. Its back edge must still update that PHI; otherwise the native
    // list loop repeats its first element forever (the regex-redux shape).
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var rows = [[1, 2], [3, 4]]\n"
        "var total = 0\n"
        "var i = 0\n"
        "while (i < 5000) {\n"
        "  for (row in rows) {\n"
        "    for (value in row) {\n"
        "      var j = 0\n"
        "      while (j < value) j = j + 1\n"
        "      total = total + j\n"
        "    }\n"
        "  }\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(total)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "50000") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    wrenFreeVM(vm);
}

TEST(test_short_inner_loop_records_after_retry) {
    // Recording that starts on a short inner loop's terminating back-edge
    // aborts once ("recording started at loop completion"), re-arms the hot
    // counter, and records on the next fresh entry. The retry must be bounded
    // and the loop must still produce the correct result.
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var total = 0\n"
        "var i = 0\n"
        "while (i < 20000) {\n"
        "  var j = 0\n"
        "  while (j < 1) { j = j + 1 }\n"
        "  total = total + i\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(total)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "199990000") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    assert(vm->jit->traces_aborted <= 4);
    wrenFreeVM(vm);
}

TEST(test_snapshot_only_number_materializes_on_exit) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "var sum = 0\n"
        "var i = 0\n"
        "while (i < 200) {\n"
        "  var x = i * 1.5 + 0.25\n"
        "  if (i == 123) System.print(x)\n"
        "  sum = sum + x\n"
        "  i = i + 1\n"
        "}\n"
        "System.print(sum)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "184.75") != NULL);
    assert(strstr(output_buf, "29900") != NULL);
    assert(vm->jit->traces_compiled >= 1);
    assert(vm->jit->total_exits >= 1);
    wrenFreeVM(vm);
}

TEST(test_recursive_numeric_kernel) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "class Fib {\n"
        "  static compute(n) {\n"
        "    if (n < 2) return n\n"
        "    return Fib.compute(n - 1) + Fib.compute(n - 2)\n"
        "  }\n"
        "}\n"
        "System.print(Fib.compute(20))\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "6765") != NULL);
    assert(vm->jit->traces_compiled == 1);
    wrenFreeVM(vm);
}

TEST(test_recursive_tree_kernels) {
    resetOutput();
    WrenVM* vm = createVM();
    const char* src =
        "class Node {\n"
        "  construct new(left, right) {\n    _left = left\n    _right = right\n  }\n"
        "  check {\n"
        "    if (_left == null) return 1\n"
        "    return 1 + _left.check + _right.check\n"
        "  }\n"
        "}\n"
        "class Trees {\n"
        "  static make(depth) {\n"
        "    if (depth == 0) return Node.new(null, null)\n"
        "    return Node.new(Trees.make(depth - 1), Trees.make(depth - 1))\n"
        "  }\n"
        "}\n"
        "System.print(Trees.make(10).check)\n";
    assert(wrenInterpret(vm, "main", src) == WREN_RESULT_SUCCESS);
    assert(strstr(output_buf, "2047") != NULL);
    assert(vm->jit->traces_compiled == 2);
    wrenFreeVM(vm);
}

int main(void) {
    printf("=== JIT Integration Tests ===\n");
    RUN(test_hot_counter_saturates_and_blacklists);
    RUN(test_unsupported_loop_is_blacklisted_once);
    RUN(test_call0_boolean_toggler_inline);
    RUN(test_call0_toggler_returning_this_inline);
    RUN(test_toggle_counter_fast_forward_odd);
    RUN(test_fractional_loop_values_stay_double);
    RUN(test_nested_loop_exit_stays_in_trace);
    RUN(test_nested_loop_conditionally_skipped_stays_in_trace);
    RUN(test_degenerate_trace_re_recorded);
    RUN(test_range_trace_reused_across_shapes);
    RUN(test_range_loop_stack_promotion);
    RUN(test_integer_comparison_specialization);
    RUN(test_huge_loop_value_stays_double);
    RUN(test_map_null_count_select_covers_both_arms);
    RUN(test_nested_list_iterators_cross_inner_loop);
    RUN(test_short_inner_loop_records_after_retry);
    RUN(test_snapshot_only_number_materializes_on_exit);
    RUN(test_recursive_numeric_kernel);
    RUN(test_recursive_tree_kernels);
    RUN(test_simple_sum);
    RUN(test_for_loop);
    RUN(test_nested_arithmetic);
    RUN(test_comparison);
    RUN(test_multiplication_loop);
    RUN(test_nested_while);
    RUN(test_hot_loop);
    RUN(test_integer_modulo);
    RUN(test_fractional_modulo_side_exits);
    RUN(test_native_sqrt_and_floor);
    RUN(test_native_bitwise_ops);
    RUN(test_list_index_load_store);
    RUN(test_list_length_bound_stays_numeric);
    RUN(test_list_bounds_hoisted_out_of_counted_loop);
    RUN(test_list_bounds_hoisted_guard_fires_out_of_bounds);
    RUN(test_nan_not_equal);
    RUN(test_multiple_vms);
    printf("All JIT tests passed!\n");
    return 0;
}
