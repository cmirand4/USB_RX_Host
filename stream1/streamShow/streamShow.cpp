/******************************************************************************
 * Example: Streaming from FX3 + Sync Code Parsing
 *
 * To compile (Windows, Visual Studio):
 *   1. Include CyAPI.lib in your linker settings (from the Cypress SDK).
 *   2. Ensure "CyAPI.h" is in your include path.
 *   3. Build as a console app or similar.
 ******************************************************************************/
#define NOMINMAX

#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>    // for std::search, std::set_intersection
#include <windows.h>
#include "CyAPI.h"
#include <string>

 // -----------------------------------------------------------------------------
 // Utility functions that mimic MATLAB behavior
 // -----------------------------------------------------------------------------

 // Convert a hex string (e.g. "9D") into an 8-bit binary vector [1,0,0,1,1,1,0,1] or similar.
std::vector<uint8_t> hexToBinaryVector(const std::string& hexStr, int totalBits = 8)
{
    // Convert hex to integer
    unsigned int val = std::stoul(hexStr, nullptr, 16);

    // Convert integer to binary vector
    std::vector<uint8_t> bits(totalBits, 0);
    for (int i = 0; i < totalBits; ++i)
    {
        // Extract bit i.  If we want the MSB first, we read from the top bit down.
        // For simplicity, let's assume the leftmost bit is the MSB:
        // bit i (MSB first) = (val >> (totalBits - 1 - i)) & 1
        // bit i (LSB first) = (val >> i) & 1
        // Because your MATLAB code sets 'endianness' = 'little' for the raw data,
        // the "sync codes" themselves in typical SDI are still often shown MSB-first.
        // We’ll assume MSB-first for the code pattern:
        bits[i] = static_cast<uint8_t>((val >> (totalBits - 1 - i)) & 0x1);
    }
    return bits;
}

// A convenience function that returns the 8-bit sync code pattern.
std::vector<uint8_t> getCode(const std::string& syncCode)
{
    //  In MATLAB:
    //    case 'sav'  -> hexToBinaryVector('80',8);
    //    case 'eav'  -> hexToBinaryVector('9D',8);
    //    case 'savi' -> hexToBinaryVector('AB',8);
    //    case 'eavi' -> hexToBinaryVector('B6',8);
    if (syncCode == "sav")
        return hexToBinaryVector("80", 8);
    else if (syncCode == "eav")
        return hexToBinaryVector("9D", 8);
    else if (syncCode == "savi")
        return hexToBinaryVector("AB", 8);
    else if (syncCode == "eavi")
        return hexToBinaryVector("B6", 8);

    // Default empty
    return std::vector<uint8_t>();
}

// Convert each element in `data` to a row of bits (vector of 0s/1s).
// This mimics your `lookAtBits` function.  By default, we assume 16 bits per pixel
// in little-endian order.  If you need 8-bit data or big-endian, adjust accordingly.
std::vector<uint8_t> lookAtBits_16(const std::vector<uint16_t>& data, bool littleEndian = true)
{
    // For each uint16_t, extract 16 bits into a 1D vector
    // The total size = data.size() * 16
    std::vector<uint8_t> bitStream;
    bitStream.reserve(data.size() * 16);

    for (auto val : data)
    {
        if (littleEndian)
        {
            // LSB first, i.e. bit 0 is least significant
            for (int bitIndex = 0; bitIndex < 16; ++bitIndex)
            {
                uint8_t bit = (val >> bitIndex) & 0x1;
                bitStream.push_back(bit);
            }
        }
        else
        {
            // MSB first
            for (int bitIndex = 15; bitIndex >= 0; --bitIndex)
            {
                uint8_t bit = (val >> bitIndex) & 0x1;
                bitStream.push_back(bit);
            }
        }
    }
    return bitStream;
}

// Overload for 8-bit data
std::vector<uint8_t> lookAtBits_8(const std::vector<uint8_t>& data, bool littleEndian = true)
{
    // For each uint8_t, extract 8 bits
    std::vector<uint8_t> bitStream;
    bitStream.reserve(data.size() * 8);

    for (auto val : data)
    {
        if (littleEndian)
        {
            for (int bitIndex = 0; bitIndex < 8; ++bitIndex)
            {
                uint8_t bit = (val >> bitIndex) & 0x1;
                bitStream.push_back(bit);
            }
        }
        else
        {
            for (int bitIndex = 7; bitIndex >= 0; --bitIndex)
            {
                uint8_t bit = (val >> bitIndex) & 0x1;
                bitStream.push_back(bit);
            }
        }
    }
    return bitStream;
}

