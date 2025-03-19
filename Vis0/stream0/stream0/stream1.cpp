#include <iostream>
#include <fstream>
#include <vector>
#include <windows.h>
#include <algorithm>
#include "CyAPI.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <bitset>
#include <string>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

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
const long BUFFER_SIZE = 65280;  // 60KB = 4 * 61440 bytes (multiple of 16KB)
const int NUM_BUFFERS = 3;       // 3 buffers * 61440 bytes =  bytes (~191.25KB total)
const DWORD FX3_BUFFER_TIMEOUT = 1000;  // Longer timeout for initial transfers
const size_t ANALYSIS_BUFFER_SIZE = 2 * 1024 * 1024; // 2 MB for analysis

// Global buffer to store received data for analysis
std::vector<unsigned char> g_analysisBuffer;

// Add these declarations near the top, before any function definitions
std::vector<bool> createSAVPattern();
std::vector<bool> createEAVPattern();
std::vector<bool> createSAVIPattern();
std::vector<bool> createEAVIPattern();
struct VideoLine;  // Forward declaration
struct VideoFrame; // Forward declaration

// Helper functions for data analysis
std::vector<bool> createSAVPattern();  // Add these declarations
std::vector<bool> createEAVPattern();  // near the other function declarations

// Add this forward declaration near the top of the file, with the other forward declarations
void applyAdaptiveHistogramEqualization(BYTE* imageData, int width, int height, int tileSize);

// Add this global variable near the top with the other globals
bool g_applyHistogramEqualization = false; // Toggle for histogram equalization

// Implement a grayscale-specific adaptive histogram equalization
void applyHistogramEqualization(BYTE* grayImageData, int width, int height) {
    if (!grayImageData || width <= 0 || height <= 0) return;
    
    // Create temporary buffer for the processed image
    BYTE* equalizedImage = new BYTE[width * height];
    memset(equalizedImage, 0, width * height);
    
    // Define tile size for adaptive processing
    const int tileSize = 32; // Can be adjusted based on image details
    const int numTilesX = (width + tileSize - 1) / tileSize;
    const int numTilesY = (height + tileSize - 1) / tileSize;
    
    // Process each tile separately
    for (int tileY = 0; tileY < numTilesY; tileY++) {
        for (int tileX = 0; tileX < numTilesX; tileX++) {
            // Tile boundaries
            int startX = tileX * tileSize;
            int startY = tileY * tileSize;
            int endX = (startX + tileSize < width) ? (startX + tileSize) : width;
            int endY = (startY + tileSize < height) ? (startY + tileSize) : height;
            
            // Calculate histogram for this tile
            int histogram[256] = {0};
            for (int y = startY; y < endY; y++) {
                for (int x = startX; x < endX; x++) {
                    histogram[grayImageData[y * width + x]]++;
                }
            }
            
            // Calculate cumulative distribution function (CDF)
            int cdf[256] = {0};
            cdf[0] = histogram[0];
            for (int i = 1; i < 256; i++) {
                cdf[i] = cdf[i - 1] + histogram[i];
            }
            
            // Skip empty tiles
            if (cdf[255] == 0) continue;
            
            // Normalize CDF to create lookup table
            float scale = 255.0f / cdf[255];
            BYTE lut[256] = {0};
            for (int i = 0; i < 256; i++) {
                int value = static_cast<int>(cdf[i] * scale);
                lut[i] = (value > 255) ? 255 : static_cast<BYTE>(value);
            }
            
            // Apply equalization to this tile
            for (int y = startY; y < endY; y++) {
                for (int x = startX; x < endX; x++) {
                    equalizedImage[y * width + x] = lut[grayImageData[y * width + x]];
                }
            }
        }
    }
    
    // Copy the equalized image back to the original buffer
    memcpy(grayImageData, equalizedImage, width * height);
    
    // Clean up
    delete[] equalizedImage;
}

// Convert a hex string to a vector of bits
std::vector<bool> hexToBinaryVector(const std::string& hexStr, int numBits) {
    std::vector<bool> result(numBits, false);
    unsigned long value = std::stoul(hexStr, nullptr, 16);
    
    for (int i = 0; i < numBits; ++i) {
        result[numBits - 1 - i] = (value >> i) & 1;
    }
    
    return result;
}

// Get code pattern for specific sync codes
std::vector<bool> getCode(const std::string& code) {
    std::vector<bool> result(8, false);
    
    if (code == "sav") {
        // SAV: Start of Active Video (0x80)
        result = {1, 0, 0, 0, 0, 0, 0, 0};  // 0x80
    } else if (code == "savi") {
        // SAVI: Start of Active Video Invalid (0xAB)
        result = {1, 0, 1, 0, 1, 0, 1, 1};  // 0xAB
    } else if (code == "eav") {
        // EAV: End of Active Video (0x9D)
        result = {1, 0, 0, 1, 1, 1, 0, 1};  // 0x9D
    } else if (code == "eavi") {
        // EAVI: End of Active Video Invalid (0xB6)
        result = {1, 0, 1, 1, 0, 1, 1, 0};  // 0xB6
    }
    
    return result;
}

// Convert uint32 data to bits with specified endianness (optimized version)
std::vector<uint8_t> lookAtBits(const std::vector<uint32_t>& data, bool useGPU, 
                               const std::string& endian, bool vector, const std::string& bitDepth) {
    // Determine number of bits based on bitDepth
    int numBits = 32;  // For uint32
    if (bitDepth == "uint16") numBits = 16;
    else if (bitDepth == "uint8") numBits = 8;
    
    // Create bit order array based on endianness
    std::vector<int> bitOrder(numBits);
    if (endian == "big") {
        for (int i = 0; i < numBits; i++) bitOrder[i] = numBits - i;
    } else {
        for (int i = 0; i < numBits; i++) bitOrder[i] = i + 1;
    }
    
    // Process each element for each bit position (like MATLAB's bitget)
    std::vector<uint8_t> result;
    result.reserve(data.size() * numBits);
    
    for (size_t i = 0; i < data.size(); i++) {
            uint32_t value = data[i];
        for (int bit = 0; bit < numBits; bit++) {
            result.push_back((value >> (bitOrder[bit] - 1)) & 1);
        }
    }
    
    return result;
}

// Find pattern in bit stream (similar to MATLAB's strfind)
// Optimized version using Knuth-Morris-Pratt algorithm for better performance
std::vector<size_t> findPattern(const std::vector<bool>& data, const std::vector<bool>& pattern) {
    std::vector<size_t> indices;
    
    if (pattern.empty() || pattern.size() > data.size()) {
        return indices;
    }
    
    // For very small patterns, use a simple search
    if (pattern.size() <= 4) {
        for (size_t i = 0; i <= data.size() - pattern.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < pattern.size(); ++j) {
                if (data[i + j] != pattern[j]) {
                    match = false;
                    break;
                }
            }
            
            if (match) {
                indices.push_back(i);
            }
        }
        return indices;
    }
    
    // For larger patterns, use KMP algorithm
    // Compute the failure function
    std::vector<size_t> failure(pattern.size(), 0);
    size_t j = 0;
    
    for (size_t i = 1; i < pattern.size(); ++i) {
        if (pattern[i] == pattern[j]) {
            failure[i] = j + 1;
            j++;
        } else if (j > 0) {
            j = failure[j - 1];
            i--; // Retry with the new j
        }
    }
    
    // Search for the pattern
    j = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == pattern[j]) {
            j++;
            if (j == pattern.size()) {
                // Pattern found
                indices.push_back(i - pattern.size() + 1);
                j = failure[j - 1];
            }
        } else if (j > 0) {
            j = failure[j - 1];
            i--; // Retry with the new j
        }
    }
    
    return indices;
}

