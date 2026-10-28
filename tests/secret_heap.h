#ifndef BRIAREUS_TEST_SECRET_HEAP_H
#define BRIAREUS_TEST_SECRET_HEAP_H
#include "test.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// A sentinel is identified without ever printing its bytes. Hooks observe allocations only in the
// included fixture sources; ordinary app/transport allocations continue to use their normal allocator.
static const char heap_marker[] = "fixture-erasure-marker";
typedef struct { void *pointer; size_t size; bool sensitive; } SecretAllocation;
static SecretAllocation heap_allocations[8192];
static size_t heap_live, heap_moves, heap_unwiped, heap_erased;

static bool heap_contains(const void *pointer, size_t size) {
    size_t n = sizeof heap_marker - 1;
    if (size < n) return false;
    for (size_t i = 0; i <= size - n; i++) if (!memcmp((const char *)pointer + i, heap_marker, n)) return true;
    return false;
}
static size_t heap_index(void *pointer) {
    for (size_t i = 0; i < heap_live; i++) if (heap_allocations[i].pointer == pointer) return i;
    return heap_live;
}
static void heap_mark_sensitive(void) {
    for (size_t i = 0; i < heap_live; i++) {
        SecretAllocation *a = &heap_allocations[i];
        a->sensitive |= heap_contains(a->pointer, a->size);
    }
}
static void *heap_malloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p || heap_live == sizeof heap_allocations / sizeof *heap_allocations) abort();
    memset(p, 0xa5, size ? size : 1);
    heap_allocations[heap_live++] = (SecretAllocation){p, size ? size : 1, false};
    return p;
}
static void *heap_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) abort();
    size_t n = count * size; void *p = heap_malloc(n); memset(p, 0, n); return p;
}
static void heap_free(void *pointer) {
    if (!pointer) return;
    size_t i = heap_index(pointer);
    if (i < heap_live) {
        SecretAllocation a = heap_allocations[i];
        bool dirty = heap_contains(pointer, a.size);
        if (a.sensitive) {
            const unsigned char *p = pointer;
            bool zero = true;
            for (size_t n = 0; n < a.size; n++) zero &= p[n] == 0;
            if (zero) heap_erased++; else dirty = true;
        }
        if (dirty) heap_unwiped++;
        heap_allocations[i] = heap_allocations[--heap_live];
    }
    free(pointer);
}
static void *heap_realloc(void *pointer, size_t size) {
    // Allocate first, forcing a different address even when libc could have grown in place.
    void *next = heap_malloc(size);
    if (pointer) {
        size_t i = heap_index(pointer); CHECK(i < heap_live);
        if (i == heap_live) abort();
        size_t previous = heap_allocations[i].size;
        memcpy(next, pointer, previous < size ? previous : size); heap_moves++;
        heap_free(pointer);
    }
    return next;
}
static void heap_reset_counts(void) {
    heap_moves = heap_unwiped = heap_erased = 0;
}
#endif
