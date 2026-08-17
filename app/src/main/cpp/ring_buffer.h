// ring_buffer.h — 无锁 SPSC（单生产者/单消费者）字节环形缓冲。
//
// 用途：在 AAudio 实时回调线程与一个普通工作线程之间传递 PCM 数据，
// 把磁盘 I/O 完全移出实时回调。回调线程只调用非阻塞的 read()/write()；
// 工作线程侧以「轮询 + 短暂睡眠」限速，避免在回调侧引入任何锁或等待。
//
// 约束：
//   - 容量必须为 2 的幂（用位掩码取模）。
//   - 同一时刻只能有一个生产者、一个消费者（SPSC）。
//   - read()/write() 尽力而为：返回实际处理字节数，可能小于请求值。

#ifndef AAUDIOTESTER_RING_BUFFER_H_
#define AAUDIOTESTER_RING_BUFFER_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

class SpScRingBuffer {
public:
    explicit SpScRingBuffer(size_t capacity_pow2)
        : mask_(capacity_pow2 - 1), buf_(capacity_pow2) {}

    size_t capacity() const { return buf_.size(); }

    // 可读字节数（消费者侧）
    size_t readable() const {
        return write_pos_.load(std::memory_order_acquire) -
               read_pos_.load(std::memory_order_acquire);
    }

    // 可写字节数（生产者侧）
    size_t writable() const { return capacity() - readable(); }

    // 生产者：尽力写入，返回实际写入字节数（空间不足只写能装下的部分）。
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

    // 生产者：整段写入——空间足够才写，否则一个字节都不写。供「整帧要么全进、要么全丢」场景。
    bool tryWrite(const char* src, size_t n) {
        if (n > writable()) return false;
        write(src, n);
        return true;
    }

    // 消费者：尽力读取，返回实际读取字节数（数据不足只读能读到的部分）。
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

// 推荐默认容量（字节，须为 2 的幂）：1 MiB ≈ 5.5 秒 @48kHz/16bit/立体声。
static constexpr size_t kDefaultRingCapacity = 1u << 20;

#endif  // AAUDIOTESTER_RING_BUFFER_H_
