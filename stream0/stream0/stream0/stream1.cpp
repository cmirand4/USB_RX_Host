#include <iostream>
#include <fstream>
#include <vector>      // Include the vector header for std::vector
#include <windows.h>
#include <algorithm>   // Add this for std::min
#include "CyAPI.h"     // Include Cypress CyAPI header for USB communication

int main() {
    std::cout << "Starting program..." << std::endl;
    
    const long KB_TO_TRANSFER = 100*48; // Exactly 10 buffers worth of data
    const long TOTAL_BYTES_TO_TRANSFER = KB_TO_TRANSFER * 1024;
    const long BUFFER_SIZE = 49152;  // Match firmware buffer size (48 * 1024)
    const int NUM_BUFFERS = 4;  // Increase from 2 to 4 to match firmware buffer count
    
    // For 150 MB/s data rate
    const size_t DATA_RATE = 297 * 1024 * 1024;  // 150 MB/s
    const size_t RECOMMENDED_BUFFER = 1 * 1024 * 1024;  // 1MB for better write performance
    
    // FX3 specific timing
    const DWORD FX3_BUFFER_TIMEOUT = 1000;  // Longer timeout for initial transfers
    const int MAX_MISSED_BUFFERS = 2;  // Maximum number of buffers we can miss before resetting
    
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

    // Add error recovery mechanism
    const int MAX_RETRY_COUNT = 3;
    int retryCount = 0;
    
    auto resetEndpoint = [&]() {
        bulkInEndpoint->Abort();
        bulkInEndpoint->Reset();
        bulkInEndpoint->SetXferSize(BUFFER_SIZE);
        bulkInEndpoint->TimeOut = FX3_BUFFER_TIMEOUT;
    };

    std::cout << "Configuring endpoint..." << std::endl;
    // Increase priority of this thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    
    std::cout << "Creating buffers..." << std::endl;
    // Create multiple aligned buffers for overlapped transfers
    OVERLAPPED* ovLapArray = new OVERLAPPED[NUM_BUFFERS];
    unsigned char** buffers = new unsigned char*[NUM_BUFFERS];

    try {
        for (int i = 0; i < NUM_BUFFERS; i++) {
            ZeroMemory(&ovLapArray[i], sizeof(OVERLAPPED));
            ovLapArray[i].hEvent = CreateEvent(NULL, false, false, NULL);
            if (ovLapArray[i].hEvent == NULL) {
                throw std::runtime_error("Failed to create event");
            }
            
            // Ensure 4-byte alignment for 32-bit transfers
            buffers[i] = (unsigned char*)_aligned_malloc(BUFFER_SIZE, sizeof(uint32_t));
            if (buffers[i] == NULL) {
                throw std::runtime_error("Failed to allocate buffer");
            }
        }
        
        std::cout << "Opening output file..." << std::endl;
        // Open file with larger buffer for better write performance
        std::ofstream outFile("C:/Users/cmirand4/Documents/MATLAB/VI_Data/streamTest/counter5.bin", 
            std::ios::binary | std::ios::out);
        
        if (!outFile.is_open()) {
            throw std::runtime_error("Failed to open output file");
        }
        
        // Use larger buffer for file writes
        char* fileBuffer = new char[RECOMMENDED_BUFFER];
        outFile.rdbuf()->pubsetbuf(fileBuffer, RECOMMENDED_BUFFER);

        // Configure for maximum performance
        bulkInEndpoint->TimeOut = FX3_BUFFER_TIMEOUT;
        bulkInEndpoint->SetXferSize(BUFFER_SIZE);

        // Reset FX3 first
        std::cout << "Resetting FX3..." << std::endl;
        bulkInEndpoint->Abort();
        bulkInEndpoint->Reset();
        
        // Wait for reset to complete
        Sleep(100);  // 100ms should be sufficient for FX3 reset

        // Now queue transfers - Queue ALL buffers before starting
        std::cout << "Queuing transfers..." << std::endl;
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (!bulkInEndpoint->BeginDataXfer(buffers[i], BUFFER_SIZE, &ovLapArray[i])) {
                throw std::runtime_error("Failed to queue initial transfer");
            }
        }

        std::cout << "Starting FPGA..." << std::endl;
        // FPGA will start sending data, transfers are already queued
        Sleep(1000);  // FPGA initialization time

        std::cout << "Starting data reception..." << std::endl;
        long totalTransferred = 0;
        int currentBuffer = 0;
        int missedBuffers = 0;
        long bytesToTransfer = BUFFER_SIZE;
        
        // Add buffer transition tracking
        uint32_t expected_sequence = 0;
        uint32_t total_gaps = 0;
        bool isFirstTransfer = true;
        
        // Performance tracking
        LARGE_INTEGER perfFreq, perfStart, perfNow;
        QueryPerformanceFrequency(&perfFreq);
        QueryPerformanceCounter(&perfStart);
        QueryPerformanceCounter(&perfNow); // Initialize perfNow to avoid uninitialized variable
        long long bytesThisInterval = 0;
        int buffersThisInterval = 0;
        
        // Adaptive timeout
        DWORD currentTimeout = FX3_BUFFER_TIMEOUT;
        int consecutiveSuccesses = 0;
        int consecutiveErrors = 0;

        // Main transfer loop
        while (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
            // Calculate next transfer size
            bytesToTransfer = static_cast<long>(std::min<long long>(
                static_cast<long long>(BUFFER_SIZE), 
                static_cast<long long>(TOTAL_BYTES_TO_TRANSFER - totalTransferred)
            )) & ~0x3;  // Align to 4-byte boundary
            
            // Wait for current buffer with adaptive timeout
            DWORD waitResult = WaitForSingleObject(ovLapArray[currentBuffer].hEvent, currentTimeout);
            
            LARGE_INTEGER waitEnd;
            QueryPerformanceCounter(&waitEnd);
            double waitTime = (waitEnd.QuadPart - perfNow.QuadPart) * 1000.0 / perfFreq.QuadPart;
            
            if (waitResult == WAIT_OBJECT_0) {
                // Transfer completed successfully
                consecutiveSuccesses++;
                consecutiveErrors = 0;
                
                // Reduce timeout after several successful transfers
                if (consecutiveSuccesses > 5 && currentTimeout > 100) {
                    currentTimeout = 100;  // Reduce to 100ms after success
                }
            } else if (waitResult == WAIT_TIMEOUT) {
                // Handle timeout
                consecutiveSuccesses = 0;
                consecutiveErrors++;
                
                // Increase timeout after failures
                if (consecutiveErrors > 2) {
                    currentTimeout = std::min<DWORD>(currentTimeout * 2, 2000);  // Double timeout up to 2 seconds
                }
                
                // Try to recover the transfer
                LONG transferred = 0;
                DWORD bytesTransferred = 0;
                if (!GetOverlappedResult(bulkInEndpoint->hDevice, 
                                      &ovLapArray[currentBuffer], 
                                      &bytesTransferred, 
                                      TRUE)) {  // Wait for completion
                    transferred = static_cast<LONG>(bytesTransferred);
                    
                    if (consecutiveErrors >= MAX_RETRY_COUNT) {
                        resetEndpoint();
                        consecutiveErrors = 0;
                        
                        // Re-queue this buffer
                        if (!bulkInEndpoint->BeginDataXfer(buffers[currentBuffer], BUFFER_SIZE, 
                            &ovLapArray[currentBuffer])) {
                            throw std::runtime_error("Failed to re-queue transfer after reset");
                        }
                        continue;  // Skip to next iteration without advancing buffer
                    }
                    
                    continue;  // Try again with the same buffer
                }
            }

            // Get transfer result
            LONG transferred = BUFFER_SIZE;  // Initialize with the buffer size
            PUCHAR buffer = buffers[currentBuffer];
            OVERLAPPED* ov = &ovLapArray[currentBuffer];
            
            // Validate pointers before proceeding
            if (!bulkInEndpoint || !buffer || !ov) {
                if (bulkInEndpoint) resetEndpoint();
                
                continue;
            }
            
            // Use a try-catch block to handle potential access violations
            try {
                // Try a different approach - use GetOverlappedResult directly instead of FinishDataXfer
                DWORD bytesXferred = 0;
                BOOL success = GetOverlappedResult(
                    bulkInEndpoint->hDevice,
                    ov,
                    &bytesXferred,
                    TRUE  // Wait for completion
                );
                
                transferred = static_cast<LONG>(bytesXferred);
                
                if (!success || transferred <= 0) {
                    consecutiveErrors++;
                    
                    if (consecutiveErrors >= MAX_RETRY_COUNT) {
                        resetEndpoint();
                        consecutiveErrors = 0;
                    }
                    
                    // Re-queue this buffer
                    if (!bulkInEndpoint->BeginDataXfer(buffers[currentBuffer], BUFFER_SIZE, 
                        &ovLapArray[currentBuffer])) {
                        throw std::runtime_error("Failed to re-queue transfer");
                    }
                    continue;  // Skip to next iteration without advancing buffer
                }
            }
            catch (const std::exception& e) {
                consecutiveErrors++;
                
                // Try to recover
                resetEndpoint();
                
                // Re-queue this buffer
                if (!bulkInEndpoint->BeginDataXfer(buffers[currentBuffer], BUFFER_SIZE, 
                    &ovLapArray[currentBuffer])) {
                    throw std::runtime_error("Failed to re-queue transfer after exception");
                }
                continue;  // Skip to next iteration without advancing buffer
            }

            if (transferred > 0) {
                // Process current buffer
                long bytesToWrite = (transferred & ~0x3);  // Align to 4-byte boundary
                
                // Validate counter values and handle buffer transitions
                uint32_t* counterData = reinterpret_cast<uint32_t*>(buffers[currentBuffer]);
                size_t numCounters = bytesToWrite / sizeof(uint32_t);
                
                if (numCounters > 0) {
                    uint32_t firstCounter = counterData[0];
                    uint32_t lastCounter = counterData[numCounters - 1];
                    
                    // Initialize sequence on first transfer
                    if (isFirstTransfer) {
                        expected_sequence = firstCounter;
                        isFirstTransfer = false;
                        
                        // Verify first buffer is internally consistent - keep logic but remove output
                        for (size_t i = 1; i < numCounters; i++) {
                            if (counterData[i] != counterData[i-1] + 1) {
                                // Buffer inconsistency detected but not reported
                                break;
                            }
                        }
                    } else {
                        // For subsequent transfers, check sequence continuity
                        if (firstCounter != expected_sequence) {
                            uint32_t gap = (firstCounter > expected_sequence) ? 
                                (firstCounter - expected_sequence) :
                                (0xFFFFFFFF - expected_sequence + firstCounter + 1);
                            
                            if (gap > 1) {  // Allow for uint32_t wraparound
                                // Calculate how many FX3 buffers might have been missed
                                int possibleMissedBuffers = gap / 12288;
                                missedBuffers += possibleMissedBuffers;
                            }
                            
                            // Verify buffer internal consistency - keep logic but remove output
                            for (size_t i = 1; i < numCounters; i++) {
                                if (counterData[i] != counterData[i-1] + 1) {
                                    // Buffer inconsistency detected but not reported
                                    break;
                                }
                            }
                        }
                        
                        expected_sequence = lastCounter + 1;
                    }
                }

                // Write data to file
                outFile.write(reinterpret_cast<char*>(buffers[currentBuffer]), bytesToWrite);
                if (totalTransferred % (32 * 1024 * 1024) == 0) {  // Flush every 32MB
                    outFile.flush();
                }
                totalTransferred += bytesToWrite;
                bytesThisInterval += bytesToWrite;
                buffersThisInterval++;
                
                // Performance reporting
                QueryPerformanceCounter(&perfNow);
                double elapsedSec = (perfNow.QuadPart - perfStart.QuadPart) / (double)perfFreq.QuadPart;
                if (elapsedSec >= 1.0) {  // Report every second
                    double mbps = (bytesThisInterval * 8.0) / (elapsedSec * 1000000.0);
                    
                    // Reset interval counters
                    bytesThisInterval = 0;
                    buffersThisInterval = 0;
                    QueryPerformanceCounter(&perfStart);
                }
                
                // Queue next transfer immediately after processing
                int nextBufferToQueue = currentBuffer;  // Reuse the buffer we just finished with
                if (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
                    if (!bulkInEndpoint->BeginDataXfer(buffers[nextBufferToQueue], BUFFER_SIZE, 
                        &ovLapArray[nextBufferToQueue])) {
                        throw std::runtime_error("Failed to begin next transfer");
                    }
                }
            }

            // Move to next buffer
            currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
        }

        // Cleanup
        outFile.close();
        delete[] fileBuffer;
        
        for (int i = 0; i < NUM_BUFFERS; i++) {
            CloseHandle(ovLapArray[i].hEvent);
            _aligned_free(buffers[i]);
        }
        delete[] ovLapArray;
        delete[] buffers;
        
        USBDevice->Close();
        delete USBDevice;

        // Calculate and report overall performance
        LARGE_INTEGER endTime;
        QueryPerformanceCounter(&endTime);
        double totalElapsedSec = (endTime.QuadPart - perfStart.QuadPart) / (double)perfFreq.QuadPart;
        double avgMbps = (totalTransferred * 8.0) / (totalElapsedSec * 1000000.0);
        
        std::cout << "Transfer complete. Total bytes transferred: " << totalTransferred << " bytes." << std::endl;
        std::cout << "Average transfer rate: " << avgMbps << " Mbps (" 
                  << (totalTransferred / (1024.0 * 1024.0)) << " MB in " 
                  << totalElapsedSec << " seconds)" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        
        // Cleanup on error
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (ovLapArray[i].hEvent != NULL) {
                CloseHandle(ovLapArray[i].hEvent);
            }
            if (buffers[i] != NULL) {
                _aligned_free(buffers[i]);
            }
        }
        delete[] ovLapArray;
        delete[] buffers;
        
        if (USBDevice) {
            USBDevice->Close();
            delete USBDevice;
        }
        
        return -1;
    }
}

