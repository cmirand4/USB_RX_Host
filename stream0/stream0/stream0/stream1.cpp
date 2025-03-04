#include <iostream>
#include <fstream>
#include <vector>      // Include the vector header for std::vector
#include <windows.h>
#include <algorithm>   // Add this for std::min
#include "CyAPI.h"     // Include Cypress CyAPI header for USB communication
#include <thread>      // For std::thread
#include <atomic>      // For std::atomic
#include <chrono>      // For std::chrono

// Global variables for watchdog
std::atomic<bool> g_programRunning(true);
std::atomic<long> g_totalBytesTransferred(0);
std::atomic<long> g_lastBytesTransferred(0);
std::atomic<bool> g_watchdogActive(false);
std::atomic<int> g_programStage(0);  // Track program stage: 0=init, 1=setup, 2=transfer
std::atomic<long long> g_lastProgressTime(0);  // Last time progress was made
std::atomic<int> g_loopHeartbeat(0);  // Heartbeat counter for the main loop
CCyBulkEndPoint* g_bulkInEndpoint = nullptr;  // Global pointer to the endpoint

// Global constants
const long BUFFER_SIZE = 61440;  // 60KB = 4 * 61440 bytes (multiple of 16KB)
const int NUM_BUFFERS = 3;       // 3 buffers × 61440 bytes =  bytes (~191.25KB total)
const DWORD FX3_BUFFER_TIMEOUT = 1000;  // Longer timeout for initial transfers

// Watchdog function to monitor progress
void watchdogThread() {
    std::cout << "Watchdog thread started..." << std::endl;
    
    const int WATCHDOG_CHECK_INTERVAL_MS = 1000;  // Check every second
    const int MAX_INACTIVITY_SECONDS = 10;        // 10 seconds of no progress before terminating
    const int MAX_INIT_SECONDS = 30;              // 30 seconds max for initialization
    
    int inactivityCounter = 0;
    long lastBytesTransferred = 0;
    int lastStage = 0;
    
    // Get current time as initial progress time
    auto now = std::chrono::steady_clock::now();
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    g_lastProgressTime.store(nowMs);
    
    int lastHeartbeat = 0;
    
    while (g_programRunning) {
        // Sleep for the check interval
        std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_CHECK_INTERVAL_MS));
        
        // Get current time
        now = std::chrono::steady_clock::now();
        nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        
        // Get current program stage
        int currentStage = g_programStage.load();
        
        // Check for progress based on stage
        if (currentStage > lastStage) {
            // Program advanced to next stage
            std::cout << "Program advanced to stage " << currentStage << std::endl;
            inactivityCounter = 0;
            g_lastProgressTime.store(nowMs);
        } else if (currentStage == 2) {
            // In transfer stage, check bytes transferred
            long currentBytes = g_totalBytesTransferred.load();
            
            if (currentBytes > lastBytesTransferred) {
                // Progress in data transfer
                std::cout << "Progress detected: " << (currentBytes - lastBytesTransferred) 
                          << " bytes transferred since last check." << std::endl;
                inactivityCounter = 0;
                g_lastProgressTime.store(nowMs);
            } else {
                // No progress in data transfer
                inactivityCounter++;
                std::cout << "No data transfer progress for " << inactivityCounter << " seconds..." << std::endl;
            }
            
            lastBytesTransferred = currentBytes;
            
            // Check heartbeat in transfer stage
            int currentHeartbeat = g_loopHeartbeat.load();
            if (currentHeartbeat == lastHeartbeat) {
                inactivityCounter++;
                std::cout << "No heartbeat progress for " << inactivityCounter << " seconds..." << std::endl;
            } else {
                // Heartbeat changed, reset inactivity counter
                inactivityCounter = 0;
                g_lastProgressTime.store(nowMs);
            }
            lastHeartbeat = currentHeartbeat;
        } else {
            // Check time since last progress
            long long lastProgressTime = g_lastProgressTime.load();
            long long elapsedMs = nowMs - lastProgressTime;
            
            if (elapsedMs > MAX_INIT_SECONDS * 1000) {
                std::cout << "WATCHDOG: Program stuck in stage " << currentStage 
                          << " for " << (elapsedMs / 1000) << " seconds. Terminating." << std::endl;
                exit(-2);
            }
        }
        
        // Check for inactivity timeout in transfer stage
        if (currentStage == 2 && inactivityCounter >= MAX_INACTIVITY_SECONDS) {
            std::cout << "WATCHDOG: No data transfer progress for " << MAX_INACTIVITY_SECONDS 
                      << " seconds. Terminating program." << std::endl;
            exit(-2);
        }
        
        lastStage = currentStage;
    }
    
    std::cout << "Watchdog thread exiting..." << std::endl;
}

// Function to reset the endpoint (moved outside main for clarity)
void resetEndpoint() {
    if (g_bulkInEndpoint) {
        g_bulkInEndpoint->Abort();
        g_bulkInEndpoint->Reset();
        g_bulkInEndpoint->SetXferSize(BUFFER_SIZE);
        g_bulkInEndpoint->TimeOut = FX3_BUFFER_TIMEOUT;
        Sleep(100);  // Give it time to reset
    }
}