// Find intersection of two vectors (similar to MATLAB's intersect)
std::vector<size_t> intersect(const std::vector<size_t>& a, const std::vector<size_t>& b) {
    std::vector<size_t> result;
    std::set<size_t> setA(a.begin(), a.end());
    
    for (const auto& val : b) {
        if (setA.find(val) != setA.end()) {
            result.push_back(val);
        }
    }
    
    std::sort(result.begin(), result.end());
    return result;
}

// Match start and end indices (similar to MATLAB's matchIdxs function)
void matchIdxs(std::vector<size_t>& start, std::vector<size_t>& stop) {
    // This function pairs each start (sav) index with an end (eav) index.
    // If no valid eav is found (i.e. none within the expected window),
    // it assigns a pseudo eav at (sav + 1488) bits.
    const size_t lowerBound = 2500/2;
    const size_t upperBound = 3500/2;
    const size_t defaultBits = 1488;  // if no eav is found, use sav + 1488 bits

    std::vector<size_t> newStart;
    std::vector<size_t> newStop;
    
    for (size_t i = 0; i < start.size(); ++i) {
        size_t s = start[i];
        bool foundMatch = false;
        
        for (size_t j = 0; j < stop.size(); ++j) {
            size_t diff = stop[j] > s ? stop[j] - s : 0;
            if (diff >= lowerBound && diff <= upperBound) {
                // Found a valid match
                newStart.push_back(s);
                newStop.push_back(stop[j]);
                foundMatch = true;
                break;
            }
        }
        
        if (!foundMatch) {
            // No matching eav found: assign a pseudo eav at sav + 1488 bits
            newStart.push_back(s);
            newStop.push_back(s + defaultBits);
        }
    }
    
    // Update the original vectors
    start = newStart;
    stop = newStop;
}

// New function to find initial sync pattern across all channels
std::vector<size_t> findInitialSync(const std::vector<std::vector<bool>>& channels, 
                                   const std::vector<bool>& savPattern) {
    const size_t numChannels = channels.size();
    if (numChannels != 4) return {};

    std::cout << "\nSearching for initial sync pattern..." << std::endl;
    std::cout << "Pattern length: " << savPattern.size() << " bits" << std::endl;
    std::cout << "Looking in " << channels[0].size() << " bits per channel" << std::endl;

    // Search first channel for SAV
    std::vector<size_t> initialIndices;
    size_t searchLimit = std::min<size_t>(channels[0].size() - savPattern.size(), 1000000);  // Limit initial search
    
    for (size_t i = 0; i <= searchLimit; i++) {
        if (i % 1000000 == 0) {  // Progress update every million positions
            std::cout << "Searching at position " << i << "..." << std::endl;
        }
        
        bool match = true;
        for (size_t j = 0; j < savPattern.size(); j++) {
            if (channels[0][i + j] != savPattern[j]) {
                match = false;
                break;
            }
        }
        
        if (match) {
            std::cout << "Found potential sync at position " << i << " in channel 1" << std::endl;
            
            // Print the matching bits
            std::cout << "Matched pattern: ";
            for (size_t j = 0; j < savPattern.size(); j++) {
                std::cout << (channels[0][i + j] ? "1" : "0");
                if ((j + 1) % 8 == 0) std::cout << " ";
            }
            std::cout << std::endl;
            
            // Verify this SAV exists at same position in all channels
            bool allChannelsMatch = true;
            for (size_t ch = 1; ch < numChannels; ch++) {
                bool channelMatch = true;
                for (size_t j = 0; j < savPattern.size(); j++) {
                    if (channels[ch][i + j] != savPattern[j]) {
                        channelMatch = false;
                        break;
                    }
                }
                if (!channelMatch) {
                    std::cout << "Pattern mismatch in channel " << (ch + 1) << std::endl;
                    allChannelsMatch = false;
                    break;
                }
                std::cout << "Pattern matched in channel " << (ch + 1) << std::endl;
            }
            
            if (allChannelsMatch) {
                std::cout << "Found valid sync across all channels at position " << i << std::endl;
                initialIndices.push_back(i);
                break;
            }
        }
    }
    
    return initialIndices;
}

// New function to validate and extract sync positions using known spacing
std::vector<std::pair<size_t, size_t>> extractSyncPositions(
    const std::vector<std::vector<bool>>& channels,
    const std::vector<bool>& savPattern,
    const std::vector<bool>& eavPattern,
    size_t initialPos) {
    
    const size_t BITS_PER_BYTE = 8;
    const size_t SYNC_SIZE = savPattern.size();
    const size_t MIN_DATA_BYTES = 180;
    const size_t MAX_DATA_BYTES = 186;  // Increased to 186 based on observed data
    
    std::vector<std::pair<size_t, size_t>> syncPairs;
    size_t currentPos = initialPos;
    size_t maxSearch = std::min<size_t>(channels[0].size(), 100000ULL);  // Limit search to first 100K positions
    
    while (currentPos < maxSearch) {
        // Verify SAV at current position
        bool validSAV = true;
        for (const auto& channel : channels) {
            for (size_t i = 0; i < savPattern.size(); i++) {
                if (currentPos + i >= channel.size() || channel[currentPos + i] != savPattern[i]) {
                    validSAV = false;
                    break;
                }
            }
            if (!validSAV) break;
        }
        
        if (!validSAV) {
            currentPos++;
            continue;
        }
        
        // Look for EAV
        bool foundEAV = false;
        size_t eavPos = 0;
        
        for (size_t dataBytes = MIN_DATA_BYTES; dataBytes <= MAX_DATA_BYTES; dataBytes++) {
            size_t testPos = currentPos + (dataBytes * BITS_PER_BYTE);
            if (testPos + eavPattern.size() > channels[0].size()) break;
            
            bool validAtPos = true;
            for (const auto& channel : channels) {
                for (size_t i = 0; i < eavPattern.size(); i++) {
                    if (channel[testPos + i] != eavPattern[i]) {
                        validAtPos = false;
                        break;
                    }
                }
                if (!validAtPos) break;
            }
            
            if (validAtPos) {
                foundEAV = true;
                eavPos = testPos;
                break;
            }
        }
        
        if (foundEAV) {
            syncPairs.push_back({currentPos, eavPos});
            currentPos = eavPos + eavPattern.size();  // Skip to after this EAV
        } else {
            currentPos++;  // No EAV found, try next position
        }
        
        // Print progress every 10000 positions
        if (syncPairs.size() % 10 == 0) {
            std::cout << "Found " << syncPairs.size() << " valid lines so far..." << std::endl;
        }
    }
    
    return syncPairs;
}

// Add these new structures to help organize frame data
struct VideoLine {
    std::vector<uint8_t> channel1;
    std::vector<uint8_t> channel2;
    std::vector<uint8_t> channel3;
    std::vector<uint8_t> channel4;
    std::vector<uint8_t> interleavedData;
    size_t startIndex;
    size_t endIndex;
};

struct VideoFrame {
    std::vector<VideoLine> lines;
    int frameNumber;
};

