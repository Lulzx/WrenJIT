#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wren.h"

#ifdef WREN_JIT
// Include internal VM header for full WrenVM definition (needed for vm->jit access)
#include "wren_vm.h"
#include "wren_jit.h"
#endif

static void writeFn(WrenVM* vm, const char* text) {
    printf("%s", text);
}

static void errorFn(WrenVM* vm, WrenErrorType errorType,
                    const char* module, int line, const char* msg) {
    switch (errorType) {
    case WREN_ERROR_COMPILE:
        fprintf(stderr, "[%s line %d] [Error] %s\n", module, line, msg);
        break;
    case WREN_ERROR_STACK_TRACE:
        fprintf(stderr, "[%s line %d] in %s\n", module, line, msg);
        break;
    case WREN_ERROR_RUNTIME:
        fprintf(stderr, "[Runtime Error] %s\n", msg);
        break;
    }
}

static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file '%s'.\n", path);
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    size_t fileSize = (size_t)ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read '%s'.\n", path);
        fclose(file);
        return NULL;
    }

    size_t bytesRead = fread(buffer, 1, fileSize, file);
    buffer[bytesRead] = '\0';
    fclose(file);
    return buffer;
}

static double getTimeMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <script.wren> [--jit] [--no-jit]\n", argv[0]);
        return 1;
    }

    const char* path = argv[1];
    bool useJit = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--jit") == 0) useJit = true;
        if (strcmp(argv[i], "--no-jit") == 0) useJit = false;
    }

    char* source = readFile(path);
    if (source == NULL) return 1;

    WrenConfiguration config;
    wrenInitConfiguration(&config);
    config.writeFn = writeFn;
    config.errorFn = errorFn;

    WrenVM* vm = wrenNewVM(&config);

#ifdef WREN_JIT
    if (useJit && vm->jit != NULL) {
        wrenJitSetEnabled(vm->jit, true);
        fprintf(stderr, "[JIT enabled]\n");
    } else {
        if (vm->jit != NULL) wrenJitSetEnabled(vm->jit, false);
        fprintf(stderr, "[JIT disabled]\n");
    }
#else
    (void)useJit;
    fprintf(stderr, "[JIT not compiled in]\n");
#endif

    double start = getTimeMs();
    WrenInterpretResult result = wrenInterpret(vm, "main", source);
    double elapsed = getTimeMs() - start;

    if (result == WREN_RESULT_COMPILE_ERROR) {
        fprintf(stderr, "Compile error.\n");
    } else if (result == WREN_RESULT_RUNTIME_ERROR) {
        fprintf(stderr, "Runtime error.\n");
    }

    fprintf(stderr, "[Time: %.3f ms]\n", elapsed);

#ifdef WREN_JIT
    if (vm->jit != NULL) {
        fprintf(stderr, "[Traces compiled: %llu, aborted: %llu, exits: %llu]\n",
                (unsigned long long)vm->jit->traces_compiled,
                (unsigned long long)vm->jit->traces_aborted,
                (unsigned long long)vm->jit->total_exits);
    }
#endif

    wrenFreeVM(vm);
    free(source);

    return result != WREN_RESULT_SUCCESS ? 1 : 0;
}
