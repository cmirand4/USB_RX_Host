#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinDef.h>
#include <basetsd.h>   // Add this for additional type definitions
#include <wtypes.h>    // Add this for additional type definitions
#include <minwindef.h> // Add this for additional type definitions
#include "../include/DataStreamer.h"
#include <iostream>

int main() {
    try {
        DataStreamer streamer;
        
        // Specify the desired file size (e.g., 100MB)
        const size_t TARGET_SIZE = 100 * 1024 * 1024;  // 100 MB

        if (!streamer.Initialize(TARGET_SIZE)) {
            std::cerr << "Failed to initialize streamer" << std::endl;
            return -1;
        }

        if (!streamer.StartStreaming()) {
            std::cerr << "Failed to start streaming" << std::endl;
            return -1;
        }

        std::cout << "Streaming data... Target size: " << TARGET_SIZE / (1024*1024) << " MB" << std::endl;
        std::cout << "Will automatically stop when target size is reached." << std::endl;

        // Wait for completion
        while (!streamer.IsComplete()) {
            Sleep(100);  // Small delay to prevent CPU spinning
        }

        std::cout << "Target size reached. Stopping..." << std::endl;
        streamer.StopStreaming();
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
}