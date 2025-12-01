#include "MemoryManager.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <iostream>
#include <atomic>

// reference the VMSTAT atomic counters defined in osemulator.cpp
extern std::atomic<uint64_t> num_paged_in;
extern std::atomic<uint64_t> num_paged_out;

// Backing store filename
static const char *BACKING_STORE_FILE = "csopesy-backing-store.txt";

std::unique_ptr<MemoryManager> mem_manager = nullptr;

MemoryManager::MemoryManager() {}

MemoryManager::~MemoryManager() {
    std::lock_guard<std::mutex> lk(mtx);
    persist_backing_store_locked();
}

void MemoryManager::init(uint32_t total_mem, uint32_t frame_size) {
    std::lock_guard<std::mutex> lk(mtx);
    total_memory_bytes = total_mem;
    frame_bytes = frame_size;
    frames_count = (frame_bytes == 0) ? 0 : (total_memory_bytes / frame_bytes);
    frame_owner.assign(frames_count, std::string());
    frame_content.assign(frames_count, std::vector<uint8_t>(frame_bytes, 0));
    free_frames.clear();
    for (uint32_t i = 0; i < frames_count; ++i) free_frames.push_back((int)i);
    fifo_queue.clear();
    backing_store.clear();

    // If backing file exists, try to load minimal contents (best-effort).
    std::ifstream ifs(BACKING_STORE_FILE);
    if (!ifs) return;
    std::string line;
    while (std::getline(ifs, line)) {
        // Format: key <hex-bytes>
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key)) continue;
        std::string hex;
        if (!(ss >> hex)) continue;
        // hex is continuous hex; decode into bytes
        std::vector<uint8_t> bytes;
        bytes.reserve(hex.size()/2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            unsigned int b;
            std::stringstream hs;
            hs << std::hex << hex.substr(i,2);
            hs >> b;
            bytes.push_back(static_cast<uint8_t>(b));
        }
        backing_store[key] = std::move(bytes);
    }
}

static inline std::string backing_hex_from_bytes(const std::vector<uint8_t> &v) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : v) oss << std::setw(2) << (int)b;
    return oss.str();
}

void MemoryManager::persist_backing_store_locked() {
    std::ofstream ofs(BACKING_STORE_FILE, std::ofstream::trunc);
    if (!ofs) return;
    for (auto &kv : backing_store) {
        ofs << kv.first << " " << backing_hex_from_bytes(kv.second) << "\n";
    }
    ofs.close();
}

std::string MemoryManager::backing_key(const std::string &procname, int page_idx) const {
    std::ostringstream ss;
    ss << procname << ":" << page_idx;
    return ss.str();
}

int MemoryManager::find_free_frame_locked() {
    if (!free_frames.empty()) {
        int f = free_frames.back();
        free_frames.pop_back();
        return f;
    }
    return -1;
}

void MemoryManager::evict_frame_locked(int frame_index) {
    if (frame_index < 0 || (size_t)frame_index >= frame_owner.size()) return;
    const std::string owner = frame_owner[frame_index];
    if (owner.empty()) return;

    // owner format proc:page
    auto pos = owner.find(':');
    if (pos == std::string::npos) {
        frame_owner[frame_index].clear();
        std::fill(frame_content[frame_index].begin(), frame_content[frame_index].end(), 0);
        return;
    }
    std::string procname = owner.substr(0,pos);
    int pageidx = std::stoi(owner.substr(pos+1));

    // save frame bytes to backing_store
    backing_store[backing_key(procname, pageidx)] = frame_content[frame_index];

    // increment paged-out counter
    num_paged_out++;

    // set owner->page_table entry invalid (if process still exists)
    {
        std::lock_guard<std::mutex> rlk(repository_mutex);
        auto it = processes.find(procname);
        if (it != processes.end()) {
            auto p = it->second;
            std::lock_guard<std::mutex> plk(p->mtx);
            if (pageidx >= 0 && pageidx < (int)p->page_table.size())
                p->page_table[pageidx] = -1;
        }
    }

    frame_owner[frame_index].clear();
    std::fill(frame_content[frame_index].begin(), frame_content[frame_index].end(), 0);

    // Remove frame from FIFO queue (if present)
    auto it = std::find(fifo_queue.begin(), fifo_queue.end(), frame_index);
    if (it != fifo_queue.end()) fifo_queue.erase(it);
}

bool MemoryManager::allocate_process(const std::shared_ptr<ProcessStub>& p, uint32_t mem_bytes) {
    if (!p) return false;
    std::lock_guard<std::mutex> lk(mtx);
    if (frame_bytes == 0) return false;
    if (mem_bytes == 0) return false;
    if (mem_bytes % frame_bytes != 0) return false; // must be multiple of frames (since mem per proc is power-of-two, config ensures this)
    int pages = static_cast<int>(mem_bytes / frame_bytes);
    if (pages <= 0) return false;

    {
        std::lock_guard<std::mutex> plk(p->mtx);
        p->page_table.assign(pages, -1);
        p->num_pages = pages;
    }

    // Create zeroed backing entries for each page (empty / uninitialized)
    std::vector<uint8_t> zeros(frame_bytes, 0);
    for (int i = 0; i < pages; ++i) {
        backing_store[backing_key(p->name, i)] = zeros;
    }

    persist_backing_store_locked();
    return true;
}

