#define main daemon_main
#include "../../daemon/vdrsd_daemon.cpp"
#undef main

#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>

void test_full_replication_pipeline() {
    vdrsd::StorageEngine storage2("./node2_data");
    assert(storage2.init());

    vdrsd::NetworkManager net2(9002);
    net2.set_data_callback([&storage2](const vdrsd::DataChunk& chunk) {
        storage2.write_chunk(chunk, false);
    });
    assert(net2.init());

    vdrsd::StorageEngine storage1("./node1_data");
    assert(storage1.init());

    vdrsd::NetworkManager net1(9001);
    net1.add_peer("node2", "127.0.0.1", 9002);
    assert(net1.init());

    vdrsd::DataChunk chunk;
    const char* text = "Pipeline Replication Data Verification";
    chunk.set_data(500, reinterpret_cast<const uint8_t*>(text), std::strlen(text));

    storage1.write_chunk(chunk, false);
    bool rep_ok = net1.replicate_chunk(chunk);
    assert(rep_ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    vdrsd::DataChunk replica_chunk;
    bool read_ok = storage2.read_chunk(500, replica_chunk);
    assert(read_ok);
    assert(replica_chunk.verify_checksum());
    assert(std::memcmp(replica_chunk.payload, text, std::strlen(text)) == 0);

    net1.shutdown();
    storage1.shutdown();
    net2.shutdown();
    storage2.shutdown();

    std::cout << " [PASS] test_full_replication_pipeline" << std::endl;
}

int main() {
    std::cout << "--- Integration Test: Multi-Node Replication ---" << std::endl;
    test_full_replication_pipeline();
    return 0;
}
