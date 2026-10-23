/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Damien P. George
 * Copyright (c) 2017 Pycom Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/gc.h"
#include "py/mpthread.h"
#include "gccollect.h"

#if CONFIG_IDF_TARGET_ARCH_XTENSA

#include "xtensa/hal.h"

// The level argument must be volatile to force the compiler to emit code that
// will call this function recursively, to nest the C stack.
static void gc_collect_inner(volatile unsigned int level) {
    if (level < XCHAL_NUM_AREGS / 8) {
        // Go deeper on the stack to spill more registers from the register window.
        gc_collect_inner(level + 1);
    } else {
        // Deep enough so that all registers are on the C stack, now trace the stack.
        volatile uint32_t sp = (uint32_t)esp_cpu_get_sp();
        gc_collect_root((void **)sp, ((mp_uint_t)MP_STATE_THREAD(stack_top) - sp) / sizeof(uint32_t));
    }
}

void gc_collect(void) {
    gc_collect_start();
    gc_collect_inner(0);
    #if MICROPY_PY_THREAD
    mp_thread_gc_others();
    #endif
    gc_collect_end();
}

#elif CONFIG_IDF_TARGET_ARCH_RISCV
#include "shared/runtime/gchelper.h"

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    #if MICROPY_PY_THREAD
    mp_thread_gc_others();
    #endif
    gc_collect_end();
}

#else
#error unknown CONFIG_IDF_TARGET_ARCH
#endif

#if MICROPY_GC_SPLIT_HEAP_AUTO

// Default internal heap headroom. Boards can override this; Python can change
// it at runtime using esp32.gc_heap_reserve(). This is not a dedicated BLE pool.
#ifndef MICROPY_GC_SPLIT_HEAP_AUTO_RESERVE
#define MICROPY_GC_SPLIT_HEAP_AUTO_RESERVE (0)
#endif

// The initial heap is allocated before esp32_gc_reset(), without a reserve.
static size_t gc_heap_reserve;

void esp32_gc_reset(void) {
    gc_heap_reserve = MICROPY_GC_SPLIT_HEAP_AUTO_RESERVE;
}

size_t esp32_gc_get_reserve(void) {
    return gc_heap_reserve;
}

void esp32_gc_set_reserve(size_t reserve) {
    gc_heap_reserve = reserve;
}

static size_t gc_get_max_new_internal(void) {
    uint32_t caps = MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL;
    size_t free_bytes = heap_caps_get_free_size(caps);
    if (free_bytes <= gc_heap_reserve) {
        return 0;
    }
    // This is a best-effort limit: allocator overhead and concurrent native
    // allocations can reduce the remaining free space below the reserve.
    size_t largest = heap_caps_get_largest_free_block(caps);
    return MIN(largest, free_bytes - gc_heap_reserve);
}

size_t gc_get_max_new_split(void) {
    if (gc_heap_reserve == 0) {
        return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    }
    size_t external = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM);
    size_t internal = gc_get_max_new_internal();
    return MAX(external, internal);
}

void *esp32_gc_alloc(size_t size) {
    if (gc_heap_reserve == 0) {
        return malloc(size);
    }
    // Explicit capabilities prevent malloc's automatic fallback from PSRAM to
    // internal RAM from consuming the reserve. Recheck before allocating.
    uint32_t external_caps = MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM;
    if (size <= heap_caps_get_largest_free_block(external_caps)) {
        void *ptr = heap_caps_malloc(size, external_caps);
        if (ptr != NULL) {
            return ptr;
        }
    }
    if (size <= gc_get_max_new_internal()) {
        return heap_caps_malloc(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
    }
    return NULL;
}

#endif
