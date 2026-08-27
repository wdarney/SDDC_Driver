#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <array>
#include <vector>
#include <cstring>

#include "../config.h"

namespace {
    const int default_count = 64;
    const int spin_count = 100;
    #define ALIGN (8)
};

template<typename T, size_t max_count = default_count> class ringbuffer {
    typedef T* TPtr;

public:
    ringbuffer():
        read_index(0),
        write_index(0),
        blocks_available(0),
        emptyCount(0),
        fullCount(0),
        writeCount(0),
        stopped(false)
    {
    }

    ~ringbuffer()
    {
        TracePrintln("ringbuffer", "");

        Stop();
    }

    int getFullCount() const { return fullCount.load(); }

    int getEmptyCount() const { return emptyCount.load(); }

    int getWriteCount() const { return writeCount.load(); }

    void Start()
    {
        std::unique_lock<std::mutex> lk(mutex);
        write_index = read_index = 0;
        blocks_available = 0;
        stopped = false;
    }

    void Stop()
    {
        std::unique_lock<std::mutex> lk(mutex);
        stopped = true;
        nonfullCV.notify_all();
        nonemptyCV.notify_all();
    }

    void setBlockSize(int size)
    {
        TracePrintln("ringbuffer", "");

        if (block_size != size)
        {
            block_size = size;

            int aligned_block_size = (block_size + ALIGN - 1) & (~(ALIGN - 1));

            DebugPrintln("ringbuffer", "New raw buffer size : %ld", max_count * aligned_block_size);

            for(auto it = buffers.begin(); it < buffers.end(); it++)
            {
                it->resize(aligned_block_size);
            }
        }
    }

    T* peekWritePtr(int offset)
    {
        return buffers[(write_index.load() + max_count + offset) % max_count].data();
    }

    T* peekReadPtr(int offset)
    {
        return buffers[(read_index.load() + max_count + offset) % max_count].data();
    }

    // These acquire/commit APIs are intentionally SPSC: callers may hold one
    // acquired block without the ring mutex while the opposite endpoint uses a
    // different block. The block remains owned by the caller until commit or
    // release advances its index.
    T* acquireWriteBlock()
    {
        for (int i = 0; i < spin_count; i++)
        {
            if (stopped.load(std::memory_order_acquire)) return nullptr;
            if (blocks_available.load(std::memory_order_acquire) < max_count)
                return buffers[write_index.load(std::memory_order_relaxed)].data();
        }

        std::unique_lock<std::mutex> lk(mutex);
        if (blocks_available >= max_count) fullCount++;
        nonfullCV.wait(lk, [this] {
            return stopped.load(std::memory_order_relaxed) || blocks_available < max_count;
        });
        if (stopped.load(std::memory_order_relaxed)) return nullptr;
        return buffers[write_index.load(std::memory_order_relaxed)].data();
    }

    bool commitWriteBlock()
    {
        std::unique_lock<std::mutex> lk(mutex);
        if (stopped.load(std::memory_order_relaxed)) return false;

        write_index = (write_index.load(std::memory_order_relaxed) + 1) % max_count;
        blocks_available++;
        writeCount++;
        lk.unlock();
        nonemptyCV.notify_one();
        return true;
    }

    const T* acquireReadBlock()
    {
        for (int i = 0; i < spin_count; i++)
        {
            if (stopped.load(std::memory_order_acquire)) return nullptr;
            if (blocks_available.load(std::memory_order_acquire) > 0)
                return buffers[read_index.load(std::memory_order_relaxed)].data();
        }

        std::unique_lock<std::mutex> lk(mutex);
        if (blocks_available <= 0) emptyCount++;
        nonemptyCV.wait(lk, [this] {
            return stopped.load(std::memory_order_relaxed) || blocks_available > 0;
        });
        if (stopped.load(std::memory_order_relaxed)) return nullptr;
        return buffers[read_index.load(std::memory_order_relaxed)].data();
    }

    void releaseReadBlock()
    {
        std::unique_lock<std::mutex> lk(mutex);
        if (blocks_available == 0) return;

        read_index = (read_index.load(std::memory_order_relaxed) + 1) % max_count;
        blocks_available--;
        lk.unlock();
        nonfullCV.notify_one();
    }

    void push(const std::vector<T>& arr)
    {
        T* dest = acquireWriteBlock();
        if (dest == nullptr) return;
        std::memcpy(dest, arr.data(), block_size * sizeof(T));
        commitWriteBlock();
    }

    std::vector<T> pop()
    {
        const T* source = acquireReadBlock();
        if (source == nullptr) return {};
        std::vector<T> vec(source, source + block_size);
        releaseReadBlock();
        return vec;
    }

    int getBlockSize() const { return block_size; }

    void WaitUntilNotEmpty()
    {
        if (stopped) return;

        // if not empty
        for (int i = 0; i < spin_count; i++)
        {
            if (blocks_available > 0)
                return;
        }

        if(blocks_available <= 0)
        {
            std::unique_lock<std::mutex> lk(mutex);

            emptyCount++;
            nonemptyCV.wait(lk, [this] {
                return blocks_available > 0;
            });
        }
    }

    void WaitUntilNotFull()
    {
        if (stopped) return;

        for (int i = 0; i < spin_count; i++)
        {
            if (blocks_available < max_count)
                return;
        }

        if (blocks_available >= max_count)
        {
            std::unique_lock<std::mutex> lk(mutex);
            fullCount++;
            nonfullCV.wait(lk, [this] {
                return blocks_available < max_count;
            });
        }
    }

    std::atomic<size_t> read_index;
    std::atomic<size_t> write_index;
    std::atomic<size_t> blocks_available;

private:
    std::atomic<int> emptyCount;
    std::atomic<int> fullCount;
    std::atomic<int> writeCount;

    std::mutex mutex;
    std::atomic<bool> stopped;
    std::condition_variable nonemptyCV;
    std::condition_variable nonfullCV;

    int block_size = 0;

    array<vector<T>, max_count> buffers;
};
