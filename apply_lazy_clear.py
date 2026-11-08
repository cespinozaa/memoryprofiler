# apply_lazy_clear.py: patch ggml so that clearing a big CPU buffer to zero drops its
# pages (they read back as zeros) instead of writing zeros into every page
# python3 apply_lazy_clear.py ~/llama.cpp/ggml/src/ggml-backend.cpp

import sys

path = sys.argv[1]
src = open(path).read()
if "ggml_backend_cpu_buffer_clear_anon" in src:
    sys.exit("already patched")

old_fn = ("static void ggml_backend_cpu_buffer_clear(ggml_backend_buffer_t buffer, uint8_t value) {\n"
          "    GGML_ASSERT(buffer);\n"
          "    memset(buffer->context, value, buffer->size);\n"
          "}\n")
if old_fn not in src:
    sys.exit("could not find ggml_backend_cpu_buffer_clear: the source differs from what was expected")

new_fn = r'''
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

// Clearing a big buffer to zero by writing zeros touches every page and makes all of it
// resident. For plain anonymous memory (what ggml_aligned_malloc returns) it is enough to
// drop the whole pages in the middle with madvise(MADV_DONTNEED): they read back as zeros
// and only become resident when they are actually written. The partial pages at the two
// ends are cleared normally.
static void ggml_backend_cpu_buffer_clear_anon(ggml_backend_buffer_t buffer, uint8_t value) {
    GGML_ASSERT(buffer);
#if defined(__linux__)
    if (value == 0 && buffer->size >= (1u << 20)) {
        const uintptr_t page  = (uintptr_t) sysconf(_SC_PAGESIZE);
        const uintptr_t begin = (uintptr_t) buffer->context;
        const uintptr_t end   = begin + buffer->size;
        const uintptr_t first = (begin + page - 1) & ~(page - 1); // first page boundary inside the buffer
        const uintptr_t last  = end & ~(page - 1);                // last page boundary inside the buffer
        if (last > first && madvise((void *) first, last - first, MADV_DONTNEED) == 0) {
            memset((void *) begin, 0, first - begin);
            memset((void *) last, 0, end - last);
            return;
        }
    }
#endif
    memset(buffer->context, value, buffer->size);
}
'''
src = src.replace(old_fn, old_fn + new_fn)

# use it only in the buffer type that owns its memory (it frees it), never in the one
# that wraps memory it does not own (a memory-mapped model file)
marker = "/* .clear           = */ ggml_backend_cpu_buffer_clear,"
idx = src.find(marker)
begin = src.rfind("= {", 0, idx)
if idx < 0 or "ggml_backend_cpu_buffer_free_buffer" not in src[begin:idx]:
    sys.exit("the first .clear entry is not in the buffer type that owns its memory: stopping, nothing changed")
src = src[:idx] + marker.replace("buffer_clear,", "buffer_clear_anon,") + src[idx + len(marker):]

open(path, "w").write(src)
print("patched", path)