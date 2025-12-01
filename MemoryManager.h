#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include <cstdint>
#include <vector>
#include <deque>
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

#include "process.h"

class MemoryManager {
public:
    MemoryManager();
    ~MemoryManager();

    // Initialize memory manager before allocator use
    // total_mem and frame_size are in bytes
    void init(uint32_t total_mem, uint32_t frame_size);

    // Allocate metadata for a process (does NOT immediately allocate frames).
    // Returns true on success (valid sizes), false if rejected.
    bool allocate_process(const std::shared_ptr<ProcessStub>& p, uint32_t mem_bytes);

    // Free process pages/frames and backing store entries
    void free_process(const std::shared_ptr<ProcessStub>& p);

    // Demand paging: ensure that virtual address is backed by a loaded frame.
    // Returns true if page is valid/loaded. Returns false on access violation
    // (address outside process memory).
    bool ensure_page_loaded(const std::shared_ptr<ProcessStub>& p, uint32_t virtual_address);

    // Read/Write uint16 values at virtual addresses relative to process's memory base.
    // Returns true on success. On invalid address returns false.
    bool read_u16(const std::shared_ptr<ProcessStub>& p, uint32_t virtual_address, uint16_t &out);
    bool write_u16(const std::shared_ptr<ProcessStub>& p, uint32_t virtual_address, uint16_t value);

    // Stats
    uint32_t frame_count() const;
    uint32_t frame_size() const;

private:
    // internal helpers
    int find_free_frame_locked();
    void evict_frame_locked(int frame_index);
    std::string backing_key(const std::string &procname, int page_idx) const;
    void persist_backing_store_locked(); // writes backing store map to file

    mutable std::mutex mtx;

    uint32_t total_memory_bytes = 0;
    uint32_t frame_bytes = 0;
    uint32_t frames_count = 0;

    // For each frame: owner key "<procname>:<page>" or empty
    std::vector<std::string> frame_owner;

    // Simulated bytes stored per frame
    std::vector<std::vector<uint8_t>> frame_content;

    // Free frames list
    std::vector<int> free_frames;

    // FIFO replacement queue of frame indices
    std::deque<int> fifo_queue;

    // Backing store: map key -> raw bytes (text file persisted)
    std::unordered_map<std::string, std::vector<uint8_t>> backing_store;

    // helper counters (externs expected by vmstat)
    // (we will update extern counters from osemulator through functions when paging occurs)
};

extern std::unique_ptr<MemoryManager> mem_manager;

#endif