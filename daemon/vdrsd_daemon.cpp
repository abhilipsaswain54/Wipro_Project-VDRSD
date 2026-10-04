/*
 * vdrsd_daemon.cpp - Virtual Distributed Replicated Storage Device Daemon
 *
 * Beginner-Friendly C++ implementation with ZERO header (.hpp / .h) files.
 * Contains:
 *   1. Data Structures (DataChunk, PeerNode, ReplicationMessage)
 *   2. Reliable POSIX I/O Helpers (read_full, write_full)
 *   3. Lock-free Write Cache (RingBuffer)
 *   4. POSIX Storage Engine (StorageEngine) with directory scanning
 *   5. TCP Network Manager & Replication (NetworkManager) with Sync support
 *   6. Observer Health Monitor (HealthMonitor)
 *   7. Cluster Synchronization Observer (ClusterSyncObserver)
 *   8. Process Daemonization & Signal Handling (Daemon)
 *   9. Singleton Configuration Manager (ConfigManager)
 *   10. Kernel Device Driver Interface (KernelInterface)
 *   11. Main Entry Point
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <array>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace vdrsd {

// ─────────────────────────────────────────────────────────────
// 1. DATA STRUCTURES & CONSTANTS
// ─────────────────────────────────────────────────────────────
constexpr size_t CHUNK_PAYLOAD_SIZE = 4096;

inline uint32_t calculate_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            uint32_t mask = -(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
    }
    return ~crc;
}

struct DataChunk {
    uint64_t chunk_id;
    uint64_t timestamp;
    uint32_t size;
    uint32_t checksum;
    uint8_t  payload[CHUNK_PAYLOAD_SIZE];

    DataChunk() : chunk_id(0), timestamp(0), size(0), checksum(0) {
        std::memset(payload, 0, CHUNK_PAYLOAD_SIZE);
    }

    void set_data(uint64_t id, const uint8_t* src_data, uint32_t len) {
        chunk_id = id;
        size = (len > CHUNK_PAYLOAD_SIZE) ? CHUNK_PAYLOAD_SIZE : len;
        std::memcpy(payload, src_data, size);
        if (size < CHUNK_PAYLOAD_SIZE) {
            std::memset(payload + size, 0, CHUNK_PAYLOAD_SIZE - size);
        }
        checksum = calculate_crc32(payload, size);
        timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool verify_checksum() const {
        return calculate_crc32(payload, size) == checksum;
    }
};

enum class NodeStatus { ONLINE, OFFLINE, SYNCING };

struct PeerNode {
    std::string node_id;
    std::string ip_address;
    uint16_t port;
    NodeStatus status;
    std::chrono::steady_clock::time_point last_heartbeat;

    PeerNode() : port(0), status(NodeStatus::OFFLINE), last_heartbeat(std::chrono::steady_clock::now()) {}
    PeerNode(const std::string& id, const std::string& ip, uint16_t p)
        : node_id(id), ip_address(ip), port(p), status(NodeStatus::OFFLINE), last_heartbeat(std::chrono::steady_clock::now()) {}
};

enum class MessageType : uint8_t {
    DATA_WRITE   = 1,
    DATA_READ    = 2,
    HEARTBEAT    = 3,
    ACK          = 4,
    NACK         = 5,
    SYNC_REQUEST = 6
};

struct ReplicationMessage {
    MessageType type;
    uint64_t    sequence_num;
    DataChunk   chunk;

    ReplicationMessage() : type(MessageType::HEARTBEAT), sequence_num(0) {}
};

// ─────────────────────────────────────────────────────────────
// 2. RELIABLE POSIX SOCKET I/O HELPERS
// ─────────────────────────────────────────────────────────────
// Guarantees all requested bytes are read even if TCP delivers in fragments
inline bool read_full(int sock, void* buf, size_t count) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t n = read(sock, ptr + total_read, count - total_read);
        if (n <= 0) return false;
        total_read += static_cast<size_t>(n);
    }
    return true;
}

// Guarantees all requested bytes are written even if buffer space is limited
inline bool write_full(int sock, const void* buf, size_t count) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t total_written = 0;
    while (total_written < count) {
        ssize_t n = write(sock, ptr + total_written, count - total_written);
        if (n <= 0) return false;
        total_written += static_cast<size_t>(n);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 3. THREAD-SAFE RING BUFFER (WRITE CACHE)
// ─────────────────────────────────────────────────────────────
template<typename T, size_t Capacity = 512>
class RingBuffer {
private:
    std::vector<T> buffer_;
    size_t head_{0};
    size_t tail_{0};
    size_t size_{0};
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;

public:
    RingBuffer() : buffer_(Capacity) {}

    bool push(const T& item, bool block = true) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (block) {
            not_full_.wait(lock, [this] { return size_ < Capacity; });
        } else if (size_ >= Capacity) {
            return false;
        }

        buffer_[tail_] = item;
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        not_empty_.notify_one();
        return true;
    }

    bool pop(T& item, bool block = true) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (block) {
            not_empty_.wait(lock, [this] { return size_ > 0; });
        } else if (size_ == 0) {
            return false;
        }

        item = buffer_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        not_full_.notify_one();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
    }
};

// ─────────────────────────────────────────────────────────────
// 4. POSIX STORAGE ENGINE
// ─────────────────────────────────────────────────────────────
class StorageEngine {
private:
    std::string base_dir_;
    RingBuffer<DataChunk, 512> write_cache_;
    std::thread flush_thread_;
    std::atomic<bool> running_{false};

    std::string get_chunk_path(uint64_t chunk_id) const {
        std::ostringstream ss;
        ss << base_dir_ << "/chunk_" << std::setw(8) << std::setfill('0') << chunk_id << ".dat";
        return ss.str();
    }

    void flush_loop() {
        while (running_) {
            DataChunk chunk;
            if (write_cache_.pop(chunk, false)) {
                flush_to_disk(chunk);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

public:
    explicit StorageEngine(const std::string& base_dir = "./vdrsd_data")
        : base_dir_(base_dir), running_(false) {}

    ~StorageEngine() { shutdown(); }

    bool init() {
        struct stat st;
        if (stat(base_dir_.c_str(), &st) != 0) {
            if (mkdir(base_dir_.c_str(), 0755) != 0) {
                std::cerr << "[StorageEngine] Failed to create storage dir: " << base_dir_ << std::endl;
                return false;
            }
        }
        running_ = true;
        flush_thread_ = std::thread(&StorageEngine::flush_loop, this);
        std::cout << "[StorageEngine] Initialized at path: " << base_dir_ << std::endl;
        return true;
    }

    void shutdown() {
        if (running_) {
            running_ = false;
            if (flush_thread_.joinable()) flush_thread_.join();

            DataChunk chunk;
            while (write_cache_.pop(chunk, false)) {
                flush_to_disk(chunk);
            }
            std::cout << "[StorageEngine] Shutdown complete." << std::endl;
        }
    }

    bool write_chunk(const DataChunk& chunk, bool async = true) {
        return async ? write_cache_.push(chunk, true) : flush_to_disk(chunk);
    }

    bool flush_to_disk(const DataChunk& chunk) {
        std::string path = get_chunk_path(chunk.chunk_id);
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return false;

        ssize_t written = write(fd, &chunk, sizeof(DataChunk));
        fsync(fd);
        close(fd);
        return written == sizeof(DataChunk);
    }

    bool read_chunk(uint64_t chunk_id, DataChunk& chunk) const {
        std::string path = get_chunk_path(chunk_id);
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;

        ssize_t bytes_read = read(fd, &chunk, sizeof(DataChunk));
        close(fd);
        return (bytes_read == sizeof(DataChunk)) && chunk.verify_checksum();
    }

    // Scans local directory and finds the highest stored chunk ID
    uint64_t get_latest_chunk_id() const {
        uint64_t max_id = 0;
        try {
            if (std::filesystem::exists(base_dir_)) {
                for (const auto& entry : std::filesystem::directory_iterator(base_dir_)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        // Chunks follow the naming pattern: chunk_XXXXXXXX.dat
                        if (filename.rfind("chunk_", 0) == 0) {
                            size_t dot_pos = filename.find(".dat");
                            if (dot_pos != std::string::npos && dot_pos > 6) {
                                std::string num_part = filename.substr(6, dot_pos - 6);
                                try {
                                    uint64_t id = std::stoull(num_part);
                                    if (id > max_id) {
                                        max_id = id;
                                    }
                                } catch (...) {}
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[StorageEngine] Directory scan error: " << e.what() << std::endl;
        }
        return max_id;
    }
};

// ─────────────────────────────────────────────────────────────
// 5. TCP NETWORK MANAGER & REPLICATION
// ─────────────────────────────────────────────────────────────
using DataReceivedCallback = std::function<void(const DataChunk&)>;
using SyncRequestedCallback = std::function<void(const std::string& peer_ip, uint16_t peer_port, uint64_t peer_latest_chunk_id)>;

class NetworkManager {
private:
    uint16_t listen_port_;
    int server_fd_{-1};
    std::vector<PeerNode> peers_;
    std::mutex peers_mutex_;
    std::thread listen_thread_;
    std::atomic<bool> running_{false};
    DataReceivedCallback on_data_received_;
    SyncRequestedCallback on_sync_requested_;

    void accept_loop() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &addr_len);
            if (client_fd >= 0) {
                char ip_buf[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
                std::string client_ip(ip_buf);
                std::thread(&NetworkManager::handle_client, this, client_fd, client_ip).detach();
            } else if (!running_) {
                break;
            }
        }
    }

    void handle_client(int client_fd, std::string client_ip) {
        ReplicationMessage msg;
        if (read_full(client_fd, &msg, sizeof(ReplicationMessage))) {
            if (msg.type == MessageType::DATA_WRITE) {
                if (msg.chunk.verify_checksum() && on_data_received_) {
                    on_data_received_(msg.chunk);
                }
            } else if (msg.type == MessageType::SYNC_REQUEST) {
                uint64_t peer_latest_id = msg.sequence_num;
                uint16_t peer_port = static_cast<uint16_t>(msg.chunk.size);

                // If port was not embedded, match by IP from configured peers
                if (peer_port == 0) {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    for (const auto& p : peers_) {
                        if (p.ip_address == client_ip || client_ip == "127.0.0.1") {
                            peer_port = p.port;
                            break;
                        }
                    }
                }

                if (on_sync_requested_ && peer_port > 0) {
                    on_sync_requested_(client_ip, peer_port, peer_latest_id);
                }
            }
        }
        close(client_fd);
    }

public:
    explicit NetworkManager(uint16_t listen_port = 9001)
        : listen_port_(listen_port), running_(false) {}

    ~NetworkManager() { shutdown(); }

    bool init() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return false;

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(listen_port_);

        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(server_fd_);
            return false;
        }

        if (listen(server_fd_, 10) < 0) {
            close(server_fd_);
            return false;
        }

        running_ = true;
        listen_thread_ = std::thread(&NetworkManager::accept_loop, this);
        std::cout << "[NetworkManager] TCP Server listening on port " << listen_port_ << std::endl;
        return true;
    }

    void shutdown() {
        if (running_) {
            running_ = false;
            if (server_fd_ >= 0) {
                ::shutdown(server_fd_, SHUT_RDWR);
                close(server_fd_);
                server_fd_ = -1;
            }
            if (listen_thread_.joinable()) listen_thread_.join();
            std::cout << "[NetworkManager] Shutdown complete." << std::endl;
        }
    }

    void add_peer(const std::string& id, const std::string& ip, uint16_t port) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers_.emplace_back(id, ip, port);
        std::cout << "[NetworkManager] Added peer: " << id << " (" << ip << ":" << port << ")" << std::endl;
    }

    void set_peer_status(const std::string& node_id, NodeStatus status) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& peer : peers_) {
            if (peer.node_id == node_id) {
                peer.status = status;
                if (status == NodeStatus::ONLINE) {
                    peer.last_heartbeat = std::chrono::steady_clock::now();
                }
                break;
            }
        }
    }

    bool send_message(const std::string& ip, uint16_t port, const ReplicationMessage& msg) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;

        sockaddr_in peer_addr{};
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &peer_addr.sin_addr);

        timeval tv{1, 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) {
            close(sock);
            return false;
        }

        bool ok = write_full(sock, &msg, sizeof(ReplicationMessage));
        close(sock);
        return ok;
    }

    bool replicate_chunk(const DataChunk& chunk) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        ReplicationMessage msg;
        msg.type = MessageType::DATA_WRITE;
        msg.sequence_num = chunk.chunk_id;
        msg.chunk = chunk;

        bool success_any = false;
        for (auto& peer : peers_) {
            if (send_message(peer.ip_address, peer.port, msg)) {
                peer.status = NodeStatus::ONLINE;
                peer.last_heartbeat = std::chrono::steady_clock::now();
                success_any = true;
            } else {
                peer.status = NodeStatus::OFFLINE;
            }
        }
        return success_any || peers_.empty();
    }

    // Sends missing chunks sequentially to a peer that requested catch-up synchronization
    void sync_peer(const std::string& ip, uint16_t port, uint64_t from_chunk_id, uint64_t to_chunk_id, StorageEngine& storage) {
        std::cout << "[NetworkManager] Synchronizing chunks #" << from_chunk_id << " through #" << to_chunk_id 
                  << " with peer " << ip << ":" << port << "..." << std::endl;
        for (uint64_t id = from_chunk_id; id <= to_chunk_id; ++id) {
            DataChunk chunk;
            if (storage.read_chunk(id, chunk)) {
                ReplicationMessage msg;
                msg.type = MessageType::DATA_WRITE;
                msg.sequence_num = chunk.chunk_id;
                msg.chunk = chunk;
                if (send_message(ip, port, msg)) {
                    std::cout << "[NetworkManager] Catch-up sync: Sent chunk #" << id << " to " << ip << ":" << port << std::endl;
                } else {
                    std::cerr << "[NetworkManager] Catch-up sync failed sending chunk #" << id << " to " << ip << ":" << port << std::endl;
                    break;
                }
            }
        }
    }

    void set_data_callback(DataReceivedCallback cb) { on_data_received_ = cb; }
    void set_sync_callback(SyncRequestedCallback cb) { on_sync_requested_ = cb; }
    const std::vector<PeerNode>& get_peers() const { return peers_; }
};

// ─────────────────────────────────────────────────────────────
// 6. OBSERVER PATTERN HEALTH MONITOR
// ─────────────────────────────────────────────────────────────
class INodeObserver {
public:
    virtual ~INodeObserver() = default;
    virtual void on_node_status_change(const PeerNode& node, NodeStatus new_status) = 0;
};

class HealthMonitor {
private:
    NetworkManager& network_mgr_;
    std::vector<INodeObserver*> observers_;
    std::mutex observers_mutex_;
    std::thread monitor_thread_;
    std::atomic<bool> running_{false};
    int interval_ms_;

    void monitor_loop() {
        while (running_) {
            ReplicationMessage hb_msg;
            hb_msg.type = MessageType::HEARTBEAT;

            auto peers = network_mgr_.get_peers();
            for (const auto& peer : peers) {
                bool alive = network_mgr_.send_message(peer.ip_address, peer.port, hb_msg);
                NodeStatus current_status = alive ? NodeStatus::ONLINE : NodeStatus::OFFLINE;
                if (current_status != peer.status) {
                    network_mgr_.set_peer_status(peer.node_id, current_status);
                    notify_observers(peer, current_status);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
        }
    }

public:
    explicit HealthMonitor(NetworkManager& net_mgr, int interval_ms = 3000)
        : network_mgr_(net_mgr), running_(false), interval_ms_(interval_ms) {}

    ~HealthMonitor() { stop(); }

    void register_observer(INodeObserver* obs) {
        std::lock_guard<std::mutex> lock(observers_mutex_);
        observers_.push_back(obs);
    }

    void notify_observers(const PeerNode& node, NodeStatus new_status) {
        std::lock_guard<std::mutex> lock(observers_mutex_);
        for (auto* obs : observers_) {
            obs->on_node_status_change(node, new_status);
        }
    }

    bool start() {
        running_ = true;
        monitor_thread_ = std::thread(&HealthMonitor::monitor_loop, this);
        std::cout << "[HealthMonitor] Node Health Monitoring started." << std::endl;
        return true;
    }

    void stop() {
        if (running_) {
            running_ = false;
            if (monitor_thread_.joinable()) monitor_thread_.join();
            std::cout << "[HealthMonitor] Monitoring stopped." << std::endl;
        }
    }
};

// ─────────────────────────────────────────────────────────────
// 7. CLUSTER SYNCHRONIZATION OBSERVER (RECOVERY & CATCH-UP)
// ─────────────────────────────────────────────────────────────
class ClusterSyncObserver : public INodeObserver {
private:
    NetworkManager& network_mgr_;
    StorageEngine& storage_engine_;
    uint16_t local_port_;

public:
    ClusterSyncObserver(NetworkManager& net_mgr, StorageEngine& storage, uint16_t local_port)
        : network_mgr_(net_mgr), storage_engine_(storage), local_port_(local_port) {}

    void on_node_status_change(const PeerNode& node, NodeStatus new_status) override {
        if (new_status == NodeStatus::ONLINE) {
            std::cout << "\n[Observer] >>> Peer node '" << node.node_id << "' (" 
                      << node.ip_address << ":" << node.port << ") is ONLINE! Initiating sync check... <<<" << std::endl;

            // Mark peer as SYNCING
            network_mgr_.set_peer_status(node.node_id, NodeStatus::SYNCING);

            // Determine our local latest chunk ID
            uint64_t latest_id = storage_engine_.get_latest_chunk_id();

            // Request synchronization from the peer
            ReplicationMessage sync_req;
            sync_req.type = MessageType::SYNC_REQUEST;
            sync_req.sequence_num = latest_id;
            sync_req.chunk.size = local_port_; // Pass our listening port to peer

            std::cout << "[Observer] Requesting catch-up sync from " << node.node_id 
                      << " (our local latest chunk ID: #" << latest_id << ")..." << std::endl;

            if (network_mgr_.send_message(node.ip_address, node.port, sync_req)) {
                std::cout << "[Observer] SYNC_REQUEST delivered to " << node.node_id << std::endl;
            } else {
                std::cerr << "[Observer] Failed to send SYNC_REQUEST to " << node.node_id << std::endl;
            }

            // Status restored to ONLINE
            network_mgr_.set_peer_status(node.node_id, NodeStatus::ONLINE);
        } else if (new_status == NodeStatus::OFFLINE) {
            std::cout << "\n[Observer] >>> Peer node '" << node.node_id << "' (" 
                      << node.ip_address << ":" << node.port << ") went OFFLINE. <<<" << std::endl;
            network_mgr_.set_peer_status(node.node_id, NodeStatus::OFFLINE);
        }
    }
};

// ─────────────────────────────────────────────────────────────
// 8. PROCESS DAEMONIZATION & SIGNALS
// ─────────────────────────────────────────────────────────────
class Daemon {
private:
    static std::atomic<bool> stop_requested_;

    static void signal_handler(int signal) {
        if (signal == SIGTERM || signal == SIGINT) {
            std::cout << "\n[Daemon] Received termination signal. Stopping..." << std::endl;
            stop_requested_ = true;
        }
    }

public:
    static void setup_signals() {
        struct sigaction sa;
        sa.sa_handler = Daemon::signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
    }

    static bool is_running() { return !stop_requested_; }
};

std::atomic<bool> Daemon::stop_requested_{false};

// ─────────────────────────────────────────────────────────────
// 9. SINGLETON CONFIG MANAGER
// ─────────────────────────────────────────────────────────────
struct PeerConfig {
    std::string id;
    std::string ip;
    uint16_t port;
};

class ConfigManager {
private:
    std::string node_id_;
    uint16_t listen_port_;
    std::string storage_path_;
    std::string device_path_;
    std::vector<PeerConfig> peers_;

    ConfigManager()
        : node_id_("node1"), listen_port_(9001),
          storage_path_("./vdrsd_data"), device_path_("/dev/vdrsd") {}

public:
    static ConfigManager& get_instance() {
        static ConfigManager instance;
        return instance;
    }

    bool load_config(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;

        peers_.clear();
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            std::istringstream iss(line);
            std::string key, value;
            if (std::getline(iss, key, '=') && std::getline(iss, value)) {
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);

                if (key == "node_id") node_id_ = value;
                else if (key == "listen_port") listen_port_ = static_cast<uint16_t>(std::stoi(value));
                else if (key == "storage_path") storage_path_ = value;
                else if (key == "device_path") device_path_ = value;
                else if (key == "peer") {
                    std::istringstream pss(value);
                    std::string pid, pip, pport;
                    if (std::getline(pss, pid, ':') && std::getline(pss, pip, ':') && std::getline(pss, pport)) {
                        PeerConfig peer;
                        peer.id = pid;
                        peer.ip = pip;
                        peer.port = static_cast<uint16_t>(std::stoi(pport));
                        peers_.push_back(peer);
                    }
                }
            }
        }
        std::cout << "[ConfigManager] Config loaded. Node ID: " << node_id_ 
                  << ", Port: " << listen_port_ << ", Peers: " << peers_.size() << std::endl;
        return true;
    }

    const std::string& get_node_id() const { return node_id_; }
    uint16_t get_listen_port() const { return listen_port_; }
    const std::string& get_storage_path() const { return storage_path_; }
    const std::string& get_device_path() const { return device_path_; }
    const std::vector<PeerConfig>& get_peers() const { return peers_; }
};

// ─────────────────────────────────────────────────────────────
// 10. KERNEL INTERFACE
// ─────────────────────────────────────────────────────────────
class KernelInterface {
private:
    std::string device_path_;
    int device_fd_{-1};
    StorageEngine& storage_engine_;
    NetworkManager& network_manager_;
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
    uint64_t next_chunk_id_{1};

    void read_loop() {
        uint8_t buffer[CHUNK_PAYLOAD_SIZE];
        while (running_) {
            ssize_t bytes_read = read(device_fd_, buffer, sizeof(buffer));
            if (bytes_read > 0) {
                DataChunk chunk;
                chunk.set_data(next_chunk_id_++, buffer, static_cast<uint32_t>(bytes_read));
                storage_engine_.write_chunk(chunk);
                network_manager_.replicate_chunk(chunk);
            } else if (bytes_read < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

public:
    KernelInterface(const std::string& dev_path, StorageEngine& storage, NetworkManager& net_mgr)
        : device_path_(dev_path), storage_engine_(storage), network_manager_(net_mgr), running_(false) {}

    ~KernelInterface() { shutdown(); }

    bool init() {
        // Continue indexing from the latest chunk persisted in local storage
        next_chunk_id_ = storage_engine_.get_latest_chunk_id() + 1;

        device_fd_ = open(device_path_.c_str(), O_RDONLY);
        if (device_fd_ < 0) {
            std::cerr << "[KernelInterface] Note: Character device " << device_path_
                      << " not accessible. Kernel driver bridge in fallback mode." << std::endl;
            return false;
        }
        running_ = true;
        reader_thread_ = std::thread(&KernelInterface::read_loop, this);
        std::cout << "[KernelInterface] Connected to kernel device: " << device_path_ 
                  << " (next chunk ID: #" << next_chunk_id_ << ")" << std::endl;
        return true;
    }

    void shutdown() {
        if (running_) {
            running_ = false;
            if (device_fd_ >= 0) {
                close(device_fd_);
                device_fd_ = -1;
            }
            if (reader_thread_.joinable()) reader_thread_.join();
            std::cout << "[KernelInterface] Bridge closed." << std::endl;
        }
    }
};

} // namespace vdrsd

// ─────────────────────────────────────────────────────────────
// 11. MAIN ENTRY POINT
// ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::cout << "=====================================================" << std::endl;
    std::cout << "  VDRSD - Virtual Distributed Replicated Storage     " << std::endl;
    std::cout << "=====================================================" << std::endl;

    std::string config_path = (argc > 1) ? argv[1] : "./config/vdrsd.conf";
    vdrsd::ConfigManager& config = vdrsd::ConfigManager::get_instance();
    config.load_config(config_path);

    vdrsd::Daemon::setup_signals();

    vdrsd::StorageEngine storage(config.get_storage_path());
    if (!storage.init()) return 1;

    vdrsd::NetworkManager net_mgr(config.get_listen_port());
    for (const auto& peer : config.get_peers()) {
        net_mgr.add_peer(peer.id, peer.ip, peer.port);
    }

    // Set callback for receiving replicated DATA_WRITE chunks
    net_mgr.set_data_callback([&storage](const vdrsd::DataChunk& chunk) {
        std::cout << "[Daemon] Received replicated chunk #" << chunk.chunk_id 
                  << " (" << chunk.size << " bytes). Saving to local disk..." << std::endl;
        storage.write_chunk(chunk);
    });

    // Set callback for responding to peer SYNC_REQUEST
    net_mgr.set_sync_callback([&net_mgr, &storage](const std::string& peer_ip, uint16_t peer_port, uint64_t peer_latest_chunk_id) {
        uint64_t my_latest = storage.get_latest_chunk_id();
        std::cout << "[SyncHandler] Peer (" << peer_ip << ":" << peer_port 
                  << ") requested sync. Peer has chunk #" << peer_latest_chunk_id 
                  << ", local node has chunk #" << my_latest << std::endl;

        if (my_latest > peer_latest_chunk_id) {
            net_mgr.sync_peer(peer_ip, peer_port, peer_latest_chunk_id + 1, my_latest, storage);
        } else {
            std::cout << "[SyncHandler] Peer is already up-to-date with local storage." << std::endl;
        }
    });

    if (!net_mgr.init()) return 1;

    // Register ClusterSyncObserver to handle peer recovery & catch-up sync
    vdrsd::ClusterSyncObserver sync_observer(net_mgr, storage, config.get_listen_port());

    vdrsd::HealthMonitor monitor(net_mgr, 3000);
    monitor.register_observer(&sync_observer);
    monitor.start();

    vdrsd::KernelInterface kernel_bridge(config.get_device_path(), storage, net_mgr);
    kernel_bridge.init();

    std::cout << "\n[VDRSD Daemon] Service successfully started. Press Ctrl+C to terminate." << std::endl;

    while (vdrsd::Daemon::is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n[VDRSD Daemon] Shutting down services..." << std::endl;
    kernel_bridge.shutdown();
    monitor.stop();
    net_mgr.shutdown();
    storage.shutdown();

    std::cout << "[VDRSD Daemon] Shutdown complete." << std::endl;
    return 0;
}