void MemoryManager::free_process(const std::shared_ptr<ProcessStub>& p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(mtx);

    // Free frames owned by this process
    for (uint32_t fi = 0; fi < frame_owner.size(); ++fi) {
        const std::string owner = frame_owner[fi];
        if (owner.empty()) continue;
        auto pos = owner.find(':');
        if (pos == std::string::npos) continue;
        std::string procname = owner.substr(0,pos);
        if (procname == p->name) {
            // write frame_content to backing store and free
            int pageidx = std::stoi(owner.substr(pos+1));
            backing_store[backing_key(procname, pageidx)] = frame_content[fi];
            frame_owner[fi].clear();
            std::fill(frame_content[fi].begin(), frame_content[fi].end(), 0);
            // add to free list
            free_frames.push_back((int)fi);
            // remove from fifo if present
            auto it = std::find(fifo_queue.begin(), fifo_queue.end(), (int)fi);
            if (it != fifo_queue.end()) fifo_queue.erase(it);
        }
    }

    // Remove backing store entries for this process
    for (int i = 0; i < p->num_pages; ++i) {
        backing_store.erase(backing_key(p->name, i));
    }

    persist_backing_store_locked();
}

bool MemoryManager::ensure_page_loaded(const std::shared_ptr<ProcessStub>& p, uint32_t virtual_address) {
    if (!p) return false;
    std::lock_guard<std::mutex> lk(mtx);

    if (frame_bytes == 0) return false;

    uint32_t page_idx = virtual_address / frame_bytes;
    if ((int)page_idx >= p->num_pages) {
        // access violation
        return false;
    }

    if (p->page_table[page_idx] != -1) return true; // already loaded

    // Need to load page -> page fault
    int frame = find_free_frame_locked();
    if (frame == -1) {
        // evict via FIFO
        if (fifo_queue.empty()) {
            // no frames available (shouldn't happen), treat as failure
            return false;
        }
        frame = fifo_queue.front();
        fifo_queue.pop_front();
        evict_frame_locked(frame);
    }

    // load backing bytes into frame_content
    std::string key = backing_key(p->name, (int)page_idx);
    auto it = backing_store.find(key);
    if (it != backing_store.end()) {
        const std::vector<uint8_t> &bytes = it->second;
        // copy at most frame_bytes
        size_t copylen = std::min(bytes.size(), frame_content[frame].size());
        std::copy(bytes.begin(), bytes.begin() + copylen, frame_content[frame].begin());
        if (copylen < frame_content[frame].size())
            std::fill(frame_content[frame].begin() + copylen, frame_content[frame].end(), 0);
    } else {
        // initialize zeros
        std::fill(frame_content[frame].begin(), frame_content[frame].end(), 0);
    }

    // set owner
    frame_owner[frame] = backing_key(p->name, (int)page_idx);
    fifo_queue.push_back(frame);

    // update p->page_table
    {
        std::lock_guard<std::mutex> plk(p->mtx);
        p->page_table[page_idx] = frame;
    }

    // increment paged-in counter (external atomic)
    num_paged_in++;

    persist_backing_store_locked(); // keep backing store consistent (optional)
    return true;
}

bool MemoryManager::read_u16(const std::shared_ptr<ProcessStub>& p, uint32_t virtual_address, uint16_t &out) {
    if (!p) return false;
    std::unique_lock<std::mutex> lk(mtx);
    if (frame_bytes == 0) return false;

    uint32_t page_idx = virtual_address / frame_bytes;
    uint32_t offset = virtual_address % frame_bytes;
    if ((int)page_idx >= p->num_pages) return false;
    if (offset + 2 > frame_bytes) return false; // cannot cross page boundary in this simplified model

    int frame = p->page_table[page_idx];
    if (frame == -1) {
        // Need to load it: release lock, call ensure_page_loaded (will re-lock internally), then relock
        lk.unlock();
        if (!ensure_page_loaded(p, virtual_address)) return false;
        lk.lock();
        frame = p->page_table[page_idx];
        if (frame == -1) return false;
    }

    // read two bytes (little-endian)
    uint8_t b0 = frame_content[frame][offset];
    uint8_t b1 = frame_content[frame][offset + 1];
    out = static_cast<uint16_t>(b0 | (b1 << 8));
    return true;
}

bool MemoryManager::write_u16(const std::shared_ptr<ProcessStub>& p, uint32_t virtual_address, uint16_t value) {
    if (!p) return false;
    std::unique_lock<std::mutex> lk(mtx);
    if (frame_bytes == 0) return false;

    uint32_t page_idx = virtual_address / frame_bytes;
    uint32_t offset = virtual_address % frame_bytes;
    if ((int)page_idx >= p->num_pages) return false;
    if (offset + 2 > frame_bytes) return false; // cannot cross page boundary in this simple model

    int frame = p->page_table[page_idx];
    if (frame == -1) {
        // Need to load it
        lk.unlock();
        if (!ensure_page_loaded(p, virtual_address)) return false;
        lk.lock();
        frame = p->page_table[page_idx];
        if (frame == -1) return false;
    }

    // write two bytes little-endian
    frame_content[frame][offset] = static_cast<uint8_t>(value & 0xFF);
    frame_content[frame][offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);

    // Also update backing_store copy so it remains consistent when evicted
    std::string key = frame_owner[frame];
    if (!key.empty()) {
        backing_store[key] = frame_content[frame];
    }

    // Note: a write doesn't immediately count as paged-out; evictions increment paged-out.
    return true;
}

uint32_t MemoryManager::frame_count() const { return frames_count; }
uint32_t MemoryManager::frame_size() const { return frame_bytes; }