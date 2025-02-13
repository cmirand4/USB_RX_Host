#include <iostream>
#include <fstream>
#include <vector>      // Include the vector header for std::vector
#include <windows.h>
#include "CyAPI.h"     // Include Cypress CyAPI header for USB communication

int main() {
    // Define the target size in kB (e.g., stop after transferring 1000 kB)
    const long KB_TO_TRANSFER = 10000;//100000;  // Transfer 1 kB for testing
    const long TOTAL_BYTES_TO_TRANSFER = KB_TO_TRANSFER * 1024;  // Convert kB to bytes
    const long BUFFER_SIZE = (512 * 512);  // Buffer size 

    // Create USB device object (FX3)
    CCyUSBDevice* USBDevice = new CCyUSBDevice(NULL);

    // Open the first available FX3 device
    if (!USBDevice->Open(0)) {
        std::cerr << "Failed to open USB device" << std::endl;
        return -1;
    }

    // Set the endpoint to use for Bulk transfer (Adjust endpoint number if necessary)
    CCyBulkEndPoint* bulkInEndpoint = USBDevice->BulkInEndPt;
    std::cout << "End point: " << bulkInEndpoint << " bytes." << std::endl;

    // Check if bulkInEndpoint is valid
    if (bulkInEndpoint == nullptr) {
        std::cerr << "No bulk IN endpoint found." << std::endl;
        return -1;
    }

    // Output the endpoint address and attributes
    std::cout << "Using bulk IN endpoint with the following properties:" << std::endl;
    std::cout << "  Endpoint Address: 0x" << std::hex << (int)bulkInEndpoint->Address << std::dec << std::endl;
    std::cout << "  Attributes: " << (int)bulkInEndpoint->Attributes << std::endl;
    std::cout << "  Max Packet Size: " << bulkInEndpoint->MaxPktSize << std::endl;
    std::cout << "  Direction: " << (bulkInEndpoint->bIn ? "IN" : "OUT") << std::endl;

    // Configuring the Endpoint Transfer Size
    bulkInEndpoint->SetXferSize(BUFFER_SIZE);  // Set transfer size to 1 KB

    // Open a file to save the received data
    //std::ofstream outFile("C:/Users/Christopher/Documents/Prelim Voltage/streamTest/my_data_output.bin", std::ios::binary); // Lab
    //std::ofstream outFile("C:/Users/cmirand4/Documents/MATLAB/VI_Data/streamTest/my_data_output.bin", std::ios::binary); // Laptop
    std::ofstream outFile("C:/Users/cmirand4/Documents/MATLAB/VI_Data/streamTest/counter3.bin", std::ios::binary); // Laptop
    if (!outFile.is_open()) {
        std::cerr << "Failed to open output file for writing" << std::endl;
        return -1;
    }

    // Prepare a dynamic buffer to store received data
    std::vector<unsigned char> dataBuffer;
    const long BUFFER_THRESHOLD = 16 * 1024;  // threshold
    dataBuffer.reserve(BUFFER_THRESHOLD);  // Reserve memory for threshold

    unsigned char tempBuffer[BUFFER_SIZE];  // Temporary buffer for each transfer
    long bytesToTransfer;
    long transferredBytes;
    long totalTransferred = 0;  // Variable to keep track of total bytes transferred

    // Data streaming loop
    while (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
        bytesToTransfer = BUFFER_SIZE;  // Reset bytesToTransfer before each call

        // Perform the bulk transfer
        bool success = bulkInEndpoint->XferData(tempBuffer, bytesToTransfer);

        if (success && bytesToTransfer > 0) {
            transferredBytes = bytesToTransfer;

            // Append to dataBuffer
            dataBuffer.insert(dataBuffer.end(), tempBuffer, tempBuffer + transferredBytes);

            // Update counters
            totalTransferred += transferredBytes;

            // Write to file if buffer reaches threshold
            if (dataBuffer.size() >= BUFFER_THRESHOLD) {
                outFile.write(reinterpret_cast<char*>(dataBuffer.data()), dataBuffer.size());
                dataBuffer.clear();
                dataBuffer.reserve(BUFFER_THRESHOLD);  // Re-reserve memory after clearing
            }

            // Output progress (optional)
            // std::cout << "Transferred " << transferredBytes << " bytes. Total: " << totalTransferred << " bytes" << std::endl;
        }
        else {
            std::cerr << "Data transfer failed" << std::endl;
            break;
        }
    }

    // Write any remaining data
    if (!dataBuffer.empty()) {
        outFile.write(reinterpret_cast<char*>(dataBuffer.data()), dataBuffer.size());
    }

    // Clean up
    // std::cout << "Transfer complete. Total bytes transferred: " << totalTransferred << " bytes." << std::endl;
    outFile.close();
    USBDevice->Close();
    delete USBDevice;

    return 0;
}
