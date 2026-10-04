# VDRSD Root Makefile (No .hpp / .h header files required)

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

DAEMON_SRC = daemon/vdrsd_daemon.cpp

TEST_RING = tests/unit/test_ring_buffer
TEST_STORAGE = tests/unit/test_storage_engine
TEST_NET = tests/unit/test_network
TEST_REPL = tests/integration/test_replication
TEST_RECOV = tests/integration/test_recovery
STRESS_WRITE = tests/stress/stress_write

TARGET = vdrsd_daemon

all: $(TARGET) tests_all kernel_module

$(TARGET): $(DAEMON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

tests_all: $(TEST_RING) $(TEST_STORAGE) $(TEST_NET) $(TEST_REPL) $(TEST_RECOV) $(STRESS_WRITE)

$(TEST_RING): tests/unit/test_ring_buffer.cpp $(DAEMON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(TEST_STORAGE): tests/unit/test_storage_engine.cpp $(DAEMON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(TEST_NET): tests/unit/test_network.cpp $(DAEMON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(TEST_REPL): tests/integration/test_replication.cpp $(DAEMON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(TEST_RECOV): tests/integration/test_recovery.cpp $(DAEMON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(STRESS_WRITE): tests/stress/stress_write.cpp $(DAEMON_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

run_tests: tests_all
	@echo "=========================================="
	@echo " Running VDRSD Automated Test Suite "
	@echo "=========================================="
	@./$(TEST_RING)
	@./$(TEST_STORAGE)
	@./$(TEST_NET)
	@./$(TEST_REPL)
	@./$(TEST_RECOV)
	@echo "=========================================="
	@echo " ALL TESTS PASSED SUCCESSFULLY! "
	@echo "=========================================="

kernel_module:
	$(MAKE) -C kernel

clean:
	rm -f $(TARGET) $(TEST_RING) $(TEST_STORAGE) $(TEST_NET) $(TEST_REPL) $(TEST_RECOV) $(STRESS_WRITE)
	rm -rf vdrsd_data test_data node*_data stress_data
	$(MAKE) -C kernel clean || true

.PHONY: all tests_all run_tests kernel_module clean
