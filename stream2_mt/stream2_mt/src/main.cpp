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

        if (!streamer.Initialize()) {
            std::cerr << "Failed to initialize streamer" << std::endl;
            return -1;
        }

        if (!streamer.StartStreaming()) {
            std::cerr << "Failed to start streaming" << std::endl;
            return -1;
        }

        // Wait for user input to stop
        std::cout << "Press Enter to stop streaming..." << std::endl;
        std::cin.get();

        streamer.StopStreaming();
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
}