// Add these global variables for the display window
HWND g_displayWindow = NULL;
HDC g_memoryDC = NULL;
HBITMAP g_memoryBitmap = NULL;
BYTE* g_displayBuffer = NULL;
int g_currentWidth = 0;
int g_currentHeight = 0;
bool g_displayInitialized = false;

// Create a window to display frames
bool InitializeDisplayWindow() {
    // Register window class
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
                case WM_CLOSE:
                    DestroyWindow(hwnd);
                    return 0;
                case WM_DESTROY:
                    PostQuitMessage(0);
                    return 0;
                case WM_PAINT: {
                    PAINTSTRUCT ps;
                    HDC hdc = BeginPaint(hwnd, &ps);
                    
                    if (g_memoryDC && g_memoryBitmap && g_displayBuffer) {
                        BitBlt(hdc, 0, 0, g_currentWidth, g_currentHeight, g_memoryDC, 0, 0, SRCCOPY);
                    }
                    
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                default:
                    return DefWindowProcA(hwnd, msg, wParam, lParam);
            }
        };
        wc.lpszClassName = "FrameViewerClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassA(&wc);
        registered = true;
    }
    
    // Create window
    g_displayWindow = CreateWindowA(
        "FrameViewerClass", "Video Frame Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );
    
    if (!g_displayWindow) {
        std::cout << "Failed to create display window" << std::endl;
        return false;
    }
    
    ShowWindow(g_displayWindow, SW_SHOW);
    UpdateWindow(g_displayWindow);
    
    g_displayInitialized = true;
    return true;
}

// Clean up display resources
void CleanupDisplay() {
    if (g_memoryBitmap) {
        DeleteObject(g_memoryBitmap);
        g_memoryBitmap = NULL;
        // g_displayBuffer is managed by g_memoryBitmap, so don't delete it separately
        g_displayBuffer = NULL;
    }
    
    if (g_memoryDC) {
        DeleteDC(g_memoryDC);
        g_memoryDC = NULL;
    }
    
    if (g_displayWindow) {
        DestroyWindow(g_displayWindow);
        g_displayWindow = NULL;
    }
    
    g_displayInitialized = false;
}

// Modify the displayFrameImage function to use the correct pixel count
void displayFrameImage(const VideoFrame& frame, int frameNumber) {
    if (frame.lines.empty()) {
        std::cout << "Cannot display empty frame" << std::endl;
        return;
    }
    
    // Use the proper calculation for pixel count - 178 bytes per channel × 4 channels = 712 pixels
    const int bytesPerChannel = 178; // From SAV/EAV analysis (1488 bits / 8 = 186 bytes, minus sync = 178)
    const int width = bytesPerChannel * 4; // All 4 channels interleaved = 712 pixels
    
    // Calculate image dimensions with safety limits
    int height = static_cast<int>(frame.lines.size());
    // Limit excessive image height to prevent memory issues
    const int MAX_HEIGHT = 2000;
    if (height > MAX_HEIGHT) {
        std::cout << "Warning: Limiting frame height from " << height << " to " << MAX_HEIGHT << " lines" << std::endl;
        height = MAX_HEIGHT;
    }
    
    // Safety check for memory allocation
    size_t requiredMemory = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (requiredMemory > 100 * 1024 * 1024) { // 100MB limit
        std::cout << "Warning: Image too large to display safely (" << 
                  (requiredMemory / (1024 * 1024)) << " MB). Skipping display." << std::endl;
        return;
    }
    
    if (width == 0) {
        std::cout << "Warning: Line has no data, cannot create image" << std::endl;
        return;
    }
    
    std::cout << "Displaying Frame " << frameNumber 
              << " (" << width << "x" << height << ") with all 4 channels interleaved" << std::endl;
    
    // Initialize display window if needed
    if (!g_displayInitialized) {
        if (!InitializeDisplayWindow()) {
            return;
        }
    }
    
    // Check if we need to recreate the bitmap (if dimensions changed)
    if (g_currentWidth != width || g_currentHeight != height) {
        // Clean up existing resources
        if (g_memoryBitmap) {
            DeleteObject(g_memoryBitmap);
            g_memoryBitmap = NULL;
            g_displayBuffer = NULL;
        }
        
        if (g_memoryDC) {
            DeleteDC(g_memoryDC);
            g_memoryDC = NULL;
        }
        
        // Create new resources
        HDC windowDC = GetDC(g_displayWindow);
        g_memoryDC = CreateCompatibleDC(windowDC);
        
        // Allocate memory for BITMAPINFO with space for 256 palette entries
        BITMAPINFO* pBmi = (BITMAPINFO*)malloc(sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
        if (!pBmi) {
            std::cout << "Failed to allocate memory for BITMAPINFO" << std::endl;
            return;
        }

        // Zero out the entire structure
        memset(pBmi, 0, sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));

        // Initialize the header
        pBmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        pBmi->bmiHeader.biWidth = static_cast<LONG>(width);
        pBmi->bmiHeader.biHeight = -static_cast<LONG>(height); // Negative for top-down
        pBmi->bmiHeader.biPlanes = 1;
        pBmi->bmiHeader.biBitCount = 8; // 8-bit grayscale
        pBmi->bmiHeader.biCompression = BI_RGB;

        // Set up a grayscale palette
        for (int i = 0; i < 256; i++) {
            pBmi->bmiColors[i].rgbBlue = pBmi->bmiColors[i].rgbGreen = pBmi->bmiColors[i].rgbRed = static_cast<BYTE>(i);
            pBmi->bmiColors[i].rgbReserved = 0;
        }
        
        // Use pBmi instead of &bmi in CreateDIBSection
        g_memoryBitmap = CreateDIBSection(windowDC, pBmi, DIB_RGB_COLORS, 
                                         (void**)&g_displayBuffer, NULL, 0);
        
        // Free the memory when done
        free(pBmi);
        
        SelectObject(g_memoryDC, g_memoryBitmap);
        
        g_currentWidth = static_cast<int>(width);
        g_currentHeight = static_cast<int>(height);
        
        // Resize window to fit the image
        RECT windowRect, clientRect;
        GetWindowRect(g_displayWindow, &windowRect);
        GetClientRect(g_displayWindow, &clientRect);
        
        int borderWidth = (windowRect.right - windowRect.left) - clientRect.right;
        int borderHeight = (windowRect.bottom - windowRect.top) - clientRect.bottom;
        
        SetWindowPos(g_displayWindow, NULL, 0, 0, 
                    static_cast<int>(width) + borderWidth, 
                    static_cast<int>(height) + borderHeight, 
                    SWP_NOMOVE | SWP_NOZORDER);
    }
    
    // Update window title with frame info
    std::string title = "Frame " + std::to_string(frameNumber) + 
                        " (" + std::to_string(width) + "x" + std::to_string(height) + ")";
    SetWindowTextA(g_displayWindow, title.c_str());
    
    // Fill bitmap data from frame - use all 4 channels interleaved
    if (g_displayBuffer) {
        ZeroMemory(g_displayBuffer, width * height);
        
        for (int y = 0; y < height; y++) {
            if (y >= static_cast<int>(frame.lines.size())) break;
            
            const auto& line = frame.lines[static_cast<size_t>(y)];
            
            // Check if we have all channel data
            if (line.channel1.empty() || line.channel2.empty() || 
                line.channel3.empty() || line.channel4.empty()) {
                continue;  // Skip if any channel is missing
            }
            
            // Calculate max pixel value for this line
            size_t ch1Size = line.channel1.size();
            size_t ch2Size = line.channel2.size();
            size_t ch3Size = line.channel3.size();
            size_t ch4Size = line.channel4.size();
            size_t pixelsPerChannel = ch1Size;
            if (ch2Size < pixelsPerChannel) pixelsPerChannel = ch2Size;
            if (ch3Size < pixelsPerChannel) pixelsPerChannel = ch3Size;
            if (ch4Size < pixelsPerChannel) pixelsPerChannel = ch4Size;
            
            // Interleave channel data according to the specified pattern
            // ch1: pixels 0, 4, 8, ...
            // ch2: pixels 1, 5, 9, ...
            // ch3: pixels 2, 6, 10, ...
            // ch4: pixels 3, 7, 11, ...
            for (size_t i = 0; i < pixelsPerChannel; i++) {
                // Each value from channel1 goes to position 0, 4, 8, ...
                g_displayBuffer[y * width + (i * 4)]     = line.channel1[i];
                // Each value from channel2 goes to position 1, 5, 9, ...
                g_displayBuffer[y * width + (i * 4 + 1)] = line.channel2[i];
                // Each value from channel3 goes to position 2, 6, 10, ...
                g_displayBuffer[y * width + (i * 4 + 2)] = line.channel3[i];
                // Each value from channel4 goes to position 3, 7, 11, ...
                g_displayBuffer[y * width + (i * 4 + 3)] = line.channel4[i];
            }
        }
    }
    
    // Apply histogram equalization only if enabled
    if (g_applyHistogramEqualization) {
        std::cout << "Applying histogram equalization to enhance image contrast..." << std::endl;
    applyHistogramEqualization(g_displayBuffer, width, height);
    } else {
        std::cout << "Displaying raw image data without enhancement..." << std::endl;
    }
    
    // Force window to repaint
    InvalidateRect(g_displayWindow, NULL, FALSE);
    UpdateWindow(g_displayWindow);
    
    // Process any pending messages
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        // Add keyboard handler for toggling histogram equalization
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == 'H' || msg.wParam == 'h') {
                g_applyHistogramEqualization = !g_applyHistogramEqualization;
                std::cout << "Histogram equalization " 
                          << (g_applyHistogramEqualization ? "enabled" : "disabled") << std::endl;
                
                // Re-process the current image with the new setting
                if (g_applyHistogramEqualization) {
                    applyHistogramEqualization(g_displayBuffer, width, height);
                } else {
                    // Redisplay the image (we'll need to reload the original data)
                    // For simplicity, just signal to redraw for now
                }
                InvalidateRect(g_displayWindow, NULL, FALSE);
            }
        }
        
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        
        if (msg.message == WM_QUIT) {
            g_displayInitialized = false;
            break;
        }
    }
}

