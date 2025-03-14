#include <iostream>
#include <fstream>
#include <vector>      // Include the vector header for std::vector
#include <windows.h>
#include <algorithm>   // Add this for std::min
#include "CyAPI.h"     // Include Cypress CyAPI header for USB communication
#include <thread>      // For std::thread
#include <atomic>      // For std::atomic
#include <chrono>      // For std::chrono
#include <bitset>      // For bit manipulation
#include <string>      // For string operations
#include <map>         // For mapping codes to patterns
#include <set>         // For set operations (intersect)

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
const size_t ANALYSIS_BUFFER_SIZE = 10 * 1024 * 1024; // 2 MB for analysis

// Global buffer to store received data for analysis
std::vector<unsigned char> g_analysisBuffer;

// Helper functions for data analysis

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
        result = {1, 0, 0, 0, 0, 0, 0, 0};
    } else if (code == "savi") {
        // SAVI: Start of Active Video Invalid (0xC0)
        result = {1, 1, 0, 0, 0, 0, 0, 0};
    } else if (code == "eav") {
        // EAV: End of Active Video (0x9D)
        result = {1, 0, 0, 1, 1, 1, 0, 1};
    } else if (code == "eavi") {
        // EAVI: End of Active Video Invalid (0xDD)
        result = {1, 1, 0, 1, 1, 1, 0, 1};
    }
    
    return result;
}

