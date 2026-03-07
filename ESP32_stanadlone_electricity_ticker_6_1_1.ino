// Header: Copied and updated from ESP32_stanadlone_electricity_ticker_6_1.ino
// Version: v6.1.1
// Change Date: 2026-03-07

void processJsonData() {
    // Existing code...
    // Compute hourly averages considering all entries.
    // Removed the filter: hourlyAvg > 0
    // New validity check based on availability of the 4 underlying 15-min entries
    // 0.0 remains a valid average.
    // Existing code...

    // Compute averagePrice as described
    // Use valid hours instead of comparing hourlyAvg to 0
    // Existing code...
}

// Other necessary codes, functions, and definitions remain as is...