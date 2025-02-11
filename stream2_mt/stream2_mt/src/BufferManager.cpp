#include "../include/BufferManager.h"
#include <stdexcept>

BufferManager::BufferManager(size_t bufferSize, int numBuffers)
    : m_bufferSize(bufferSize)
{
    // Create all buffers and add them to the empty queue
    for (int i = 0; i < numBuffers; ++i) {
        auto buffer = std::make_unique<Buffer>();
        buffer->data = std::make_unique<unsigned char[]>(bufferSize);
        buffer->size = bufferSize;
        buffer->bytesUsed = 0;

        m_emptyBuffers.push(buffer.get());
        m_allBuffers.push_back(std::move(buffer));
    }
}

BufferManager::~BufferManager() = default;

Buffer* BufferManager::GetEmptyBuffer() {
    std::lock_guard<std::mutex> lock(m_emptyMutex);
    if (m_emptyBuffers.empty()) {
        return nullptr;
    }

    Buffer* buffer = m_emptyBuffers.front();
    m_emptyBuffers.pop();
    return buffer;
}

void BufferManager::QueueFullBuffer(Buffer* buffer) {
    std::lock_guard<std::mutex> lock(m_fullMutex);
    m_fullBuffers.push(buffer);
}

Buffer* BufferManager::GetFullBuffer() {
    std::lock_guard<std::mutex> lock(m_fullMutex);
    if (m_fullBuffers.empty()) {
        return nullptr;
    }

    Buffer* buffer = m_fullBuffers.front();
    m_fullBuffers.pop();
    return buffer;
}

void BufferManager::ReturnEmptyBuffer(Buffer* buffer) {
    std::lock_guard<std::mutex> lock(m_emptyMutex);
    buffer->bytesUsed = 0;  // Reset the used bytes count
    m_emptyBuffers.push(buffer);
}

bool BufferManager::HasEmptyBuffers() const {
    std::lock_guard<std::mutex> lock(m_emptyMutex);
    return !m_emptyBuffers.empty();
}

bool BufferManager::HasFullBuffers() const {
    std::lock_guard<std::mutex> lock(m_fullMutex);
    return !m_fullBuffers.empty();
}