// Modify the displayAllFrames function to display each frame for 5 seconds
void displayAllFrames(const std::vector<VideoFrame>& frames) {
    std::cout << "\n===== Displaying Frame Images =====\n" << std::endl;
    
    if (frames.empty()) {
        std::cout << "No frames to display" << std::endl;
        return;
    }
    
    // Initialize the display window once
    if (!g_displayInitialized) {
        if (!InitializeDisplayWindow()) {
            return;
        }
    }
    
    // Display frames with a 5 second delay between them
    for (const auto& frame : frames) {
        displayFrameImage(frame, frame.frameNumber);
        
        // Display each frame for 5 seconds as requested
        std::cout << "Showing frame " << frame.frameNumber << " for 5 seconds..." << std::endl;
        
        // Handle window messages while waiting to ensure UI responsiveness
        DWORD startTime = GetTickCount();
        const DWORD displayTime = 5000; // 5 seconds in milliseconds (changed from 1000)
        
        while (GetTickCount() - startTime < displayTime) {
            // Process window messages while waiting
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                
                if (msg.message == WM_QUIT) {
                    g_displayInitialized = false;
                    break;
                }
            }
            
            if (!g_displayInitialized) break;
            
            // Short sleep to avoid maxing out CPU
            Sleep(10);
        }
        
        // Check if window was closed
        if (!g_displayInitialized) {
            break;
        }
    }
    
    std::cout << "All " << frames.size() << " frames have been displayed" << std::endl;
    std::cout << "Press Enter in the console to continue..." << std::endl;
    std::cin.get(); // Wait for user to press Enter
    
    // Clean up display resources
    CleanupDisplay();
}

