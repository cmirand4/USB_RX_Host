#include "../include/DataStreamer.h"
#include "../include/BufferManager.h"
#include <iostream>
#include <windows.h>

DataStreamer::DataStreamer()
    : m_usbDevice(nullptr)
    , m_bulkEndpoint(nullptr)
    , m_running(false)
{
}

DataStreamer::~DataStreamer() {
    StopStreaming();
}

bool DataStreamer::Initialize() {
    // Create USB device object
    m_usbDevice = std::make_unique<CCyUSBDevice>(nullptr);

    // Open the first available device
    if (!m_usbDevice->Open(0)) {
        std::cerr << "Failed to open USB device" << std::endl;
        return false;
    }

    // Get bulk endpoint
    m_bulkEndpoint = m_usbDevice->BulkInEndPt;
    if (!m_bulkEndpoint) {
        std::cerr << "Failed to get bulk endpoint" << std::endl;
        return false;
    }

    // Configure endpoint with improved settings
    m_bulkEndpoint->TimeOut = USB_TIMEOUT;
    m_bulkEndpoint->SetXferSize(BUFFER_SIZE);

    // Print endpoint details for debugging
    std::cout << "Endpoint Address: 0x" << std::hex 
              << static_cast<int>(m_bulkEndpoint->Address) << std::dec << std::endl;
    std::cout << "Max Packet Size: " << m_bulkEndpoint->MaxPktSize << " bytes" << std::endl;

    // Create buffer manager
    m_bufferManager = std::make_unique<BufferManager>(BUFFER_SIZE, NUM_BUFFERS);

    // Configure file buffer size for more frequent writes
    m_outFile.rdbuf()->pubsetbuf(nullptr, 16 * 1024);  // 16KB buffer size

    // Open output file
    m_outFile.open("C:/Users/cmirand4/Documents/MATLAB/VI_Data/streamTest/counter2.bin",
        std::ios::binary | std::ios::out);

    if (!m_outFile.is_open()) {
        std::cerr << "Failed to open output file" << std::endl;
        return false;
    }

    return true;
}

bool DataStreamer::StartStreaming() {
    if (m_running) {
        return false;
    }

    m_running = true;

    // Create reader and writer threads
    try {
        m_readerThread = std::make_unique<std::thread>(&DataStreamer::UsbReaderThread, this);
        m_writerThread = std::make_unique<std::thread>(&DataStreamer::DiskWriterThread, this);
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to create threads: " << e.what() << std::endl;
        m_running = false;
        return false;
    }

    return true;
}

void DataStreamer::StopStreaming() {
    m_running = false;
    m_dataReady.notify_all();

    if (m_readerThread && m_readerThread->joinable()) {
        m_readerThread->join();
    }
    if (m_writerThread && m_writerThread->joinable()) {
        m_writerThread->join();
    }

    m_outFile.close();
}

void DataStreamer::UsbReaderThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Pre-allocate overlapped structures and start initial transfers
    std::vector<OVERLAPPED> ovLapArray(NUM_BUFFERS);
    std::vector<Buffer*> activeBuffers(NUM_BUFFERS, nullptr);
    
    // Initialize overlapped structures and start initial transfers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        Buffer* buffer = m_bufferManager->GetEmptyBuffer();
        if (!buffer) continue;

        ZeroMemory(&ovLapArray[i], sizeof(OVERLAPPED));
        ovLapArray[i].hEvent = CreateEventA(NULL, false, false, NULL);
        if (!ovLapArray[i].hEvent) {
            m_bufferManager->ReturnEmptyBuffer(buffer);
            continue;
        }

        if (!m_bulkEndpoint->BeginDataXfer(
            buffer->data.get(),
            static_cast<long>(buffer->size),
            &ovLapArray[i]
        )) {
            CloseHandle(ovLapArray[i].hEvent);
            m_bufferManager->ReturnEmptyBuffer(buffer);
            continue;
        }

        activeBuffers[i] = buffer;
    }

    int currentBuffer = 0;
    while (m_running) {
        if (!activeBuffers[currentBuffer]) {
            currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
            continue;
        }

        if (WaitForSingleObject(ovLapArray[currentBuffer].hEvent, USB_TIMEOUT) == WAIT_OBJECT_0) {
            LONG transferred = static_cast<LONG>(activeBuffers[currentBuffer]->size);
            if (GetOverlappedResult(m_bulkEndpoint->hDevice, &ovLapArray[currentBuffer], 
                reinterpret_cast<PULONG>(&transferred), FALSE)) {
                
                activeBuffers[currentBuffer]->bytesUsed = static_cast<size_t>(transferred);
                m_bufferManager->QueueFullBuffer(activeBuffers[currentBuffer]);
                m_dataReady.notify_one();

                // Start new transfer immediately
                Buffer* newBuffer = m_bufferManager->GetEmptyBuffer();
                if (newBuffer) {
                    if (m_bulkEndpoint->BeginDataXfer(
                        newBuffer->data.get(),
                        static_cast<long>(newBuffer->size),
                        &ovLapArray[currentBuffer]
                    )) {
                        activeBuffers[currentBuffer] = newBuffer;
                    } else {
                        m_bufferManager->ReturnEmptyBuffer(newBuffer);
                        activeBuffers[currentBuffer] = nullptr;
                    }
                }
            }
        } else {
            m_bulkEndpoint->Abort();
            m_bufferManager->ReturnEmptyBuffer(activeBuffers[currentBuffer]);
            activeBuffers[currentBuffer] = nullptr;
        }

        currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
    }

    // Cleanup
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (ovLapArray[i].hEvent) {
            CloseHandle(ovLapArray[i].hEvent);
        }
        if (activeBuffers[i]) {
            m_bufferManager->ReturnEmptyBuffer(activeBuffers[i]);
        }
    }
}

void DataStreamer::DiskWriterThread() {
    const size_t FLUSH_THRESHOLD = 1024 * 1024;  // 1MB
    size_t bytesWrittenSinceFlush = 0;

    while (m_running) {
        Buffer* buffer = m_bufferManager->GetFullBuffer();
        if (!buffer) {
            continue;
        }

        m_outFile.write(reinterpret_cast<char*>(buffer->data.get()), buffer->bytesUsed);
        bytesWrittenSinceFlush += buffer->bytesUsed;

        if (bytesWrittenSinceFlush >= FLUSH_THRESHOLD) {
            m_outFile.flush();
            bytesWrittenSinceFlush = 0;
        }

        m_bufferManager->ReturnEmptyBuffer(buffer);
    }
}