#define main daemon_main
#include "../../daemon/vdrsd_daemon.cpp"
#undef main

#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>

void test_network_transmission() {
    vdrsd::NetworkManager receiver(9876);
    bool received = false;
    uint64_t rec_id = 0;

    receiver.set_data_callback([&received, &rec_id](const vdrsd::DataChunk& chunk) {
        received = true;
        rec_id = chunk.chunk_id;
    });
    assert(receiver.init());

    vdrsd::NetworkManager sender(9877);
    sender.add_peer("node2", "127.0.0.1", 9876);

    vdrsd::DataChunk send_chunk;
    const char* payload = "Hello Network Peer!";
    send_chunk.set_data(101, reinterpret_cast<const uint8_t*>(payload), std::strlen(payload));

    assert(sender.replicate_chunk(send_chunk));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    assert(received);
    assert(rec_id == 101);

    sender.shutdown();
    receiver.shutdown();
    std::cout << " [PASS] test_network_transmission" << std::endl;
}

int main() {
    std::cout << "--- Testing NetworkManager ---" << std::endl;
    test_network_transmission();
    return 0;
}
