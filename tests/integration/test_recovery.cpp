#define main daemon_main
#include "../../daemon/vdrsd_daemon.cpp"
#undef main

#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>
#include <filesystem>

void test_recovery_pipeline() {
    std::filesystem::remove_all("./node1_data");
    std::filesystem::remove_all("./node2_data");

    // 1. Initialize Node 1
    vdrsd::StorageEngine storage1("./node1_data");
    assert(storage1.init());

    vdrsd::NetworkManager net1(9001);
    net1.add_peer("node2", "127.0.0.1", 9002);
    net1.set_sync_callback([&net1, &storage1](const std::string& ip, uint16_t port, uint64_t peer_latest) {
        uint64_t my_latest = storage1.get_latest_chunk_id();
        if (my_latest > peer_latest) {
            net1.sync_peer(ip, port, peer_latest + 1, my_latest, storage1);
        }
    });
    assert(net1.init());

    // 2. Initialize Node 2
    vdrsd::StorageEngine storage2("./node2_data");
    assert(storage2.init());

    vdrsd::NetworkManager net2(9002);
    net2.add_peer("node1", "127.0.0.1", 9001);
    net2.set_data_callback([&storage2](const vdrsd::DataChunk& chunk) {
        storage2.write_chunk(chunk, false);
    });
    assert(net2.init());

    // 3. Write chunk 1 and chunk 2 to Node 1, replicating to Node 2
    vdrsd::DataChunk c1, c2;
    const char* txt1 = "Payload of chunk 1";
    const char* txt2 = "Payload of chunk 2";
    c1.set_data(1, reinterpret_cast<const uint8_t*>(txt1), std::strlen(txt1));
    c2.set_data(2, reinterpret_cast<const uint8_t*>(txt2), std::strlen(txt2));

    storage1.write_chunk(c1, false);
    assert(net1.replicate_chunk(c1));
    storage1.write_chunk(c2, false);
    assert(net1.replicate_chunk(c2));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify Node 2 has chunk 1 and chunk 2
    vdrsd::DataChunk check1, check2;
    assert(storage2.read_chunk(1, check1));
    assert(storage2.read_chunk(2, check2));
    assert(storage2.get_latest_chunk_id() == 2);

    // 4. Simulate Node 2 going offline (shutdown net2 and storage2)
    std::cout << "[Test] Simulating Node 2 going offline..." << std::endl;
    net2.shutdown();
    storage2.shutdown();

    // 5. Node 1 receives chunk 3 and chunk 4 while Node 2 is offline
    vdrsd::DataChunk c3, c4;
    const char* txt3 = "Payload of chunk 3 (written during Node 2 downtime)";
    const char* txt4 = "Payload of chunk 4 (written during Node 2 downtime)";
    c3.set_data(3, reinterpret_cast<const uint8_t*>(txt3), std::strlen(txt3));
    c4.set_data(4, reinterpret_cast<const uint8_t*>(txt4), std::strlen(txt4));

    storage1.write_chunk(c3, false);
    net1.replicate_chunk(c3); // replication to Node 2 fails / marks peer offline
    storage1.write_chunk(c4, false);
    net1.replicate_chunk(c4);

    assert(storage1.get_latest_chunk_id() == 4);

    // 6. Simulate Node 2 coming back online
    std::cout << "[Test] Simulating Node 2 coming back online and starting recovery..." << std::endl;
    vdrsd::StorageEngine storage2_recovered("./node2_data");
    assert(storage2_recovered.init());
    assert(storage2_recovered.get_latest_chunk_id() == 2); // only has chunk 1 and 2

    vdrsd::NetworkManager net2_recovered(9002);
    net2_recovered.add_peer("node1", "127.0.0.1", 9001);
    net2_recovered.set_data_callback([&storage2_recovered](const vdrsd::DataChunk& chunk) {
        storage2_recovered.write_chunk(chunk, false);
    });
    assert(net2_recovered.init());

    // Register ClusterSyncObserver on Node 2 and start HealthMonitor
    vdrsd::ClusterSyncObserver observer2(net2_recovered, storage2_recovered, 9002);
    vdrsd::HealthMonitor monitor2(net2_recovered, 500);
    monitor2.register_observer(&observer2);
    monitor2.start();

    // Allow background monitor thread to detect Node 1 and perform catch-up sync
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    // 7. Verify Node 2 received missed chunk 3 and chunk 4
    vdrsd::DataChunk check3, check4;
    assert(storage2_recovered.read_chunk(3, check3));
    assert(check3.verify_checksum());
    assert(std::memcmp(check3.payload, txt3, std::strlen(txt3)) == 0);

    assert(storage2_recovered.read_chunk(4, check4));
    assert(check4.verify_checksum());
    assert(std::memcmp(check4.payload, txt4, std::strlen(txt4)) == 0);

    assert(storage2_recovered.get_latest_chunk_id() == 4);

    monitor2.stop();
    net2_recovered.shutdown();
    storage2_recovered.shutdown();
    net1.shutdown();
    storage1.shutdown();

    std::filesystem::remove_all("./node1_data");
    std::filesystem::remove_all("./node2_data");

    std::cout << " [PASS] test_recovery_pipeline" << std::endl;
}

int main() {
    std::cout << "--- Integration Test: Replica Recovery & Catch-up Sync ---" << std::endl;
    test_recovery_pipeline();
    return 0;
}
