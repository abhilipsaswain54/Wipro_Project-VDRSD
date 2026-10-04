#define main daemon_main
#include "../../daemon/vdrsd_daemon.cpp"
#undef main

#include <iostream>
#include <cassert>
#include <thread>

void test_basic_ops() {
    vdrsd::RingBuffer<int, 5> rb;
    assert(rb.empty());
    assert(rb.size() == 0);

    rb.push(10);
    rb.push(20);
    assert(!rb.empty());
    assert(rb.size() == 2);

    int val = 0;
    rb.pop(val);
    assert(val == 10);
    rb.pop(val);
    assert(val == 20);
    assert(rb.empty());
    std::cout << " [PASS] test_basic_ops" << std::endl;
}

void test_multithreaded() {
    vdrsd::RingBuffer<int, 100> rb;
    constexpr int total_items = 1000;

    std::thread producer([&rb]() {
        for (int i = 0; i < total_items; ++i) {
            rb.push(i, true);
        }
    });

    std::thread consumer([&rb]() {
        for (int i = 0; i < total_items; ++i) {
            int val = -1;
            rb.pop(val, true);
            assert(val == i);
        }
    });

    producer.join();
    consumer.join();
    assert(rb.empty());
    std::cout << " [PASS] test_multithreaded" << std::endl;
}

int main() {
    std::cout << "--- Testing RingBuffer ---" << std::endl;
    test_basic_ops();
    test_multithreaded();
    return 0;
}
