#include <iostream>
#include <fstream>
#include <vector>      // Include the vector header for std::vector
#include <windows.h>
#include "CyAPI.h"     // Include Cypress CyAPI header for USB communication

int main() {
    const int PACKETS_PER_XFER = 512; // 512
    const int BYTES_PER_PACKET = 1024;
    const long BUFFER_SIZE = PACKETS_PER_XFER * BYTES_PER_PACKET; // 512 KB
    const int NUM_XFERS = 2; // 64
    const long KB_TO_TRANSFER = 10000;
    const long TOTAL_BYTES_TO_TRANSFER = KB_TO_TRANSFER * 1024;

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
    bulkInEndpoint->SetXferSize(BUFFER_SIZE);

    // Open a file to save the received data
    //std::ofstream outFile("C:/Users/Christopher/Documents/Prelim Voltage/streamTest/my_data_output.bin", std::ios::binary); // Lab
    std::ofstream outFile("C:/Users/cmirand4/Documents/MATLAB/VI_Data/streamTest/stream12.bin", std::ios::binary); // Laptop
    if (!outFile.is_open()) {
        std::cerr << "Failed to open output file for writing" << std::endl;
        return -1;
    }

    // Prepare buffers and OVERLAPPED structures for asynchronous transfers
    std::vector<unsigned char*> buffers(NUM_XFERS);
    std::vector<OVERLAPPED> ovList(NUM_XFERS);
    std::vector<UCHAR*> outContexts(NUM_XFERS, nullptr);

    for (int i = 0; i < NUM_XFERS; i++) {
        buffers[i] = new unsigned char[BUFFER_SIZE];
        memset(&ovList[i], 0, sizeof(OVERLAPPED));
        ovList[i].hEvent = CreateEvent(NULL, false, false, NULL);
        if (ovList[i].hEvent == NULL) {
            std::cerr << "Failed to create event for transfer " << i << std::endl;
            return -1;
        }
    }

    // Initially queue all transfers
    for (int i = 0; i < NUM_XFERS; i++) {
        LONG len = BUFFER_SIZE;
        outContexts[i] = bulkInEndpoint->BeginDataXfer(buffers[i], len, &ovList[i]);
        if (outContexts[i] == nullptr) {
            std::cerr << "BeginDataXfer failed on xfer " << i << std::endl;
            return -1;
        }
    }

    long totalTransferred = 0;
    int activeTransfers = NUM_XFERS;
    // Data streaming loop
    while (totalTransferred < TOTAL_BYTES_TO_TRANSFER && activeTransfers > 0) {
        // Wait for any transfer to complete
        // We'll just loop through the ovList and check WaitForXfer on each
        for (int i = 0; i < NUM_XFERS; i++) {
            // Check if this transfer is complete
            if (bulkInEndpoint->WaitForXfer(&ovList[i], 100000)) {
                // Transfer is ready to finish
                LONG len = BUFFER_SIZE;
                if (bulkInEndpoint->FinishDataXfer(buffers[i], len, &ovList[i], outContexts[i])) {
                    // Successful Transfer
                    if (len > 0) {
                        outFile.write(reinterpret_cast<char*>(buffers[i]), len);
                        totalTransferred += len;
                    }

                    // If we still need more data, re-queue the transfer
                    if (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
                        LONG reLen = BUFFER_SIZE;
                        outContexts[i] = bulkInEndpoint->BeginDataXfer(buffers[i], reLen, &ovList[i]);
                        if (outContexts[i] == nullptr) {
                            std::cerr << "Re-queue BeginDataXfer failed" << std::endl;
                            activeTransfers--;
                        }
                    }
                    else {
                        // No more data needed
                        activeTransfers--;
                    }
                }
                else {
                    std::cerr << "FinishDataXfer failed on xfer " << i << std::endl;
                    activeTransfers--;
                }
            }
            else
            {
                //std::cerr << "WaitForXfer timed out"<< std::endl;
                //return -1; // WaitForXfer timed out, you can handle this if needed or continue
            }
            if (totalTransferred >= TOTAL_BYTES_TO_TRANSFER)
                break;
        }
    }
    std::cout << "Transfer complete. Total bytes transferred: " << totalTransferred << " bytes." << std::endl;
    // Cleanup
    for (int i = 0; i < NUM_XFERS; i++) {
        delete[] buffers[i];
        CloseHandle(ovList[i].hEvent);
    }
    outFile.close();
    USBDevice->Close();
    delete USBDevice;

    return 0;
}
