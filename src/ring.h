#pragma once
#include <atomic>
#include <stdint.h>
#include <string.h>

// Lock-free single-producer / single-consumer byte ring. Safe between the
// two cores, and between a core and an ISR, as long as there is exactly one
// writer and one reader. Capacity must be a power of two.
template <uint32_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "ring capacity must be a power of two");

public:
    uint32_t used() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    uint32_t space() const { return N - used(); }
    static constexpr uint32_t capacity() { return N; }

    // Producer side. All-or-nothing: returns false (and writes nothing) when
    // there is not room for all n bytes.
    bool write(const void *src, uint32_t n) {
        uint32_t h = head_.load(std::memory_order_relaxed);
        uint32_t t = tail_.load(std::memory_order_acquire);
        if (N - (h - t) < n) return false;
        uint32_t off = h & (N - 1);
        uint32_t first = (n < N - off) ? n : N - off;
        memcpy(buf_ + off, src, first);
        memcpy(buf_, (const uint8_t *)src + first, n - first);
        head_.store(h + n, std::memory_order_release);
        return true;
    }

    // Consumer side. Reads up to n bytes, returns the count read.
    uint32_t read(void *dst, uint32_t n) {
        uint32_t t = tail_.load(std::memory_order_relaxed);
        uint32_t h = head_.load(std::memory_order_acquire);
        uint32_t avail = h - t;
        if (n > avail) n = avail;
        uint32_t off = t & (N - 1);
        uint32_t first = (n < N - off) ? n : N - off;
        memcpy(dst, buf_ + off, first);
        memcpy((uint8_t *)dst + first, buf_, n - first);
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    // Consumer side: discard everything currently queued.
    void drop_all() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    uint8_t buf_[N];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
};