// Find all occurrences of `pattern` in `bitStream` (similar to MATLAB's strfind).
// Return the starting indices (0-based).
std::vector<size_t> findPattern(const std::vector<uint8_t>& bitStream,
    const std::vector<uint8_t>& pattern)
{
    std::vector<size_t> indices;
    if (pattern.empty() || bitStream.empty()) return indices;

    // We'll do a naive search using std::search
    auto it = bitStream.begin();
    while (true)
    {
        it = std::search(it, bitStream.end(), pattern.begin(), pattern.end());
        if (it == bitStream.end()) break;

        // Found an occurrence
        size_t pos = std::distance(bitStream.begin(), it);
        indices.push_back(pos);
        ++it; // move past this match to look for the next
    }

    return indices;
}

// Intersection of two sorted vectors (similar to MATLAB's intersect).
std::vector<size_t> intersectIndices(const std::vector<size_t>& a, const std::vector<size_t>& b)
{
    std::vector<size_t> result;
    result.reserve(std::min(a.size(), b.size()));

    auto itA = a.begin();
    auto itB = b.begin();
    while (itA != a.end() && itB != b.end())
    {
        if (*itA < *itB) ++itA;
        else if (*itB < *itA) ++itB;
        else
        {
            // *itA == *itB
            result.push_back(*itA);
            ++itA;
            ++itB;
        }
    }
    return result;
}

// A simplified version of your matchIdxs() function: 
//   we want to pair "start" and "stop" such that stop - start is within [lowerBound, upperBound].
// We'll remove pairs that don't match.  (You can expand this logic to mirror your MATLAB approach.)
void matchIdxs(std::vector<size_t>& starts,
    std::vector<size_t>& stops,
    size_t lowerBound = 2500,
    size_t upperBound = 3500)
{
    if (starts.empty() || stops.empty()) return;

    // We'll produce new, filtered vectors
    std::vector<size_t> newStarts, newStops;
    newStarts.reserve(starts.size());
    newStops.reserve(stops.size());

    // For each start, find the first stop that is in range.
    size_t j = 0;  // index for stops
    for (size_t i = 0; i < starts.size(); ++i)
    {
        auto s = starts[i];

        // Advance j while stops[j] < s
        while (j < stops.size() && stops[j] < s)
            j++;

        if (j >= stops.size()) break;

        size_t diff = stops[j] - s;
        if (diff >= lowerBound && diff <= upperBound)
        {
            newStarts.push_back(s);
            newStops.push_back(stops[j]);
            j++; // consume this stop
        }
        // else not matched
    }

    // Update references
    starts = newStarts;
    stops = newStops;
}

// -----------------------------------------------------------------------------
// Main Program
// -----------------------------------------------------------------------------

