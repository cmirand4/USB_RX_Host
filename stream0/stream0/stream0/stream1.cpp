#include <iostream>
#include <fstream>
#include <vector>      // Include the vector header for std::vector
#include <windows.h>
#include <algorithm>   // Add this for std::min
#include "CyAPI.h"     // Include Cypress CyAPI header for USB communication

int main() {
    std::cout << "Starting program..." << std::endl;
    
    const long KB_TO_TRANSFER = 1000000;
    const long TOTAL_BYTES_TO_TRANSFER = KB_TO_TRANSFER * 1024;
    const long BUFFER_SIZE = (512 * 512) & ~0x3;  // Align to 4-byte boundary
    const int NUM_BUFFERS = 4;  // Increased from 2 to 4 for better buffering
    
    // For 150 MB/s data rate
    const size_t DATA_RATE = 150 * 1024 * 1024;  // 150 MB/s
    const size_t RECOMMENDED_BUFFER = 16 * 1024;  // 64 KB for more frequent writes
    const size_t LARGE_BUFFER = 64 * 1024 * 1024;  // If needed, go up to 64 MB
    
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

    std::cout << "Configuring endpoint..." << std::endl;
    // Increase priority of this thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    
    // Configure for maximum performance
    bulkInEndpoint->TimeOut = 10000;
    bulkInEndpoint->SetXferSize(BUFFER_SIZE);

    // Consider increasing buffer size if discontinuities persist
    // const long BUFFER_SIZE = 1024 * 1024;  // 1 MB buffer
    
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
                // Calculate remaining bytes for this transfer
                bytesToTransfer = static_cast<long>(std::min<long long>(
                    static_cast<long long>(BUFFER_SIZE), 
                    static_cast<long long>(TOTAL_BYTES_TO_TRANSFER - totalTransferred)
                ));
                if (bytesToTransfer <= 0) break;  // Stop if we've reached the total

                if (!bulkInEndpoint->BeginDataXfer(buffers[i], bytesToTransfer, &ovLapArray[i])) {
                    throw std::runtime_error("Failed to begin data transfer");
                }
                totalTransferred += bytesToTransfer;
                std::cout << "Started transfer " << i << " with " << bytesToTransfer << " bytes" << std::endl;
            }

            std::cout << "Entering main transfer loop..." << std::endl;
            totalTransferred = 0;  // Reset for actual transfers
            // Main transfer loop
            while (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
                // Ensure transfer size is multiple of 4 bytes
                bytesToTransfer = static_cast<long>(std::min<long long>(
                    static_cast<long long>(BUFFER_SIZE), 
                    static_cast<long long>(TOTAL_BYTES_TO_TRANSFER - totalTransferred)
                )) & ~0x3;  // Align to 4-byte boundary
                
                // Wait for current buffer to complete with timeout
                if (!WaitForSingleObject(ovLapArray[currentBuffer].hEvent, 10000) == WAIT_OBJECT_0) {
                    std::cerr << "Wait failed with error: " << GetLastError() << std::endl;
                    throw std::runtime_error("Transfer wait timeout");
                }

                LONG transferred = BUFFER_SIZE;
                
                // Get transfer status
                if (!GetOverlappedResult(bulkInEndpoint->hDevice, 
                                       &ovLapArray[currentBuffer], 
                                       (PULONG)&transferred, 
                                       FALSE)) {
                    std::cerr << "GetOverlappedResult failed: " << GetLastError() << std::endl;
                    throw std::runtime_error("Failed to get transfer result");
                }

                if (transferred > 0) {
                    // Ensure write size is multiple of 4 bytes
                    long bytesToWrite = (static_cast<long>(std::min<long long>(
                        static_cast<long long>(transferred), 
                        static_cast<long long>(TOTAL_BYTES_TO_TRANSFER - totalTransferred)
                    ))) & ~0x3;  // Align to 4-byte boundary

                    outFile.write(reinterpret_cast<char*>(buffers[currentBuffer]), bytesToWrite);
                    if (totalTransferred % (8 * 1024 * 1024) == 0) {  // Flush every 8MB
                        outFile.flush();
                    }
                    totalTransferred += bytesToWrite;

                    // Don't start a new transfer if we've reached the total
                    if (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
                        bytesToTransfer = static_cast<long>(std::min<long long>(
                            static_cast<long long>(BUFFER_SIZE), 
                            static_cast<long long>(TOTAL_BYTES_TO_TRANSFER - totalTransferred)
                        ));
                        if (!bulkInEndpoint->BeginDataXfer(buffers[currentBuffer], bytesToTransfer, 
                            &ovLapArray[currentBuffer])) {
                            throw std::runtime_error("Failed to begin next transfer");
                        }
                    }
                }

                currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception: " << e.what() << std::endl;
            return -1;
        }

        // Cleanup
        outFile.close();
        
        for (int i = 0; i < NUM_BUFFERS; i++) {
            CloseHandle(ovLapArray[i].hEvent);
            _aligned_free(buffers[i]);
        }
        delete[] ovLapArray;
        delete[] buffers;
        
        USBDevice->Close();
        delete USBDevice;

        std::cout << "Transfer complete. Total bytes transferred: " << totalTransferred << " bytes." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }
}