// Modified extractFrames function
std::vector<VideoFrame> extractFrames(
    const std::vector<std::vector<bool>>& channels,
    const std::vector<size_t>& savIndices,
    const std::vector<size_t>& eavIndices,
    const std::vector<size_t>& saviIndices,
    const std::vector<size_t>& eaviIndices) {
    
    std::vector<VideoFrame> frames;
    const size_t patternLen = 32;
    const size_t NORMAL_LINE_GAP = 1776;  // Normal gap between SAV markers
    const size_t FRAME_BOUNDARY_THRESHOLD = 2 * NORMAL_LINE_GAP;  // Threshold to detect frame boundaries
    
    // Add safety checks
    if (channels.empty() || channels[0].empty()) {
        std::cout << "Error: No channel data available" << std::endl;
        return frames;
    }
    
    // Create sorted copies of indices
    std::vector<size_t> sortedSav = savIndices;
    std::vector<size_t> sortedEav = eavIndices;
    std::sort(sortedSav.begin(), sortedSav.end());
    std::sort(sortedEav.begin(), sortedEav.end());
    
    // Initialize first frame
    VideoFrame currentFrame;
    currentFrame.frameNumber = 1;
    
    std::cout << "\nAnalyzing line spacing..." << std::endl;
    
    // Process SAV positions
    for (size_t i = 0; i < sortedSav.size(); i++) {
        size_t savPos = sortedSav[i];
        
        // Check if this is a frame boundary by looking at gap to next SAV
        bool isFrameBoundary = false;
        if (i < sortedSav.size() - 1) {
            size_t gap = sortedSav[i + 1] - savPos;
            if (gap > FRAME_BOUNDARY_THRESHOLD) {
                std::cout << "Found frame boundary at SAV[" << i << "] - Gap: " << gap << " bits" << std::endl;
                isFrameBoundary = true;
            }
        }
        
        // Find corresponding EAV
        auto eavIt = std::upper_bound(sortedEav.begin(), sortedEav.end(), savPos);
        if (eavIt == sortedEav.end()) continue;
        size_t eavPos = *eavIt;
        
        // Create and add line
        VideoLine line;
        line.startIndex = savPos;
        line.endIndex = eavPos;
        
        // Extract data between SAV and EAV for each channel
        std::vector<std::vector<uint8_t>> channelBytes(4);
        
        for (size_t ch = 0; ch < channels.size(); ch++) {
            size_t start = savPos + patternLen;
            size_t end = eavPos;
            
            if (start >= end || end > channels[ch].size()) continue;
            
            // Convert bits to bytes - MODIFIED FOR LITTLE-ENDIAN
            std::vector<uint8_t> bytes;
            for (size_t pos = start; pos < end; pos += 8) {
                if (pos + 8 > end) break;
                
                uint8_t byte = 0;
                for (size_t bit = 0; bit < 8; bit++) {
                    // Little-endian: least significant bit first
                    // Use OR with shifted bit position instead of shifting the byte
                    byte |= (channels[ch][pos + bit] ? 1 : 0) << bit;
                }
                channelBytes[ch].push_back(byte);
            }
            
            // Add this after the bit-to-byte conversion loop to see sample values
            if (ch == 0 && bytes.size() > 0 && bytes.size() % 100 == 0) {
                std::cout << "Channel " << ch << " byte samples (little-endian): ";
                for (int i = 0; i < std::min<size_t>(10, bytes.size()); i++) {
                    std::cout << std::hex << std::setw(2) << std::setfill('0') 
                              << static_cast<int>(bytes[i]) << " ";
                }
                std::cout << std::dec << std::endl;
                
                // Also calculate what the big-endian version would be for comparison
                std::cout << "Same bytes if big-endian: ";
                for (int i = 0; i < std::min<size_t>(10, bytes.size()); i++) {
                    uint8_t bigEndianByte = 0;
                    for (int bit = 0; bit < 8; bit++) {
                        bigEndianByte = (bigEndianByte << 1) | ((bytes[i] >> bit) & 1);
                    }
                    std::cout << std::hex << std::setw(2) << std::setfill('0') 
                              << static_cast<int>(bigEndianByte) << " ";
                }
                std::cout << std::dec << std::endl;
            }
        }
        
        // Now interleave the channels like in MATLAB
        // First, determine the min length to ensure we don't go out of bounds
        size_t minLength = SIZE_MAX;
        for (const auto& chBytes : channelBytes) {
            minLength = (chBytes.size() < minLength) ? chBytes.size() : minLength;
        }
        
        if (minLength == 0) continue; // Skip if any channel has no data
        
        // Interleave the channels into a single vector
        std::vector<uint8_t> interleavedData(minLength * 4);
        for (size_t idx = 0; idx < minLength; idx++) {
            for (size_t ch = 0; ch < 4; ch++) {
                interleavedData[idx * 4 + ch] = channelBytes[ch][idx];
            }
        }
        
        // Store the interleaved data in the line
        line.channel1 = channelBytes[0]; // Keep individual channels for compatibility
        line.channel2 = channelBytes[1];
        line.channel3 = channelBytes[2];
        line.channel4 = channelBytes[3];
        line.interleavedData = interleavedData; // Add new field for interleaved data
        
        currentFrame.lines.push_back(line);
        
        // If this was a frame boundary, complete the current frame and start a new one
        if (isFrameBoundary && !currentFrame.lines.empty()) {
            frames.push_back(currentFrame);
            std::cout << "Completed frame " << currentFrame.frameNumber 
                      << " with " << currentFrame.lines.size() << " lines" << std::endl;
            
            // Start new frame
            currentFrame = VideoFrame();
            currentFrame.frameNumber = frames.size() + 1;
        }
        
        // Print progress less frequently
        if (currentFrame.lines.size() % 1000 == 0) {
            std::cout << "Current frame: Processed " << currentFrame.lines.size() << " lines" << std::endl;
        }
    }
    
    // Add last frame if it has any lines
    if (!currentFrame.lines.empty()) {
        frames.push_back(currentFrame);
    }
    
    // Print detailed frame summary
    std::cout << "\nDetailed Frame Summary:" << std::endl;
    for (const auto& frame : frames) {
        std::cout << "Frame " << frame.frameNumber << ": " << frame.lines.size() 
                  << " lines (Start: " << frame.lines.front().startIndex 
                  << ", End: " << frame.lines.back().endIndex 
                  << ", First line length: " << (frame.lines.back().endIndex - frame.lines.front().startIndex)
                  << " bits)" << std::endl;
        
        // Print the first few line gaps in this frame
        if (!frame.lines.empty() && frame.lines.size() > 1) {
            std::cout << "  First few line gaps in frame " << frame.frameNumber << ":" << std::endl;
            for (size_t i = 0; i < std::min<size_t>(5, frame.lines.size() - 1); i++) {
                size_t gap = frame.lines[i + 1].startIndex - frame.lines[i].startIndex;
                std::cout << "    Line " << i << " to " << (i + 1) << ": " << gap << " bits" << std::endl;
            }
        }
    }
    
    return frames;
}

