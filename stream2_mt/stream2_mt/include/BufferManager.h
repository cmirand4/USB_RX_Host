#pragma once

#include <queue>
#include <mutex>
#include <vector>
#include <memory>

struct Buffer {
    std::unique_ptr<unsigned char[]> data;
    size_t size;
    size_t bytesUsed;
};

class BufferManager {
public:
    BufferManager(size_t bufferSize, int numBuffers);
    ~BufferManager();

    Buffer* GetEmptyBuffer();
    void QueueFullBuffer(Buffer* buffer);
    Buffer* GetFullBuffer();
    void ReturnEmptyBuffer(Buffer* buffer);

    bool HasEmptyBuffers() const;
    bool HasFullBuffers() const;

private:
    std::queue<Buffer*> m_emptyBuffers;
    std::queue<Buffer*> m_fullBuffers;
    std::vector<std::unique_ptr<Buffer>> m_allBuffers;

    mutable std::mutex m_emptyMutex;
    mutable std::mutex m_fullMutex;

    const size_t m_bufferSize;
};