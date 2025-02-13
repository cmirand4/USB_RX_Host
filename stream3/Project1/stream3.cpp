#include <iostream>
#include <fstream>
#include <vector>      // Include the vector header for std::vector
#include <windows.h>
#include <algorithm>   // Add this for std::min
#include <string>      // Add this for std::to_string
#include "CyAPI.h"     // Include Cypress CyAPI header for USB communication

int main() {
    std::cout << "Starting program..." << std::endl;

    const long KB_TO_TRANSFER = 1000000;
    const long TOTAL_BYTES_TO_TRANSFER = KB_TO_TRANSFER * 1024;

    // Optimize for maximum transfer rate
    const int NUM_BUFFERS = 64;  // Back to maximum queue depth
    const int PPX = 512;  // Maximum packets per transfer
    const size_t DATA_RATE = 150 * 1024 * 1024;  // 150 MB/s target
    const size_t RECOMMENDED_BUFFER = 64 * 1024;  // Increased for fewer writes
    const size_t LARGE_BUFFER = 64 * 1024 * 1024;  // 64 MB buffer

    // Add tracking variables to match C#
    long Successes = 0;
    long Failures = 0;
    double XferBytes = 0;
    bool bRunning = true;

    // Minimal performance monitoring
    double transferRate = 0.0;
    LARGE_INTEGER startTime, endTime, frequency;
    QueryPerformanceFrequency(&frequency);

    std::cout << "Creating USB device..." << std::endl;
    // Create USB device object (FX3)
    CCyUSBDevice* USBDevice = new CCyUSBDevice(NULL);

    std::cout << "Opening USB device..." << std::endl;
    // Open the first available FX3 device
    if (!USBDevice->Open(0)) {
        std::cerr << "Failed to open USB device" << std::endl;
        return -1;
    }
    std::cout << "USB device opened successfully" << std::endl;

    std::cout << "Getting bulk endpoint..." << std::endl;
    // Set the endpoint to use for Bulk transfer
    CCyBulkEndPoint* bulkInEndpoint = USBDevice->BulkInEndPt;

    // Now we can define BUFFER_SIZE using the endpoint's MaxPktSize
    const long BUFFER_SIZE = bulkInEndpoint->MaxPktSize * PPX;  // Buffer size based on packet size

    // Add detailed error checking
    if (!USBDevice || !bulkInEndpoint) {
        std::cerr << "Error: USB Device or Endpoint is null" << std::endl;
        if (USBDevice) {
            USBDevice->Close();
            delete USBDevice;
        }
        return -1;
    }

    // Print endpoint details for debugging
    std::cout << "Endpoint Address: 0x" << std::hex << (int)bulkInEndpoint->Address << std::dec << std::endl;
    std::cout << "Max Packet Size: " << bulkInEndpoint->MaxPktSize << " bytes" << std::endl;

    std::cout << "Configuring endpoint..." << std::endl;
    // Increase priority of this thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Configure endpoint for maximum throughput
    bulkInEndpoint->TimeOut = 1000;
    bulkInEndpoint->SetXferSize(BUFFER_SIZE);

    // Print initial configuration once
    std::cout << "Starting transfers with " << NUM_BUFFERS << " buffers of " 
              << BUFFER_SIZE << " bytes each" << std::endl;

    QueryPerformanceCounter(&startTime);

    std::cout << "Creating buffers..." << std::endl;
    // Create multiple aligned buffers for overlapped transfers
    std::vector<unsigned char*> buffers(NUM_BUFFERS);
    std::vector<OVERLAPPED> ovLapArray(NUM_BUFFERS);

    // Configure file writing
    const size_t WRITES_PER_BUFFER = 16;  // Combine 8 transfers before writing
    std::vector<char> fileBuffer(BUFFER_SIZE * WRITES_PER_BUFFER);
    size_t bufferedBytes = 0;

    try {
        std::cout << "Allocating " << NUM_BUFFERS << " buffers of size " << BUFFER_SIZE << " bytes each" << std::endl;
        
        for (int i = 0; i < NUM_BUFFERS; i++) {
            // Initialize OVERLAPPED structure
            ZeroMemory(&ovLapArray[i], sizeof(OVERLAPPED));
            ovLapArray[i].hEvent = CreateEvent(NULL, false, false, NULL);
            if (ovLapArray[i].hEvent == NULL) {
                throw std::runtime_error("Failed to create event for buffer " + std::to_string(i));
            }

            // Allocate and initialize buffer
            buffers[i] = (unsigned char*)_aligned_malloc(BUFFER_SIZE, sizeof(uint32_t));
            if (buffers[i] == NULL) {
                throw std::runtime_error("Failed to allocate buffer " + std::to_string(i));
            }

            // Initialize buffer with default value
            std::cout << "Initializing buffer " << i << std::endl;
            std::fill_n(buffers[i], BUFFER_SIZE, 0xA5);
        }

        std::cout << "Opening output file..." << std::endl;
        // Open file with smaller buffer for more frequent writes
        std::ofstream outFile("C:/Users/cmirand4/Documents/MATLAB/VI_Data/streamTest/counter2.bin",
            std::ios::binary | std::ios::out);

        if (!outFile.is_open()) {
            throw std::runtime_error("Failed to open output file");
        }

        outFile.rdbuf()->pubsetbuf(nullptr, RECOMMENDED_BUFFER);

        std::cout << "Initializing transfers..." << std::endl;
        long totalTransferred = 0;
        int currentBuffer = 0;
        long bytesToTransfer = BUFFER_SIZE;

        // Start initial transfers with error checking
        try {
            std::cout << "Starting initial transfers..." << std::endl;
            for (int i = 0; i < NUM_BUFFERS; i++) {
                if (!bulkInEndpoint || !buffers[i]) {
                    throw std::runtime_error("Null endpoint or buffer at index " + std::to_string(i));
                }

                // Calculate remaining bytes for this transfer
                bytesToTransfer = static_cast<long>(std::min<long long>(
                    static_cast<long long>(BUFFER_SIZE),
                    static_cast<long long>(TOTAL_BYTES_TO_TRANSFER - totalTransferred)
                ));
                if (bytesToTransfer <= 0) break;  // Stop if we've reached the total

                std::cout << "Starting transfer " << i << " with " << bytesToTransfer << " bytes" << std::endl;
                if (!bulkInEndpoint->BeginDataXfer(buffers[i], bytesToTransfer, &ovLapArray[i])) {
                    throw std::runtime_error("Failed to begin data transfer for buffer " + std::to_string(i));
                }
                totalTransferred += bytesToTransfer;
            }

            std::cout << "Entering main transfer loop..." << std::endl;
            totalTransferred = 0;  // Reset for actual transfers
            // Main transfer loop
            while (bRunning && totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
                // Wait for current buffer with timeout (500ms like C#)
                if (WaitForSingleObject(ovLapArray[currentBuffer].hEvent, 500) != WAIT_OBJECT_0) {
                    bulkInEndpoint->Abort();
                    WaitForSingleObject(ovLapArray[currentBuffer].hEvent, 500);
                }

                LONG transferred = BUFFER_SIZE;
                if (!GetOverlappedResult(bulkInEndpoint->hDevice,
                    &ovLapArray[currentBuffer],
                    (PULONG)&transferred,
                    FALSE)) {
                    Failures++;
                } else {
                    XferBytes += transferred;
                    Successes++;
                    
                    // Copy to file buffer
                    memcpy(&fileBuffer[bufferedBytes], buffers[currentBuffer], transferred);
                    bufferedBytes += transferred;

                    // Write to file when buffer is full
                    if (bufferedBytes >= fileBuffer.size()) {
                        outFile.write(fileBuffer.data(), bufferedBytes);
                        bufferedBytes = 0;
                    }
                }

                // Queue next transfer
                if (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
                    bytesToTransfer = static_cast<long>(std::min<long long>(
                        static_cast<long long>(BUFFER_SIZE),
                        static_cast<long long>(TOTAL_BYTES_TO_TRANSFER - totalTransferred)
                    ));

                    if (!bulkInEndpoint->BeginDataXfer(buffers[currentBuffer], bytesToTransfer,
                        &ovLapArray[currentBuffer])) {
                        Failures++;
                    }
                }

                currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
                totalTransferred += transferred;

                // Update progress less frequently (every 64 transfers)
                /*
                if ((Successes + Failures) % 64 == 0) {
                    QueryPerformanceCounter(&endTime);
                    double elapsed = static_cast<double>(endTime.QuadPart - startTime.QuadPart) / frequency.QuadPart;
                    transferRate = (XferBytes / (1024*1024)) / elapsed;  // MB/s
                    std::cout << "\rRate: " << transferRate << " MB/s" << std::flush;
                }
                */
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Exception: " << e.what() << std::endl;
            return -1;
        }

        // Write any remaining data
        if (bufferedBytes > 0) {
            outFile.write(fileBuffer.data(), bufferedBytes);
        }

        // Cleanup section
        std::cout << "Beginning cleanup..." << std::endl;
        
        // First close the file
        outFile.close();
        std::cout << "File closed" << std::endl;

        // Stop the endpoint
        if (bulkInEndpoint) {
            bulkInEndpoint->Abort();
            std::cout << "Endpoint aborted" << std::endl;
        }

        // Clean up buffers and events
        for (int i = 0; i < NUM_BUFFERS; i++) {
            // Clean up event handles
            if (ovLapArray[i].hEvent) {
                if (!CloseHandle(ovLapArray[i].hEvent)) {
                    std::cout << "Warning: Failed to close event handle " << i << std::endl;
                }
                ovLapArray[i].hEvent = NULL;
            }
            
            // Clean up buffers
            if (buffers[i]) {
                try {
                    _aligned_free(buffers[i]);
                    buffers[i] = nullptr;
                }
                catch (...) {
                    std::cout << "Warning: Exception while freeing buffer " << i << std::endl;
                }
            }
        }
        std::cout << "Buffers cleaned up" << std::endl;

        // Clean up USB device
        if (USBDevice) {
            USBDevice->Close();
            delete USBDevice;
            USBDevice = nullptr;
        }
        std::cout << "USB device cleaned up" << std::endl;

        std::cout << "Transfer complete. Total bytes transferred: " << totalTransferred << " bytes." << std::endl;
        std::cout << "Successes: " << Successes << ", Failures: " << Failures << std::endl;
        
        // Clear vectors before exit
        buffers.clear();
        ovLapArray.clear();
        
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception during execution: " << e.what() << std::endl;
        return -1;
    }
    catch (...) {
        std::cerr << "Unknown exception during execution" << std::endl;
        return -1;
    }
}