int main()
{
    // -----------------------
    // 1) STREAM FROM FX3
    // -----------------------
    // Example streaming logic (similar to your code). 
    // Adjust buffer sizes and endpoints as needed.
    const int    PACKETS_PER_XFER = 512;
    const int    BYTES_PER_PACKET = 1024;
    const long   BUFFER_SIZE = PACKETS_PER_XFER * BYTES_PER_PACKET;
    const int    NUM_XFERS = 2;
    const long   KB_TO_TRANSFER = 100;         // smaller for demo
    const long   TOTAL_BYTES_TO_XFER = KB_TO_TRANSFER * 1024;

    // Create and open the USB device
    CCyUSBDevice* USBDevice = new CCyUSBDevice(NULL);
    if (!USBDevice->Open(0))
    {
        std::cerr << "Failed to open USB device." << std::endl;
        return -1;
    }

    // Select the Bulk IN endpoint (as in your code)
    CCyBulkEndPoint* bulkInEndpoint = USBDevice->BulkInEndPt;
    if (!bulkInEndpoint)
    {
        std::cerr << "No bulk IN endpoint found." << std::endl;
        USBDevice->Close();
        delete USBDevice;
        return -1;
    }

    // Configure the endpoint
    bulkInEndpoint->SetXferSize(BUFFER_SIZE);

    // Prepare buffers
    std::vector<unsigned char*> buffers(NUM_XFERS);
    std::vector<OVERLAPPED>     ovList(NUM_XFERS);
    std::vector<UCHAR*>         outContexts(NUM_XFERS, nullptr);

    for (int i = 0; i < NUM_XFERS; i++)
    {
        buffers[i] = new unsigned char[BUFFER_SIZE];
        memset(&ovList[i], 0, sizeof(OVERLAPPED));
        ovList[i].hEvent = CreateEvent(NULL, false, false, NULL);
        if (ovList[i].hEvent == NULL)
        {
            std::cerr << "Failed to create event for transfer " << i << std::endl;
            return -1;
        }
    }

    // Initially queue all transfers
    for (int i = 0; i < NUM_XFERS; i++)
    {
        LONG len = BUFFER_SIZE;
        outContexts[i] = bulkInEndpoint->BeginDataXfer(buffers[i], len, &ovList[i]);
        if (!outContexts[i])
        {
            std::cerr << "BeginDataXfer failed on transfer " << i << std::endl;
            return -1;
        }
    }

    // Main loop: collect the data in a std::vector
    std::vector<uint16_t> collectedData;
    // or std::vector<uint8_t> if your FX3 is truly 8-bit.
    // We'll assume 16-bit, as your MATLAB code does 'uint16' frequently.
    collectedData.reserve(TOTAL_BYTES_TO_XFER / 2); // each sample is 2 bytes if 16-bit

    long totalTransferred = 0;
    int  activeTransfers = NUM_XFERS;

    while (totalTransferred < TOTAL_BYTES_TO_XFER && activeTransfers > 0)
    {
        for (int i = 0; i < NUM_XFERS; i++)
        {
            // Wait with some long timeout
            if (bulkInEndpoint->WaitForXfer(&ovList[i], 5000))
            {
                LONG len = BUFFER_SIZE;
                if (bulkInEndpoint->FinishDataXfer(buffers[i], len, &ovList[i], outContexts[i]))
                {
                    if (len > 0)
                    {
                        // Copy data from buffers[i] into our vector.
                        // len is in bytes; each 16-bit sample is 2 bytes
                        int numSamples = len / 2;
                        const uint16_t* ptr16 = reinterpret_cast<uint16_t*>(buffers[i]);

                        // Append to collectedData
                        collectedData.insert(collectedData.end(), ptr16, ptr16 + numSamples);

                        totalTransferred += len;
                    }

                    // Re-queue if needed
                    if (totalTransferred < TOTAL_BYTES_TO_XFER)
                    {
                        LONG reLen = BUFFER_SIZE;
                        outContexts[i] = bulkInEndpoint->BeginDataXfer(buffers[i], reLen, &ovList[i]);
                        if (!outContexts[i])
                        {
                            std::cerr << "Re-queue BeginDataXfer failed on xfer " << i << std::endl;
                            activeTransfers--;
                        }
                    }
                    else
                    {
                        // We have all we need
                        activeTransfers--;
                    }
                }
                else
                {
                    std::cerr << "FinishDataXfer failed on xfer " << i << std::endl;
                    activeTransfers--;
                }
            }
            // else WaitForXfer timed out or failed—handle as needed

            if (totalTransferred >= TOTAL_BYTES_TO_XFER)
                break;
        }
    }

    std::cout << "FX3 Transfer complete. Total bytes transferred: "
        << totalTransferred << std::endl;

    // Cleanup the USB stuff
    for (int i = 0; i < NUM_XFERS; i++)
    {
        delete[] buffers[i];
        CloseHandle(ovList[i].hEvent);
    }
    USBDevice->Close();
    delete USBDevice;

    // -----------------------
    // 2) PARSE THE DATA (Mimic your MATLAB code)
    // -----------------------
    //   The key difference is that we already have "collectedData" in memory.
    //   Below, we replicate the bit-level analysis that your MATLAB script performs.

    if (collectedData.empty())
    {
        std::cerr << "No data was collected from the FX3. Exiting." << std::endl;
        return 0;
    }

    // Convert to bitstream (16 bits each, little-endian)
    std::vector<uint8_t> bitStream = lookAtBits_16(collectedData, /*littleEndian=*/true);

    // Define the "global" 24-bit prefix from your MATLAB code: 0xFF, 0x00, 0x00 (MSB-first).
    // In your MATLAB: 
    //   code1 = hexToBinaryVector('FF',8);
    //   code2 = hexToBinaryVector('00',8);
    //   code3 = hexToBinaryVector('00',8);
    //   patternStartValid   = [code1, code2, code3, getCode('sav')];
    // We will build these as a single pattern vector:
    std::vector<uint8_t> code1 = hexToBinaryVector("FF", 8);  // [1,1,1,1,1,1,1,1]
    std::vector<uint8_t> code2 = hexToBinaryVector("00", 8);  // [0,0,0,0,0,0,0,0]
    std::vector<uint8_t> code3 = hexToBinaryVector("00", 8);  // [0,0,0,0,0,0,0,0]

    // Build patterns
    auto sav = getCode("sav");   // hexToBinaryVector("80")
    auto savi = getCode("savi");  // hexToBinaryVector("AB")
    auto eav = getCode("eav");   // hexToBinaryVector("9D")
    auto eavi = getCode("eavi");  // hexToBinaryVector("B6")

    // Concatenate them, e.g. patternStartValid = [code1, code2, code3, sav]
    auto patternStartValid = code1;
    patternStartValid.insert(patternStartValid.end(), code2.begin(), code2.end());
    patternStartValid.insert(patternStartValid.end(), code3.begin(), code3.end());
    patternStartValid.insert(patternStartValid.end(), sav.begin(), sav.end());

    auto patternStartInvalid = code1;
    patternStartInvalid.insert(patternStartInvalid.end(), code2.begin(), code2.end());
    patternStartInvalid.insert(patternStartInvalid.end(), code3.begin(), code3.end());
    patternStartInvalid.insert(patternStartInvalid.end(), savi.begin(), savi.end());

    auto patternEndValid = code1;
    patternEndValid.insert(patternEndValid.end(), code2.begin(), code2.end());
    patternEndValid.insert(patternEndValid.end(), code3.begin(), code3.end());
    patternEndValid.insert(patternEndValid.end(), eav.begin(), eav.end());

    auto patternEndInvalid = code1;
    patternEndInvalid.insert(patternEndInvalid.end(), code2.begin(), code2.end());
    patternEndInvalid.insert(patternEndInvalid.end(), code3.begin(), code3.end());
    patternEndInvalid.insert(patternEndInvalid.end(), eavi.begin(), eavi.end());

    // For your MATLAB code, you do this pattern detection on "bits1" and "bits2".
    // That implies you’re de-interleaving or you have two separate channels.
    // For brevity, let’s assume we only have one channel in this example:
    // If you do have two channels, you’d split the bitstream or read from two separate buffers.
    // --- For demonstration, we do single-channel "bitStream" only:

    // 3) Find occurrences
    auto idxSav = findPattern(bitStream, patternStartValid);
    auto idxSavi = findPattern(bitStream, patternStartInvalid);
    auto idxEav = findPattern(bitStream, patternEndValid);
    auto idxEavi = findPattern(bitStream, patternEndInvalid);

    // Just print how many we found
    std::cout << "Start valid (sav) patterns found: " << idxSav.size() << std::endl;
    std::cout << "Start invalid (savi) patterns found: " << idxSavi.size() << std::endl;
    std::cout << "End valid (eav) patterns found: " << idxEav.size() << std::endl;
    std::cout << "End invalid (eavi) patterns found: " << idxEavi.size() << std::endl;

    // If you wanted to replicate the "matching" logic (to ensure that start/end patterns are in
    // pairs within a certain distance), do something like:
    matchIdxs(idxSav, idxEav);   // Filter them so that eav - sav is in [2500, 3500] range
    matchIdxs(idxSavi, idxEavi);  // Filter them so that eavi - savi is in [2500, 3500] range

    std::cout << "Filtered sav count:  " << idxSav.size() << std::endl;
    std::cout << "Filtered eav count:  " << idxEav.size() << std::endl;
    std::cout << "Filtered savi count: " << idxSavi.size() << std::endl;
    std::cout << "Filtered eavi count: " << idxEavi.size() << std::endl;

    // 4) Further steps: 
    //    - If you had 2 channels (bits1, bits2), you would do the above for each channel
    //      and then do intersection(...) like your MATLAB code, etc.
    //    - Then you’d extract the “payload” bits between start and end sync codes
    //      and reconstruct images or frames as needed.

    std::cout << "Done parsing bit stream." << std::endl;
    return 0;
}


// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