// Modified analyzeData function to detect frame boundaries properly
void analyzeData(bool quickAnalysis) {
    std::cout << "\n=== Starting Data Analysis ===\n" << std::endl;
    
    // Check if we have data to analyze
    if (g_analysisBuffer.empty()) {
        std::cerr << "No data available for analysis." << std::endl;
        return;
    }
    
    std::cout << "Analyzing " << g_analysisBuffer.size() << " bytes of data..." << std::endl;
    
    // Convert raw bytes to uint32 for pattern search
    size_t numElements = g_analysisBuffer.size() / sizeof(uint32_t);
    std::vector<uint32_t> data(numElements);
    memcpy(data.data(), g_analysisBuffer.data(), g_analysisBuffer.size());
    
    // Convert to bits
    std::vector<uint8_t> rawBits = lookAtBits(data, false, "little", false, "uint32");
    std::vector<bool> bits(rawBits.begin(), rawBits.end());
    
    // Create patterns for SAV and EAV
    std::vector<bool> savPattern = createSAVPattern();
    std::vector<bool> eavPattern = createEAVPattern();
    
    std::cout << "Searching for SAV/EAV patterns in channel 0 to determine frame structure..." << std::endl;
    
    // Split the bits into 4 channels
    std::vector<std::vector<bool>> channelBits(4);
    for (size_t i = 0; i < bits.size(); i++) {
        channelBits[i % 4].push_back(bits[i]);
    }
    
    // Search for patterns in only the first channel for efficiency
    std::vector<size_t> savPositions = findPattern(channelBits[0], savPattern);
    std::vector<size_t> eavPositions = findPattern(channelBits[0], eavPattern);
    
    std::cout << "Found " << savPositions.size() << " SAV markers in channel 0" << std::endl;
    std::cout << "Found " << eavPositions.size() << " EAV markers in channel 0" << std::endl;
    std::cout << "Using channel 0 markers for frame structure analysis..." << std::endl;

    // Analyze a sample of SAV/EAV pairs to get actual pixel counts
    if (!savPositions.empty() && !eavPositions.empty()) {
        std::cout << "\n=== Analyzing SAV/EAV Pixel Data ===\n" << std::endl;
        
        // For the first few SAV markers, find their matching EAV markers
        const int samplesToShow = 5; // Number of SAV/EAV pairs to analyze
        int samplesFound = 0;
        
        std::cout << "Sample SAV/EAV pairs from channel 0:" << std::endl;
        
        for (size_t i = 0; i < savPositions.size() && samplesFound < samplesToShow; i++) {
            // Find the corresponding EAV that comes after this SAV
            auto eavIt = std::upper_bound(eavPositions.begin(), eavPositions.end(), savPositions[i]);
            
            if (eavIt != eavPositions.end()) {
                // Calculate distance in bits and bytes
                size_t savPos = savPositions[i];
                size_t eavPos = *eavIt;
                size_t distanceBits = eavPos - savPos;
                size_t distanceBytes = (distanceBits - 64) / 8; // Subtract SAV/EAV markers (each 32 bits) and convert to bytes
                
                std::cout << "Pair " << (samplesFound + 1) << ": SAV at " << savPos 
                          << ", EAV at " << eavPos 
                          << " | Distance: " << distanceBits << " bits"
                          << " | Pixel data: " << distanceBytes << " bytes in one channel" << std::endl;
                
                // Calculate how many pixels this would be across all 4 channels
                size_t totalPixelsAcrossChannels = distanceBytes * 4;
                std::cout << "  → When interleaved, this row would contain " << totalPixelsAcrossChannels 
                          << " total pixels across all 4 channels" << std::endl;
                
                samplesFound++;
            }
        }
        
        // Calculate the average for a larger sample if enough data is available
        if (savPositions.size() >= 10) {
            size_t totalBits = 0;
            int validSamples = 0;
            
            // Use first 50 samples for average calculation (or fewer if not available)
            size_t sampleLimit = std::min<size_t>(savPositions.size(), 50);
            
            for (size_t i = 0; i < sampleLimit; i++) {
                auto eavIt = std::upper_bound(eavPositions.begin(), eavPositions.end(), savPositions[i]);
                if (eavIt != eavPositions.end()) {
                    size_t distanceBits = *eavIt - savPositions[i];
                    
                    // Only include reasonable distances
                    if (distanceBits > 100 && distanceBits < 5000) {
                        totalBits += distanceBits;
                        validSamples++;
                    }
                }
            }
            
            if (validSamples > 0) {
                double avgBitsPerLine = static_cast<double>(totalBits) / validSamples;
                double avgBytesPerChannel = (avgBitsPerLine - 64) / 8.0; // Subtract SAV/EAV markers
                double avgTotalPixels = avgBytesPerChannel * 4; // Multiply by 4 channels
                
                std::cout << "\nAverage Line Statistics (from " << validSamples << " samples):" << std::endl;
                std::cout << "  • Average bits between SAV and EAV: " << avgBitsPerLine << " bits" << std::endl;
                std::cout << "  • Average pixel data per channel: " << avgBytesPerChannel << " bytes" << std::endl;
                std::cout << "  • Average total pixels per row (all channels): " << avgTotalPixels << " pixels" << std::endl;
            }
        }
    }

    // Calculate normal line gaps to identify frame boundaries
    std::vector<int> lineGaps;
    for (size_t i = 1; i < savPositions.size(); i++) {
        int gap = static_cast<int>(savPositions[i] - savPositions[i-1]);
        lineGaps.push_back(gap);
    }

    // Find the most common line gap (typical line distance)
    int normalLineGap = 0;
    if (!lineGaps.empty()) {
        std::sort(lineGaps.begin(), lineGaps.end());
        
            int currentCount = 1;
            int maxCount = 1;
        int mostCommonGap = lineGaps[0];
            
        for (size_t i = 1; i < lineGaps.size(); i++) {
            if (lineGaps[i] == lineGaps[i-1]) {
                    currentCount++;
                } else {
                    if (currentCount > maxCount) {
                        maxCount = currentCount;
                    mostCommonGap = lineGaps[i-1];
                    }
                    currentCount = 1;
                }
            }
            
            // Check the last sequence
            if (currentCount > maxCount) {
            mostCommonGap = lineGaps.back();
        }
        
        normalLineGap = mostCommonGap;
        std::cout << "\nAnalyzing line spacing in channel 0..." << std::endl;
        std::cout << "Normal line gap between SAV markers in channel 0: " << normalLineGap << " bits" << std::endl;
    }

    // Detect frame boundaries using significant gaps between SAV markers
    std::vector<size_t> frameStartIndices = {0}; // First SAV is start of first frame
    int frameBoundaryThreshold = normalLineGap * 2; // Gap threshold for frame boundary
    std::cout << "Using frame boundary threshold of " << frameBoundaryThreshold 
              << " bits (2× normal line gap) to detect frame boundaries" << std::endl;

    for (size_t i = 1; i < savPositions.size(); i++) {
        int gap = static_cast<int>(savPositions[i] - savPositions[i-1]);
        if (gap > frameBoundaryThreshold) {
            std::cout << "Frame boundary detected at position " << i 
                      << " (gap: " << gap << " bits, significantly larger than normal line gap)" << std::endl;
            frameStartIndices.push_back(i);
        }
    }

    std::cout << "\nDetected " << frameStartIndices.size() 
              << " potential frames in channel 0 data" << std::endl;

    // Create frame(s) based on the detected boundaries
    std::vector<VideoFrame> frames;

    // Add memory safety check
    size_t totalEstimatedLines = 0;
    for (size_t frameIdx = 0; frameIdx < frameStartIndices.size(); frameIdx++) {
        size_t startIdx = frameStartIndices[frameIdx];
        size_t endIdx = (frameIdx < frameStartIndices.size() - 1) ? 
                        frameStartIndices[frameIdx + 1] : savPositions.size();
        
        int frameRows = static_cast<int>(endIdx - startIdx);
        totalEstimatedLines += frameRows;
        
        // Limit total lines to prevent memory issues
        if (totalEstimatedLines > 5000) {
            std::cout << "Warning: Limiting frame analysis to prevent memory overflow." << std::endl;
            // If we need to limit frames, keep only up to the current frame index
            frameStartIndices.resize(frameIdx + 1);
                    break;
        }
    }

    // In the analyzeData function, replace the frame creation and display section with this code:

    std::cout << "\nProcessing and displaying " << frameStartIndices.size() << " frames one at a time..." << std::endl;
    
    try {
        // Process each frame individually
        for (size_t frameIdx = 0; frameIdx < frameStartIndices.size(); frameIdx++) {
            size_t startIdx = frameStartIndices[frameIdx];
            size_t endIdx = (frameIdx < frameStartIndices.size() - 1) ? 
                            frameStartIndices[frameIdx + 1] : savPositions.size();
            
            int frameRows = static_cast<int>(endIdx - startIdx);
            std::cout << "Processing frame " << (frameIdx + 1) << " with " << frameRows << " rows" << std::endl;
            
            // Create a single frame
            VideoFrame frame;
            frame.frameNumber = static_cast<int>(frameIdx + 1);
            frame.lines.reserve(frameRows); // Reserve space but don't allocate yet
            
            // Process lines in smaller batches to avoid memory issues
            const size_t BATCH_SIZE = 100; // Process 100 lines at a time
            
            for (size_t batchStart = startIdx; batchStart < endIdx; batchStart += BATCH_SIZE) {
                size_t batchEnd = batchStart + BATCH_SIZE;
                if (batchEnd > endIdx) batchEnd = endIdx;
                
                size_t batchStartLine = batchStart - startIdx;
                size_t batchEndLine = batchEnd - startIdx;
                
                std::cout << "  Processing batch of lines " << batchStartLine << " to " 
                          << batchEndLine << " of frame " << (frameIdx + 1) << std::endl;
                
                // Process each line in this batch
                for (size_t i = batchStart; i < batchEnd; i++) {
                    if (i >= savPositions.size()) break;
                    
                    VideoLine line;
                    line.startIndex = savPositions[i];
                    
                    // Find corresponding EAV
                    size_t eavIdx = 0;
                    for (size_t j = 0; j < eavPositions.size(); j++) {
                        if (eavPositions[j] > savPositions[i]) {
                            eavIdx = j;
                            break;
                        }
                    }
                    
                    if (eavIdx < eavPositions.size()) {
                        line.endIndex = eavPositions[eavIdx];
                    } else {
                        // If no EAV found, use SAV + estimated active video width
                        line.endIndex = savPositions[i] + 1456; // Using detected row width in bits
                    }
                    
                    // Extract data directly from the deinterleaved channels
                    size_t dataStartBit = savPositions[i] + 32;  // Skip SAV marker (32 bits)
                    size_t dataEndBit = (eavIdx < eavPositions.size()) ? 
                                       eavPositions[eavIdx] : (savPositions[i] + 1456);
                    
                    // Initialize vectors for each channel's data with proper capacity
                    size_t expectedBytes = (dataEndBit - dataStartBit + 7) / 8;
                    line.channel1.reserve(expectedBytes);
                    line.channel2.reserve(expectedBytes);
                    line.channel3.reserve(expectedBytes);
                    line.channel4.reserve(expectedBytes);
                    
                    // Extract data from each channel and convert bits to bytes
                    for (size_t ch = 0; ch < channelBits.size(); ch++) {
                        std::vector<uint8_t>* targetChannel = nullptr;
                        switch (ch) {
                            case 0: targetChannel = &line.channel1; break;
                            case 1: targetChannel = &line.channel2; break;
                            case 2: targetChannel = &line.channel3; break;
                            case 3: targetChannel = &line.channel4; break;
                            default: continue; // Shouldn't happen
                        }
                        
                        // Convert bits to bytes
                        for (size_t pos = dataStartBit; pos < dataEndBit; pos += 8) {
                            if (pos + 8 > channelBits[ch].size()) break;
                            
                            uint8_t byte = 0;
                            for (size_t bit = 0; bit < 8; bit++) {
                                // Big-endian: most significant bit first
                                byte |= (channelBits[ch][pos + bit] ? 1 : 0) << (7 - bit);
                            }
                            targetChannel->push_back(byte);
                        }
                    }
                    
                    frame.lines.push_back(line);
                }
            }
            
            if (!frame.lines.empty()) {
                std::cout << "Frame " << frameIdx + 1 << " has " << frame.lines.size() << " lines" << std::endl;
                
                // Create a special display function that only displays this one frame
                std::cout << "Displaying frame " << frameIdx + 1 << "..." << std::endl;
                
                // Display this frame
                std::vector<VideoFrame> singleFrame;
                singleFrame.push_back(frame);
                displayAllFrames(singleFrame);
                
                // Clear memory immediately
                singleFrame.clear();
            }
            
            // Force cleanup of this frame's data before processing the next frame
            frame.lines.clear();
            frame.lines.shrink_to_fit();
            
            // Force a garbage collection to free memory before next frame
            std::cout << "Memory cleared after frame " << frameIdx + 1 << std::endl;
        }
        
        std::cout << "All " << frameStartIndices.size() << " frames have been processed and displayed" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error during frame processing: " << e.what() << std::endl;
        // Continue with the program despite the error
    } catch (...) {
        std::cerr << "Unknown error during frame processing" << std::endl;
        // Continue with the program despite the error
    }
    
    // Return from function
    return;
}

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
        // Sleep for the check interval, but check for exit signal more frequently
        for (int i = 0; i < 10; i++) {  // Check 10 times per interval
            if (!g_programRunning) {
                break;  // Exit early if program is shutting down
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_CHECK_INTERVAL_MS / 10));
        }
        
        // Exit early if program is shutting down
        if (!g_programRunning) {
            break;
        }

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
        }
        else if (currentStage == 2) {
            // In transfer stage, check bytes transferred
            long currentBytes = g_totalBytesTransferred.load();

            if (currentBytes > lastBytesTransferred) {
                // Progress in data transfer
                std::cout << "Progress detected: " << (currentBytes - lastBytesTransferred)
                    << " bytes transferred since last check." << std::endl;
                inactivityCounter = 0;
                g_lastProgressTime.store(nowMs);
            }
            else {
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
            }
            else {
                // Heartbeat changed, reset inactivity counter
                inactivityCounter = 0;
                g_lastProgressTime.store(nowMs);
            }
            lastHeartbeat = currentHeartbeat;
        }
        else {
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

// Helper function to create pattern with given code
std::vector<bool> createPattern(const std::string& codeType) {
    std::vector<bool> code1 = hexToBinaryVector("FF", 8);
    std::vector<bool> code2 = hexToBinaryVector("00", 8);
    std::vector<bool> code3 = hexToBinaryVector("00", 8);
    std::vector<bool> code = getCode(codeType);
    
    std::vector<bool> pattern;
    pattern.insert(pattern.end(), code1.begin(), code1.end());
    pattern.insert(pattern.end(), code2.begin(), code2.end());
    pattern.insert(pattern.end(), code3.begin(), code3.end());
    pattern.insert(pattern.end(), code.begin(), code.end());
    
    return pattern;
}

// Helper function to create SAV pattern
std::vector<bool> createSAVPattern() {
    return createPattern("sav");
}

// Helper function to create EAV pattern
std::vector<bool> createEAVPattern() {
    return createPattern("eav");
}

// Add these helper functions
std::vector<bool> createSAVIPattern() {
    return createPattern("savi");
}

std::vector<bool> createEAVIPattern() {
    return createPattern("eavi");
}

// Global variable to store GDI+ token
ULONG_PTR g_gdiplusToken = 0;

// Initialize and shutdown GDI+
void InitGDIPlus() {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
}

void ShutdownGDIPlus() {
    if (g_gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
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
    const long KB_TO_TRANSFER = 10 * 1024 / BUFFER_SIZE * BUFFER_SIZE / 1024; // ~10MB instead of 2MB
    const long TOTAL_BYTES_TO_TRANSFER = KB_TO_TRANSFER * 1024;

    // For 150 MB/s data rate
    const size_t DATA_RATE = 297 * 1024 * 1024;  // 297 MB/s
    const size_t RECOMMENDED_BUFFER = 2 * 1024 * 1024;  // 1MB for better write performance

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
    unsigned char** buffers = new unsigned char* [NUM_BUFFERS];

    try {
        // Define filename separately for easy modification
        std::string fileName = "camera16.bin";

        // Reserve space for the analysis buffer (approximately 100MB)
        std::cout << "Reserving " << (ANALYSIS_BUFFER_SIZE / (1024 * 1024)) << " MB for analysis buffer..." << std::endl;
        g_analysisBuffer.reserve(ANALYSIS_BUFFER_SIZE);
        updateProgress();

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

        // Instead of having separate flush and acquisition phases:

        // 1. Set up circular buffer management
        const size_t FLUSH_COUNT = 10;  // Reduced from 100 to just 10 buffer cycles for testing
        size_t bufferCycleCount = 0;
        bool flushComplete = false;

        // 2. Set up a single acquisition loop
        std::cout << "Starting continuous acquisition (initial cycles will be used for flushing)..." << std::endl;

        // Initialize all buffers once
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (!bulkInEndpoint->BeginDataXfer(buffers[i], BUFFER_SIZE, &ovLapArray[i])) {
                throw std::runtime_error("Failed to queue initial transfer");
            }
        }

        totalTransferred = 0;
        currentBuffer = 0;

        // Single acquisition loop
        while (g_analysisBuffer.size() < ANALYSIS_BUFFER_SIZE) {
            // Process buffer as before...
            // Wait for current buffer with adaptive timeout
            DWORD waitResult = WaitForSingleObject(ovLapArray[currentBuffer].hEvent, currentTimeout);
            
            // Handle the transfer completion as before...
                DWORD bytesXferred = 0;
                BOOL success = GetOverlappedResult(
                    bulkInEndpoint->hDevice,
                &ovLapArray[currentBuffer],
                    &bytesXferred,
                TRUE
            );
            
            // Make sure we declare this variable properly
            LONG transferred = static_cast<LONG>(bytesXferred);
            
            if (transferred > 0) {
                bufferCycleCount++;
                
                // Only start saving data after flush phase
                if (!flushComplete) {
                    if (bufferCycleCount >= FLUSH_COUNT) {
                        flushComplete = true;
                        std::cout << "Flush complete. Starting data collection..." << std::endl;
                    }
                } else {
                    // Save data only after flush phase
                long bytesToWrite = (transferred & ~0x3);  // Align to 4-byte boundary
                    
                    // No longer print for every buffer
                    g_analysisBuffer.insert(g_analysisBuffer.end(), 
                                                   buffers[currentBuffer], 
                                                   buffers[currentBuffer] + bytesToWrite);

                    totalTransferred += bytesToWrite;
                    g_totalBytesTransferred.store(totalTransferred);
                    
                    // Print progress only every 10 buffers instead of every buffer
                    if (bufferCycleCount % 10 == 0) {
                        std::cout << "Collected " << g_analysisBuffer.size() / (1024.0 * 1024.0) 
                                  << " MB of data (" << (g_analysisBuffer.size() * 100 / ANALYSIS_BUFFER_SIZE) 
                                  << "% complete)" << std::endl;
                    }
                
                    // Break once we have 2 MB of analysis data
                if (g_analysisBuffer.size() >= ANALYSIS_BUFFER_SIZE) {
                        std::cout << "Collected " << g_analysisBuffer.size() / (1024.0 * 1024.0) 
                                  << " MB of data for analysis. Stopping data collection." << std::endl;
                    break;  // Exit the transfer loop
                }
                }
                
                // Always re-queue the buffer immediately
                if (!bulkInEndpoint->BeginDataXfer(buffers[currentBuffer], BUFFER_SIZE,
                                                  &ovLapArray[currentBuffer])) {
                    throw std::runtime_error("Failed to re-queue transfer");
                }
            }
            else {
                // Handle case where no bytes were transferred - only print on failures
                std::cout << "Buffer cycle failed - 0 bytes transferred. waitResult=" << waitResult 
                          << ", success=" << (success ? "true" : "false") << std::endl;
                
                // Still re-queue the buffer to keep the pipeline moving
                if (!bulkInEndpoint->BeginDataXfer(buffers[currentBuffer], BUFFER_SIZE,
                                                  &ovLapArray[currentBuffer])) {
                    throw std::runtime_error("Failed to re-queue transfer after 0 bytes");
                }
            }

            // Move to next buffer
            currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
        }

        // Clean up
        g_programRunning.store(false);  // Signal watchdog to exit
        std::cout << "Shutting down watchdog thread..." << std::endl;
        Sleep(2000);  // Give watchdog thread time to exit cleanly

        std::cout << "Starting cleanup..." << std::endl;
        
        // Safe cleanup - handle each step independently to avoid cascading errors
        try {
            if (bulkInEndpoint) {
                std::cout << "Aborting any pending transfers..." << std::endl;
                bulkInEndpoint->Abort();
                g_bulkInEndpoint = nullptr;  // Clear global pointer
            }
        } catch (const std::exception& e) {
            std::cerr << "Error during endpoint abort: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown error during endpoint abort. Continuing cleanup..." << std::endl;
        }
        
        try {
            if (USBDevice) {
                std::cout << "Closing USB device..." << std::endl;
                USBDevice->Close();
                delete USBDevice;
                USBDevice = nullptr;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error closing USB device: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown error closing USB device. Continuing cleanup..." << std::endl;
        }
        
        try {
            // Clean up events
            std::cout << "Closing event handles..." << std::endl;
            for (int i = 0; i < NUM_BUFFERS; i++) {
                if (ovLapArray && ovLapArray[i].hEvent) {
                    CloseHandle(ovLapArray[i].hEvent);
                    ovLapArray[i].hEvent = NULL;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error closing event handles: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown error closing event handles. Continuing cleanup..." << std::endl;
        }
        
        try {
            // Free aligned memory
            std::cout << "Freeing buffer memory..." << std::endl;
            for (int i = 0; i < NUM_BUFFERS; i++) {
                if (buffers && buffers[i]) {
                    _aligned_free(buffers[i]);
                    buffers[i] = NULL;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error freeing buffer memory: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown error freeing buffer memory. Continuing cleanup..." << std::endl;
        }
        
        try {
            // Delete arrays
            std::cout << "Deleting array containers..." << std::endl;
            delete[] ovLapArray;
            delete[] buffers;
            ovLapArray = nullptr;
            buffers = nullptr;
        } catch (const std::exception& e) {
            std::cerr << "Error deleting arrays: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown error deleting arrays. Continuing cleanup..." << std::endl;
        }

        // Calculate and report overall performance
        LARGE_INTEGER endTime;
        QueryPerformanceCounter(&endTime);
        double totalElapsedSec = (endTime.QuadPart - perfStart.QuadPart) / (double)perfFreq.QuadPart;
        double avgMbps = (totalTransferred * 8.0) / (totalElapsedSec * 1000000.0);

        std::cout << "Transfer complete. Total bytes transferred: " << totalTransferred << " bytes." << std::endl;
        std::cout << "Average transfer rate: " << avgMbps << " Mbps ("
            << (totalTransferred / (1024.0 * 1024.0)) << " MB in "
            << totalElapsedSec << " seconds)" << std::endl;
        std::cout << "Data is now stored in memory buffer for analysis. Buffer size: " 
            << g_analysisBuffer.size() << " bytes." << std::endl;
            
        // Analyze the collected data
        try {
            bool quickAnalysis = false; // Set to false to do full analysis as requested
            analyzeData(quickAnalysis);
        } catch (const std::exception& e) {
            std::cerr << "Error during data analysis: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown error during data analysis" << std::endl;
        }

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;

        // Safe cleanup with detailed error handling
        try {
            // Abort any pending USB operations
            if (bulkInEndpoint) {
                bulkInEndpoint->Abort();
            }
        } catch (...) {
            std::cerr << "Error aborting endpoint operations during cleanup" << std::endl;
        }
        
        try {
            // Clean up any resources that might have been allocated
            for (int i = 0; i < NUM_BUFFERS; i++) {
                if (ovLapArray && ovLapArray[i].hEvent) {
                    CloseHandle(ovLapArray[i].hEvent);
                    ovLapArray[i].hEvent = NULL;
                }
            }
        } catch (...) {
            std::cerr << "Error closing event handles during cleanup" << std::endl;
        }
        
        try {
            for (int i = 0; i < NUM_BUFFERS; i++) {
                if (buffers && buffers[i]) {
                    _aligned_free(buffers[i]);
                    buffers[i] = NULL;
                }
            }
        } catch (...) {
            std::cerr << "Error freeing buffer memory during cleanup" << std::endl;
        }
        
        try {
            delete[] ovLapArray;
            delete[] buffers;
        } catch (...) {
            std::cerr << "Error deleting arrays during cleanup" << std::endl;
        }

        try {
            if (USBDevice) {
                USBDevice->Close();
                delete USBDevice;
            }
        } catch (...) {
            std::cerr << "Error closing USB device during cleanup" << std::endl;
        }

        return -1;
    }
}