int main() {
    std::cout << "Starting program..." << std::endl;
    
    // Start watchdog thread
    std::thread watchdog(watchdogThread);
    watchdog.detach();  // Detach so it can run independently
    
    // Updated buffer size and count to match firmware configuration
    // const long BUFFER_SIZE = 61440;  // Moved to global scope
    // const int NUM_BUFFERS = 3;       // Moved to global scope
    
    // Calculate total bytes to transfer based on buffer size (approximately 100MB)
    const long KB_TO_TRANSFER = 100000*1024/BUFFER_SIZE * BUFFER_SIZE/1024; // Adjust to be a multiple of buffer size
    const long TOTAL_BYTES_TO_TRANSFER = KB_TO_TRANSFER * 1024;
    
    // For 150 MB/s data rate
    const size_t DATA_RATE = 297 * 1024 * 1024;  // 150 MB/s
    const size_t RECOMMENDED_BUFFER = 1 * 1024 * 1024;  // 1MB for better write performance
    
    // FX3 specific timing
    // const DWORD FX3_BUFFER_TIMEOUT = 1000;  // Moved to global scope
    const DWORD INACTIVITY_TIMEOUT = 5000;  // 5 seconds of no data before considering transmission stopped
    
    std::cout << "Creating USB device..." << std::endl;
    // Create USB device object (FX3)
    CCyUSBDevice* USBDevice = new CCyUSBDevice(NULL);
    
    // Update progress
    auto updateProgress = []() {
        auto now = std::chrono::steady_clock::now();
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        g_lastProgressTime.store(nowMs);
    };
    updateProgress();
    
    std::cout << "Opening USB device..." << std::endl;
    // Open the first available FX3 device
    if (!USBDevice->Open(0)) {
        std::cerr << "Failed to open USB device" << std::endl;
        return -1;
    }
    std::cout << "USB device opened successfully" << std::endl;
    updateProgress();

    std::cout << "Getting bulk endpoint..." << std::endl;
    // Set the endpoint to use for Bulk transfer
    CCyBulkEndPoint* bulkInEndpoint = USBDevice->BulkInEndPt;
    g_bulkInEndpoint = bulkInEndpoint;  // Set the global pointer

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
    std::cout << "Buffer Size: " << BUFFER_SIZE << " bytes" << std::endl;
    std::cout << "Number of Buffers: " << NUM_BUFFERS << std::endl;
    std::cout << "Total Buffer Memory: " << (BUFFER_SIZE * NUM_BUFFERS) << " bytes" << std::endl;

    // Add error recovery mechanism
    const int MAX_RETRY_COUNT = 3;
    int retryCount = 0;
    
    // Removed lambda definition for resetEndpoint since we now have a global function
    
    std::cout << "Configuring endpoint..." << std::endl;
    // Increase priority of this thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    
    std::cout << "Creating buffers..." << std::endl;
    // Create multiple aligned buffers for overlapped transfers
    OVERLAPPED* ovLapArray = new OVERLAPPED[NUM_BUFFERS];
    unsigned char** buffers = new unsigned char*[NUM_BUFFERS];

    try {
        // Define filename separately for easy modification
        std::string fileName = "counter7.bin";

        std::string outputFilePath;
        char hostname[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = sizeof(hostname);

        if (GetComputerNameA(hostname, &size)) {
            std::string computerName(hostname);
            
            if (computerName == "DESKTOP-CMO8VI1") {
                // Lab computer
                outputFilePath = "C:/Users/Christopher/Documents/Prelim Voltage/streamTest/" + fileName;
            } 
            else if (computerName == "BIO-7GW8HW3") {
                // Laptop
                outputFilePath = "C:/Users/cmirand4/Documents/MATLAB/VI_Data/streamTest/" + fileName;
            }
            else {
                // Default path
                outputFilePath = "C:/Temp/" + fileName;
                std::cout << "Unknown computer: " << computerName << ". Using default path." << std::endl;
            }
        } 
        else {
            // Fallback if hostname can't be determined
            outputFilePath = "C:/Temp/" + fileName;
            std::cout << "Could not determine hostname. Using default path." << std::endl;
        }

        std::cout << "Opening output file: " << outputFilePath << std::endl;
        std::ofstream outFile(outputFilePath, std::ios::binary | std::ios::out);
        updateProgress();

        if (!outFile.is_open()) {
            throw std::runtime_error("Failed to open output file: " + outputFilePath);
        }

        // Now create the buffers
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
            updateProgress();
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
        updateProgress();
        
        // Wait for reset to complete
        Sleep(100);  // 100ms should be sufficient for FX3 reset

        // Now queue transfers - Queue ALL buffers before starting
        std::cout << "Queuing transfers..." << std::endl;
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (!bulkInEndpoint->BeginDataXfer(buffers[i], BUFFER_SIZE, &ovLapArray[i])) {
                throw std::runtime_error("Failed to queue initial transfer");
            }
            updateProgress();
        }

        std::cout << "Starting FPGA..." << std::endl;
        // FPGA will start sending data, transfers are already queued
        Sleep(1000);  // FPGA initialization time

        // Update progress to stage 1 (setup)
        g_programStage.store(1);
        updateProgress();
        
        std::cout << "Starting data reception..." << std::endl;
        long totalTransferred = 0;
        int currentBuffer = 0;
        long bytesToTransfer = BUFFER_SIZE;
        
        // Update progress to stage 2 (transfer)
        g_programStage.store(2);
        updateProgress();
        
        // Activate watchdog after initialization
        g_watchdogActive.store(true);
        
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

        // Inactivity detection
        LARGE_INTEGER lastDataTime;
        QueryPerformanceCounter(&lastDataTime);
        bool dataReceivedSinceLastCheck = false;

        // Main transfer loop
        while (totalTransferred < TOTAL_BYTES_TO_TRANSFER) {
            // Increment heartbeat counter to show the loop is still running
            g_loopHeartbeat++;
            
            // Calculate next transfer size
            bytesToTransfer = static_cast<long>(std::min<long long>(
                static_cast<long long>(BUFFER_SIZE), 
                static_cast<long long>(TOTAL_BYTES_TO_TRANSFER - totalTransferred)
            )) & ~0x3;  // Align to 4-byte boundary
            
            // Wait for current buffer with adaptive timeout
            g_loopHeartbeat++;  // Heartbeat before wait
            DWORD waitResult = WaitForSingleObject(ovLapArray[currentBuffer].hEvent, currentTimeout);
            g_loopHeartbeat++;  // Heartbeat after wait
            
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
                g_loopHeartbeat++;  // Heartbeat in timeout handling
            }

            // Get transfer result
            g_loopHeartbeat++;  // Heartbeat before getting result
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
                g_loopHeartbeat++;  // Heartbeat after getting result
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
                g_loopHeartbeat++;  // Heartbeat in exception handling
            }

            if (transferred > 0) {
                // Process current buffer
                long bytesToWrite = (transferred & ~0x3);  // Align to 4-byte boundary
                
                // Write data to file without any real-time analysis
                outFile.write(reinterpret_cast<char*>(buffers[currentBuffer]), bytesToWrite);
                if (totalTransferred % (32 * 1024 * 1024) == 0) {  // Flush every 32MB
                    outFile.flush();
                }
                totalTransferred += bytesToWrite;
                g_totalBytesTransferred.store(totalTransferred);  // Update global counter for watchdog
                bytesThisInterval += bytesToWrite;
                buffersThisInterval++;
                
                // Update last data time when we receive data
                if (bytesToWrite > 0) {
                    QueryPerformanceCounter(&lastDataTime);
                    dataReceivedSinceLastCheck = true;
                }
                
                // Performance reporting (minimal, once per second)
                QueryPerformanceCounter(&perfNow);
                double elapsedSec = (perfNow.QuadPart - perfStart.QuadPart) / (double)perfFreq.QuadPart;
                if (elapsedSec >= 1.0) {  // Report every second
                    double mbps = (bytesThisInterval * 8.0) / (elapsedSec * 1000000.0);
                    std::cout << "Transfer rate: " << mbps << " Mbps (" 
                              << (bytesThisInterval / (1024.0 * 1024.0)) << " MB/s)" << std::endl;
                    
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
                
                // Check for inactivity
                QueryPerformanceCounter(&perfNow);
                double inactivityTime = (perfNow.QuadPart - lastDataTime.QuadPart) / (double)perfFreq.QuadPart * 1000.0;
                
                // If no data received for INACTIVITY_TIMEOUT milliseconds, exit the loop
                if (dataReceivedSinceLastCheck && inactivityTime > INACTIVITY_TIMEOUT) {
                    std::cout << "No data received for " << (inactivityTime / 1000.0) << " seconds. Transmission appears to have stopped." << std::endl;
                    std::cout << "Exiting transfer loop..." << std::endl;
                    break;  // Exit the transfer loop
                }
                
                // Reset the flag for the next interval
                dataReceivedSinceLastCheck = false;
                g_loopHeartbeat++;  // Heartbeat after processing buffer
            }

            // Move to next buffer
            currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
            g_loopHeartbeat++;  // Heartbeat after queuing next transfer
        }

        // Clean up
        g_programRunning.store(false);  // Signal watchdog to exit
        
        // Close file and release resources
        outFile.close();
        delete[] fileBuffer;
        
        // Properly clean up resources with null checks
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
        std::cout << "Use MATLAB to analyze the counter values in the binary file." << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        
        // Cleanup on error with proper null checks
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (ovLapArray && ovLapArray[i].hEvent != NULL) {
                CloseHandle(ovLapArray[i].hEvent);
            }
            if (buffers && buffers[i] != NULL) {
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

