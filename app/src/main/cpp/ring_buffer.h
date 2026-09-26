// ring_buffer.h — Lock-free SPSC (single-producer/single-consumer) byte ring buffer.
//
// Purpose: transfer PCM data between the AAudio real-time callback thread and a
// regular worker thread, moving disk I/O completely out of the real-time callback.
// The callback thread only calls non-blocking read()/write();
// the worker thread throttles via "polling + brief sleep", avoiding any lock or wait
// on the callback side.
//
// Constraints:
//   - Capacity must be a power of 2 (modulo via bitmask).
//   - Only one producer and one consumer at a time (SPSC).
//   - read()/write() are best-effort: they return the actual number of bytes processed,
//     which may be less than requested.

#ifndef AAUDIOTESTER_RING_BUFFER_H_
#define AAUDIOTESTER_RING_BUFFER_H_

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>

class SpScRingBuffer {
public:
    explicit SpScRingBuffer(size_t capacity_pow2)
        : mask_(capacity_pow2 - 1), buf_(capacity_pow2) {
        // The mask/writable arithmetic below is only correct for a positive power of two:
        // anything else misaligns the index mapping and can underflow writable() into a
        // huge size_t, turning the bounded memcpy into an out-of-bounds write.
        assert(capacity_pow2 > 0 && (capacity_pow2 & (capacity_pow2 - 1)) == 0);
    }

    size_t capacity() const { return buf_.size(); }

    // Bytes available for reading (consumer side)
    size_t readable() const {
        return write_pos_.load(std::memory_order_acquire) -
               read_pos_.load(std::memory_order_acquire);
    }

    // Bytes available for writing (producer side)
    size_t writable() const { return capacity() - readable(); }

    // Producer: best-effort write, returns actual bytes written (only writes what fits if space is low).
    size_t write(const char* src, size_t n) {
        n = std::min(n, writable());
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        const size_t idx = w & mask_;
        const size_t first = std::min(n, capacity() - idx);
        std::memcpy(buf_.data() + idx, src, first);
        std::memcpy(buf_.data(), src + first, n - first);
        write_pos_.store(w + n, std::memory_order_release);
        return n;
    }

    // Producer: all-or-nothing write — only writes if enough space, otherwise writes nothing. For "whole frame in or drop" scenarios.
    bool tryWrite(const char* src, size_t n) {
        if (n > writable()) return false;
        write(src, n);
        return true;
    }

    // Consumer: best-effort read, returns actual bytes read (only reads what is available if data is low).
    size_t read(char* dst, size_t n) {
        n = std::min(n, readable());
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t idx = r & mask_;
        const size_t first = std::min(n, capacity() - idx);
        std::memcpy(dst, buf_.data() + idx, first);
        std::memcpy(dst + first, buf_.data(), n - first);
        read_pos_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    const size_t mask_;
    std::vector<char> buf_;
    std::atomic<size_t> read_pos_{0};
    std::atomic<size_t> write_pos_{0};
};

// Recommended default capacity (bytes, must be a power of 2): 1 MiB ≈ 5.5 s @48kHz/16bit/stereo.
static constexpr size_t kDefaultRingCapacity = 1u << 20;

#endif  // AAUDIOTESTER_RING_BUFFER_H_
