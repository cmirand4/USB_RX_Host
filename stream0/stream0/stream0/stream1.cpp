#include <iostream>
#include <fstream>
#include <vector>      // Include the vector header for std::vector
#include <windows.h>
#include "CyAPI.h"     // Include Cypress CyAPI header for USB communication

int main() {
    std::cout << "Starting program..." << std::endl;
    
    const long KB_TO_TRANSFER = 100000;
    const long TOTAL_BYTES_TO_TRANSFER = KB_TO_TRANSFER * 1024;
    const long BUFFER_SIZE = 512 * 1024;  // 512 KB buffer
    const int NUM_BUFFERS = 3;  // Triple buffering
    
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
    // Configure for maximum performance
    bulkInEndpoint->TimeOut = 10000;
    bulkInEndpoint->SetXferSize(BUFFER_SIZE);

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
            
            buffers[i] = (unsigned char*)_aligned_malloc(BUFFER_SIZE, 4096);
            if (buffers[i] == NULL) {
                throw std::runtime_error("Failed to allocate buffer");
            }
        }
        
        std::cout << "Opening output file..." << std::endl;
        // Open file with larger buffer
        std::ofstream outFile("C:/Users/cmirand4/Documents/MATLAB/VI_Data/streamTest/stream17.bin", 
            std::ios::binary | std::ios::out);
        
        if (!outFile.is_open()) {
            throw std::runtime_error("Failed to open output file");
        }
        
        outFile.rdbuf()->pubsetbuf(nullptr, 8 * 1024 * 1024);  // 8MB file buffer

        std::cout << "Initializing transfers..." << std::endl;
        long totalTransferred = 0;
        int currentBuffer = 0;
        long bytesToTransfer = BUFFER_SIZE;
        
        // Start initial transfers with error checking
        try {
            std::cout << "Starting initial transfers..." << std::endl;
            for (int i = 0; i < NUM_BUFFERS; i++) {
                bytesToTransfer = BUFFER_SIZE;  // Reset for each transfer
                if (!bulkInEndpoint->BeginDataXfer(buffers[i], bytesToTransfer, &ovLapArray[i])) {
                    throw std::runtime_error("Failed to begin data transfer");
                }
                std::cout << "Started transfer " << i << std::endl;
            }

            std::cout << "Entering main transfer loop..." << std::endl;
            // Main transfer loop
            while (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
                bytesToTransfer = BUFFER_SIZE;
                
                std::cout << "Waiting for buffer " << currentBuffer << "..." << std::endl;
                // Wait for current buffer to complete with timeout
                if (!WaitForSingleObject(ovLapArray[currentBuffer].hEvent, 10000) == WAIT_OBJECT_0) {
                    std::cerr << "Wait failed with error: " << GetLastError() << std::endl;
                    throw std::runtime_error("Transfer wait timeout");
                }

                std::cout << "Finishing transfer for buffer " << currentBuffer << "..." << std::endl;
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
                    // Verify data integrity
                    int32_t* data = reinterpret_cast<int32_t*>(buffers[currentBuffer]);
                    size_t numInts = transferred / sizeof(int32_t);
                    
                    // Check for discontinuities in the sequence
                    for (size_t i = 1; i < numInts; i++) {
                        int32_t diff = data[i] - data[i-1];
                        if (diff != 1) {
                            std::cerr << "Data discontinuity detected at offset " 
                                    << totalTransferred + (i * sizeof(int32_t)) 
                                    << ": " << data[i-1] << " -> " << data[i] 
                                    << " (diff: " << diff << ")" << std::endl;
                        }
                    }

                    std::cout << "Writing " << transferred << " bytes to file..." << std::endl;
                    outFile.write(reinterpret_cast<char*>(buffers[currentBuffer]), transferred);
                    totalTransferred += transferred;

                    // Start next transfer
                    bytesToTransfer = BUFFER_SIZE;
                    if (!bulkInEndpoint->BeginDataXfer(buffers[currentBuffer], bytesToTransfer, 
                        &ovLapArray[currentBuffer])) {
                        throw std::runtime_error("Failed to begin next transfer");
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
