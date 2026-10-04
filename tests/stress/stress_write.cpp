#define main daemon_main
#include "../../daemon/vdrsd_daemon.cpp"
#undef main

#include <iostream>
#include <chrono>

int main(int argc, char* argv[]) {
    int total_chunks = (argc > 1) ? std::stoi(argv[1]) : 1000;
    std::cout << "--- Stress Test: Writing " << total_chunks << " chunks ---" << std::endl;

    vdrsd::StorageEngine engine("./stress_data");
    if (!engine.init()) {
        std::cerr << "Engine init failed." << std::endl;
        return 1;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 1; i <= total_chunks; ++i) {
        vdrsd::DataChunk chunk;
        std::string payload = "Stress Payload Chunk #" + std::to_string(i);
        chunk.set_data(i, reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
        engine.write_chunk(chunk, true);
    }

    engine.shutdown();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "Successfully wrote and flushed " << total_chunks << " chunks in " << duration_ms << " ms." << std::endl;
    std::cout << "Throughput: " << (total_chunks * 1000.0 / duration_ms) << " chunks/sec" << std::endl;
    return 0;
}
