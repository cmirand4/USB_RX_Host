#pragma once

// Windows headers must come before Cypress headers
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Prevent Windows USB definitions from conflicting with Cypress
#define _USB_H_
#define __USB_H__
#define _WINUSB_H_
#define __WINUSB_H__

#include <setupapi.h>     // Required for Windows Setup API

// Now we can include Cypress headers
#include "CyAPI.h"

// Standard library includes
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <memory>
#include <fstream>
#include <atomic>

class BufferManager;

class DataStreamer {
public:
    DataStreamer();
    ~DataStreamer();

    bool Initialize(size_t totalBytes);
    bool StartStreaming();
    void StopStreaming();
    bool IsComplete() const { return m_totalBytesWritten >= m_targetBytes; }

private:
    void UsbReaderThread();
    void DiskWriterThread();

    // USB device management
    std::unique_ptr<CCyUSBDevice> m_usbDevice;
    CCyBulkEndPoint* m_bulkEndpoint;

    // Threading components
    std::unique_ptr<std::thread> m_readerThread;
    std::unique_ptr<std::thread> m_writerThread;
    std::mutex m_mutex;
    std::condition_variable m_dataReady;
    std::atomic<bool> m_running;

    // Buffer management
    std::unique_ptr<BufferManager> m_bufferManager;

    // File handling
    std::ofstream m_outFile;

    // Updated constants for better performance
    static constexpr size_t BUFFER_SIZE = (512 * 512) & ~0x3;  // Aligned to 4-byte boundary
    static constexpr int NUM_BUFFERS = 4;  // Reduced from 8 to 4 for optimal performance
    static constexpr size_t FLUSH_THRESHOLD = 8 * 1024 * 1024;  // Flush every 8MB
    static constexpr DWORD USB_TIMEOUT = 10000;  // 10 second timeout
    
    // Track total bytes transferred
    size_t m_targetBytes;
    std::atomic<size_t> m_totalBytesWritten;
};