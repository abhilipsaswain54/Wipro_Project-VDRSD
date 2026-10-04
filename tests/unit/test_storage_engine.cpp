#define main daemon_main
#include "../../daemon/vdrsd_daemon.cpp"
#undef main

#include <iostream>
#include <cassert>
#include <cstring>

void test_chunk_persistence() {
    vdrsd::StorageEngine engine("./test_data");
    assert(engine.init());

    vdrsd::DataChunk write_chunk;
    const char* sample_text = "VDRSD Persistent Storage Test Data";
    write_chunk.set_data(42, reinterpret_cast<const uint8_t*>(sample_text), std::strlen(sample_text));

    assert(engine.write_chunk(write_chunk, false));

    vdrsd::DataChunk read_chunk;
    assert(engine.read_chunk(42, read_chunk));
    assert(read_chunk.chunk_id == 42);
    assert(read_chunk.verify_checksum());
    assert(std::memcmp(read_chunk.payload, sample_text, std::strlen(sample_text)) == 0);

    engine.shutdown();
    std::cout << " [PASS] test_chunk_persistence" << std::endl;
}

int main() {
    std::cout << "--- Testing StorageEngine ---" << std::endl;
    test_chunk_persistence();
    return 0;
}