// Convert uint32 data to bits with specified endianness (optimized version)
std::vector<bool> lookAtBits(const std::vector<uint32_t>& data, bool useGPU, const std::string& endian, bool verbose, const std::string& bitDepth) {
    // Determine bit size based on bit depth
    int numBits = 32; // Default for uint32
    if (bitDepth == "uint8") {
        numBits = 8;
    } else if (bitDepth == "uint16") {
        numBits = 16;
    }
    
    // Pre-allocate the result vector with exact size to avoid any reallocations
    size_t totalBits = data.size() * numBits;
    std::vector<bool> bits(totalBits); // Create with exact size
    
    if (verbose) {
        std::cout << "Converting " << data.size() << " elements to " << totalBits << " bits..." << std::endl;
    }
    
    // Process all elements at once with direct indexing for better performance
    if (endian == "little") {
        // Little endian: LSB first
        #pragma omp parallel for if(data.size() > 10000) // Use OpenMP for large datasets
        for (int i = 0; i < data.size(); ++i) {
            uint32_t value = data[i];
            size_t baseIdx = i * numBits;
            
            for (int bit = 0; bit < numBits; ++bit) {
                bits[baseIdx + bit] = (value >> bit) & 1;
            }
        }
    } else {
        // Big endian: MSB first
        #pragma omp parallel for if(data.size() > 10000) // Use OpenMP for large datasets
        for (int i = 0; i < data.size(); ++i) {
            uint32_t value = data[i];
            size_t baseIdx = i * numBits;
            
            for (int bit = 0; bit < numBits; ++bit) {
                bits[baseIdx + bit] = (value >> (numBits - 1 - bit)) & 1;
            }
        }
    }
    
    return bits;
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

// Analyze the collected data for patterns
void analyzeData(bool quickAnalysis = false) {
    std::cout << "\n=== Starting Data Analysis ===\n" << std::endl;
    
    // Set verbose mode for detailed progress reporting
    bool verbose = true;
    
    if (quickAnalysis) {
        std::cout << "Quick analysis mode enabled - skipping pattern search." << std::endl;
    }
    
    // Variables (similar to MATLAB)
    std::string bitDepthFX3 = "uint32";
    std::string endian = "little";
    bool useGPU = false;
    
    // Check if we have data to analyze
    if (g_analysisBuffer.empty()) {
        std::cerr << "No data available for analysis." << std::endl;
        return;
    }
    
    // Check if we have enough data for meaningful analysis
    const size_t MIN_BYTES_NEEDED = 1000 * sizeof(uint32_t); // At least 1000 uint32 elements
    if (g_analysisBuffer.size() < MIN_BYTES_NEEDED) {
        std::cerr << "Warning: Buffer contains only " << g_analysisBuffer.size() 
                  << " bytes, which may be insufficient for meaningful analysis." << std::endl;
        std::cerr << "Proceeding with limited data..." << std::endl;
    }
    
    std::cout << "Analyzing " << g_analysisBuffer.size() << " bytes of data..." << std::endl;
    
    // Convert raw bytes to uint32 (assuming data is already in uint32 format)
    size_t numElements = g_analysisBuffer.size() / sizeof(uint32_t);
    std::vector<uint32_t> data(numElements);
    
    // Copy data from g_analysisBuffer to data vector
    std::cout << "Copying data to analysis buffer..." << std::endl;
    auto startTime = std::chrono::high_resolution_clock::now();
    memcpy(data.data(), g_analysisBuffer.data(), g_analysisBuffer.size());
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    std::cout << "Data copy completed in " << duration << " ms." << std::endl;
    
    // Limit data size for analysis (similar to MATLAB's 5e6 limit)
    const size_t MAX_ELEMENTS = 2500000; // 250,000 elements (about 1MB for uint32)
    if (data.size() > MAX_ELEMENTS) {
        std::cout << "Limiting analysis to first " << MAX_ELEMENTS << " elements (" 
                  << (MAX_ELEMENTS * sizeof(uint32_t) / (1024.0 * 1024.0)) << " MB)." << std::endl;
        data.resize(MAX_ELEMENTS);
    }
    
    // Convert to proper type then to binary bits
    std::cout << "Converting data to bits..." << std::endl;
    startTime = std::chrono::high_resolution_clock::now();
    std::vector<bool> bits = lookAtBits(data, useGPU, endian, verbose, bitDepthFX3);
    endTime = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    std::cout << "Bit conversion completed in " << duration << " ms. Total bits: " << bits.size() << std::endl;
    
    // Reshape bits into columns (similar to MATLAB's reshape)
    std::vector<bool> columnBits = bits; // No need to reshape in C++
    
    // Split the data into four channels
    std::cout << "Splitting data into channels..." << std::endl;
    startTime = std::chrono::high_resolution_clock::now();
    
    // Pre-allocate vectors with exact size
    const size_t channelSize = columnBits.size() / 4;
    std::vector<bool> bits1(channelSize);
    std::vector<bool> bits2(channelSize);
    std::vector<bool> bits3(channelSize);
    std::vector<bool> bits4(channelSize);
    
    // Process in batches for better performance
    const size_t BATCH_SIZE = 10000;
    const size_t numBatches = (columnBits.size() / 4 + BATCH_SIZE - 1) / BATCH_SIZE;
    
    if (verbose) {
        std::cout << "Processing " << numBatches << " batches for channel splitting..." << std::endl;
    }
    
    size_t channelIdx = 0;
    for (size_t batch = 0; batch < numBatches; ++batch) {
        if (verbose && batch % 10 == 0) {
            std::cout << "Processing batch " << batch + 1 << " of " << numBatches << " for channel splitting..." << std::endl;
        }
        
        size_t startIdx = batch * BATCH_SIZE * 4;
        size_t endIdx = std::min<size_t>(startIdx + BATCH_SIZE * 4, columnBits.size() - 3);
        
        for (size_t i = startIdx; i < endIdx; i += 4) {
            if (channelIdx < channelSize) {
                bits3[channelIdx] = columnBits[i];     // 1:4:end
                bits1[channelIdx] = columnBits[i + 1]; // 2:4:end
                bits2[channelIdx] = columnBits[i + 2]; // 3:4:end
                bits4[channelIdx] = columnBits[i + 3]; // 4:4:end
                channelIdx++;
            }
        }
    }
    
    endTime = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    std::cout << "Channel splitting completed in " << duration << " ms." << std::endl;
    std::cout << "Channel bit counts: Ch1: " << bits1.size() << ", Ch2: " << bits2.size() 
              << ", Ch3: " << bits3.size() << ", Ch4: " << bits4.size() << std::endl;
    
    // Define patterns to search for
    std::cout << "Creating patterns to search for..." << std::endl;
    startTime = std::chrono::high_resolution_clock::now();
    
    std::vector<bool> code1 = hexToBinaryVector("FF", 8);
    std::vector<bool> code2 = hexToBinaryVector("00", 8);
    std::vector<bool> code3 = hexToBinaryVector("00", 8);
    
    // Create the patterns
    std::vector<bool> patternStartValid, patternStartInvalid, patternEndValid, patternEndInvalid;
    
    // Concatenate vectors to create patterns
    patternStartValid.insert(patternStartValid.end(), code1.begin(), code1.end());
    patternStartValid.insert(patternStartValid.end(), code2.begin(), code2.end());
    patternStartValid.insert(patternStartValid.end(), code3.begin(), code3.end());
    std::vector<bool> savCode = getCode("sav");
    patternStartValid.insert(patternStartValid.end(), savCode.begin(), savCode.end());
    
    patternStartInvalid.insert(patternStartInvalid.end(), code1.begin(), code1.end());
    patternStartInvalid.insert(patternStartInvalid.end(), code2.begin(), code2.end());
    patternStartInvalid.insert(patternStartInvalid.end(), code3.begin(), code3.end());
    std::vector<bool> saviCode = getCode("savi");
    patternStartInvalid.insert(patternStartInvalid.end(), saviCode.begin(), saviCode.end());
    
    patternEndValid.insert(patternEndValid.end(), code1.begin(), code1.end());
    patternEndValid.insert(patternEndValid.end(), code2.begin(), code2.end());
    patternEndValid.insert(patternEndValid.end(), code3.begin(), code3.end());
    std::vector<bool> eavCode = getCode("eav");
    patternEndValid.insert(patternEndValid.end(), eavCode.begin(), eavCode.end());
    
    patternEndInvalid.insert(patternEndInvalid.end(), code1.begin(), code1.end());
    patternEndInvalid.insert(patternEndInvalid.end(), code2.begin(), code2.end());
    patternEndInvalid.insert(patternEndInvalid.end(), code3.begin(), code3.end());
    std::vector<bool> eaviCode = getCode("eavi");
    patternEndInvalid.insert(patternEndInvalid.end(), eaviCode.begin(), eaviCode.end());
    
    endTime = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    std::cout << "Pattern creation completed in " << duration << " ms." << std::endl;
    
    // Store patterns and channel data in vectors for easier iteration
    std::vector<std::vector<bool>> patternCell = {
        patternStartValid, patternStartInvalid, patternEndValid, patternEndInvalid
    };
    std::vector<std::string> patternNames = {"sav", "savi", "eav", "eavi"};
    std::vector<std::vector<bool>> bitsCell = {bits1, bits2, bits3, bits4};
    
    // Search for patterns in each channel
    std::cout << "\nSearching for patterns in each channel..." << std::endl;
    std::vector<std::vector<std::vector<size_t>>> indicesMatrix(patternCell.size(), 
                                                              std::vector<std::vector<size_t>>(bitsCell.size()));
    
    for (size_t p = 0; p < patternCell.size(); ++p) {
        const auto& pattern = patternCell[p];
        for (size_t ch = 0; ch < bitsCell.size(); ++ch) {
            std::cout << "Searching for " << patternNames[p] << " in channel " << (ch + 1) << "... ";
            std::cout.flush(); // Flush to show progress
            
            auto startTime = std::chrono::high_resolution_clock::now();
            indicesMatrix[p][ch] = findPattern(bitsCell[ch], pattern);
            auto endTime = std::chrono::high_resolution_clock::now();
            
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            std::cout << "Found " << indicesMatrix[p][ch].size() << " matches in " 
                      << duration << " ms." << std::endl;
        }
    }
    
    // Display pattern search results
    std::cout << "\nPattern Search Results:" << std::endl;
    for (size_t p = 0; p < patternCell.size(); ++p) {
        for (size_t ch = 0; ch < bitsCell.size(); ++ch) {
            std::cout << "Pattern: " << patternNames[p] << " in Channel " << (ch + 1) 
                      << " => Found " << indicesMatrix[p][ch].size() << " times." << std::endl;
        }
    }
    
    // Search for specific sync patterns across channels
    std::cout << "\nSearching for specific sync patterns..." << std::endl;
    
    // For each channel, find start-valid, start-invalid, end-valid, and end-invalid indices
    std::vector<size_t> idx1sav = findPattern(bits1, patternStartValid);
    std::vector<size_t> idx2sav = findPattern(bits2, patternStartValid);
    std::vector<size_t> idx3sav = findPattern(bits3, patternStartValid);
    std::vector<size_t> idx4sav = findPattern(bits4, patternStartValid);
    
    std::vector<size_t> idx1savi = findPattern(bits1, patternStartInvalid);
    std::vector<size_t> idx2savi = findPattern(bits2, patternStartInvalid);
    std::vector<size_t> idx3savi = findPattern(bits3, patternStartInvalid);
    std::vector<size_t> idx4savi = findPattern(bits4, patternStartInvalid);
    
    std::vector<size_t> idx1eav = findPattern(bits1, patternEndValid);
    std::vector<size_t> idx2eav = findPattern(bits2, patternEndValid);
    std::vector<size_t> idx3eav = findPattern(bits3, patternEndValid);
    std::vector<size_t> idx4eav = findPattern(bits4, patternEndValid);
    
    std::vector<size_t> idx1eavi = findPattern(bits1, patternEndInvalid);
    std::vector<size_t> idx2eavi = findPattern(bits2, patternEndInvalid);
    std::vector<size_t> idx3eavi = findPattern(bits3, patternEndInvalid);
    std::vector<size_t> idx4eavi = findPattern(bits4, patternEndInvalid);
    
    // Display counts of sync indices
    std::cout << "\nSync Pattern Counts:" << std::endl;
    std::cout << "Start valid patterns found: "
              << "Ch1: " << idx1sav.size() << ", "
              << "Ch2: " << idx2sav.size() << ", "
              << "Ch3: " << idx3sav.size() << ", "
              << "Ch4: " << idx4sav.size() << std::endl;
              
    std::cout << "End valid patterns found: "
              << "Ch1: " << idx1eav.size() << ", "
              << "Ch2: " << idx2eav.size() << ", "
              << "Ch3: " << idx3eav.size() << ", "
              << "Ch4: " << idx4eav.size() << std::endl;
              
    std::cout << "Start invalid patterns found: "
              << "Ch1: " << idx1savi.size() << ", "
              << "Ch2: " << idx2savi.size() << ", "
              << "Ch3: " << idx3savi.size() << ", "
              << "Ch4: " << idx4savi.size() << std::endl;
              
    std::cout << "End invalid patterns found: "
              << "Ch1: " << idx1eavi.size() << ", "
              << "Ch2: " << idx2eavi.size() << ", "
              << "Ch3: " << idx3eavi.size() << ", "
              << "Ch4: " << idx4eavi.size() << std::endl;
    
    // Get matching sync indices between channels (using intersections)
    std::cout << "\nFinding matching sync indices between channels..." << std::endl;
    
    // For SAV (Start Active Video)
    std::vector<size_t> matchingValues1 = intersect(idx1sav, idx2sav);
    std::vector<size_t> matchingValues2 = intersect(idx3sav, idx4sav);
    std::vector<size_t> matchingValues = intersect(matchingValues1, matchingValues2);
    
    idx1sav = matchingValues;
    idx2sav = matchingValues;
    idx3sav = matchingValues;
    idx4sav = matchingValues;
    
    // For EAV (End Active Video)
    matchingValues1 = intersect(idx1eav, idx2eav);
    matchingValues2 = intersect(idx3eav, idx4eav);
    matchingValues = intersect(matchingValues1, matchingValues2);
    
    idx1eav = matchingValues;
    idx2eav = matchingValues;
    idx3eav = matchingValues;
    idx4eav = matchingValues;
    
    // For SAVI (Start Active Video Invalid)
    matchingValues1 = intersect(idx1savi, idx2savi);
    matchingValues2 = intersect(idx3savi, idx4savi);
    matchingValues = intersect(matchingValues1, matchingValues2);
    
    idx1savi = matchingValues;
    idx2savi = matchingValues;
    idx3savi = matchingValues;
    idx4savi = matchingValues;
    
    // For EAVI (End Active Video Invalid)
    matchingValues1 = intersect(idx1eavi, idx2eavi);
    matchingValues2 = intersect(idx3eavi, idx4eavi);
    matchingValues = intersect(matchingValues1, matchingValues2);
    
    idx1eavi = matchingValues;
    idx2eavi = matchingValues;
    idx3eavi = matchingValues;
    idx4eavi = matchingValues;
    
    // Display final results after intersection
    std::cout << "\nFinal Results (after intersection):" << std::endl;
    std::cout << "Matching SAV patterns across all channels: " << idx1sav.size() << std::endl;
    std::cout << "Matching EAV patterns across all channels: " << idx1eav.size() << std::endl;
    std::cout << "Matching SAVI patterns across all channels: " << idx1savi.size() << std::endl;
    std::cout << "Matching EAVI patterns across all channels: " << idx1eavi.size() << std::endl;
    
    // Filter good indices using matchIdxs function
    std::cout << "\nFiltering and matching indices..." << std::endl;
    matchIdxs(idx1sav, idx1eav);
    matchIdxs(idx2sav, idx2eav);
    matchIdxs(idx3sav, idx3eav);
    matchIdxs(idx4sav, idx4eav);
    matchIdxs(idx1savi, idx1eavi);
    matchIdxs(idx2savi, idx2eavi);
    matchIdxs(idx3savi, idx3eavi);
    matchIdxs(idx4savi, idx4eavi);
    
    std::cout << "Filtered sav: " << idx1sav.size() << ", Filtered eav: " << idx1eav.size() << std::endl;
    std::cout << "Filtered savi: " << idx1savi.size() << ", Filtered eavi: " << idx1eavi.size() << std::endl;
    
    std::cout << "\n=== Analysis Complete ===\n" << std::endl;
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

int main() {
    std::cout << "Starting program..." << std::endl;

    // Start watchdog thread
    std::thread watchdog(watchdogThread);
    watchdog.detach();  // Detach so it can run independently

    // Updated buffer size and count to match firmware configuration
    // const long BUFFER_SIZE = 61440;  // Moved to global scope
    // const int NUM_BUFFERS = 3;       // Moved to global scope

    // Calculate total bytes to transfer based on buffer size (approximately 100MB)
    const long KB_TO_TRANSFER = 100000 * 1024 / BUFFER_SIZE * BUFFER_SIZE / 1024; // Adjust to be a multiple of buffer size
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
                if (consecutiveSuccesses > 5 && currentTimeout > 50) {
                    currentTimeout = 50;  // Reduce to 50ms after success
                }
            }
            else if (waitResult == WAIT_TIMEOUT) {
                // Handle timeout
                consecutiveSuccesses = 0;
                consecutiveErrors++;

                // Increase timeout after failures
                if (consecutiveErrors > 2) {
                    currentTimeout = std::min<DWORD>(currentTimeout * 2, 1000);  // Double timeout up to 1 seconds
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

                // Store data in memory buffer for later analysis
                g_analysisBuffer.insert(g_analysisBuffer.end(), buffers[currentBuffer], buffers[currentBuffer] + bytesToWrite);
                
                // Check if we've reached the buffer size limit
                if (g_analysisBuffer.size() >= ANALYSIS_BUFFER_SIZE) {
                    std::cout << "Analysis buffer full (" << (g_analysisBuffer.size() / (1024 * 1024)) 
                              << " MB). Stopping data collection." << std::endl;
                    break;  // Exit the transfer loop
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
        std::cout << "Shutting down watchdog thread..." << std::endl;
        Sleep(2000);  // Give watchdog thread time to exit cleanly

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
        std::cout << "Data is now stored in memory buffer for analysis. Buffer size: " 
            << g_analysisBuffer.size() << " bytes." << std::endl;
            
        // Analyze the collected data with quick analysis mode for faster results
        bool quickAnalysis = false; // Set to true for quick analysis, false for full analysis
        analyzeData(quickAnalysis);

        return 0;
    }
    catch (const std::exception& e) {
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
