#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <thread>
#include <queue>
#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <sstream>
#include "process.h"
#include "config.h"
#include "MemoryManager.h"

extern std::unique_ptr<MemoryManager> mem_manager;

extern std::atomic<uint64_t> used_memory;
extern std::atomic<uint64_t> free_memory;

using namespace std;

extern atomic<int> active_cores;

// VMSTAT globals (defined in osemulator.cpp)
extern atomic<uint64_t> idle_ticks;
extern atomic<uint64_t> active_ticks;
extern atomic<uint64_t> total_ticks;
extern atomic<uint64_t> num_paged_in;
extern atomic<uint64_t> num_paged_out;

class Scheduler {
private:
    Config config;
    atomic<bool> running{false};
    vector<thread> core_threads;
    thread batch_thread;  // Thread for periodic batch process creation
    queue<shared_ptr<ProcessStub>> ready_queue;
    mutable mutex mtx;
    condition_variable cv;
    
    vector<shared_ptr<ProcessStub>> core_process;

public:
    Scheduler(const Config &cfg)
        : config(cfg),
          core_process(cfg.num_cpu, nullptr) {}

    void add_process(shared_ptr<ProcessStub> p) {
        if (!p) return;
        // If process has explicit code lines, treat that size as total_instructions
        {
            lock_guard<mutex> plk(p->mtx);
            if (p->code.lines.size() > 0)
                p->total_instructions = static_cast<int>(p->code.lines.size());
        }

        lock_guard<mutex> lk(mtx);
        ready_queue.push(p);
        cv.notify_one();
    }

    void start() {
        if (running.load()) return;
        running.store(true);
        cout << "Scheduler started (" << config.scheduler
             << ") with " << config.num_cpu << " cores." << endl;
        
        // Start core threads
        for (int i = 0; i < config.num_cpu; ++i)
            core_threads.emplace_back(&Scheduler::core_loop, this, i);
        
        // Start batch process creation thread
        batch_thread = thread(&Scheduler::batch_process_loop, this);
    }

    void stop() {
        running.store(false);
        cv.notify_all();
        
        if (batch_thread.joinable()) batch_thread.join();
        
        for (auto &t : core_threads)
            if (t.joinable()) t.join();
        core_threads.clear();
        
        cout << "Scheduler stopped." << endl;
    }

    bool is_running() const { return running.load(); }

    vector<shared_ptr<ProcessStub>> get_core_processes() const {
        lock_guard<mutex> lk(mtx);
        return core_process;
    }

private:
    // Periodic batch process creation
    void batch_process_loop() {
        while (running.load()) {
            int wait_ms = config.batch_process_freq * 1000;  // Convert to milliseconds
            this_thread::sleep_for(chrono::milliseconds(wait_ms));
            
            if (!running.load()) break;
            
            // Create a new process
            string pname = gen_auto_name();
            auto p = create_process(pname);
            int num_ins = config.min_ins + (rand() % (config.max_ins - config.min_ins + 1));
            generate_dummy_instructions(p, num_ins);
               // Allocate memory for this process (randomize between min and max)
               uint32_t mem = config.min_mem_per_proc + (rand() % (config.max_mem_per_proc - config.min_mem_per_proc + 1));
               p->memory_required = mem;
               if (mem_manager && mem <= free_memory.load()) {
                        mem_manager->allocate_process(p, mem);
                        used_memory += mem;
                        free_memory -= mem;
                      }
            add_log(p, "Generated " + to_string(num_ins) + " randomized instructions");
            add_process(p);
        }
    }

    // Helper to parse numeric or variable operand inside a process
    static uint16_t resolve_operand(const shared_ptr<ProcessStub>& p, const string &tok) {
        if (tok.empty()) return 0;
        // try integer
        try {
            int v = stoi(tok);
            if (v < 0) return 0;
            if (v > 65535) return 65535;
            return static_cast<uint16_t>(v);
        } catch (...) {
            lock_guard<mutex> lk(p->mtx);
            auto it = p->vars.find(tok);
            if (it == p->vars.end()) {
                p->vars[tok] = 0;
                return 0;
            }
            return it->second;
        }
    }

    // Execute a single instruction for process p (hybrid model: only some ops)
    void execute_instruction(const shared_ptr<ProcessStub>& p, const string &instr, int core_id) {
        if (!p) return;
        // trim
        size_t s = instr.find_first_not_of(" \t\r\n");
        if (s == string::npos) return;
        string line = instr.substr(s);

        // simple tokenization
        istringstream iss(line);
        string op;
        iss >> op;
        if (op == "SLEEP") {
            string tstr; iss >> tstr;
            int t = 50;
            try { t = stoi(tstr); } catch(...) {}
            add_log(p, "SLEEP start for " + to_string(t) + " ms", core_id);
            this_thread::sleep_for(chrono::milliseconds(t));
            add_log(p, "SLEEP end", core_id);
        } else if (op == "PRINT") {
            // rest of line is message (may be quoted)
            string rest;
            getline(iss, rest);
            if (!rest.empty()) {
                // trim leading spaces
                size_t ppos = rest.find_first_not_of(" \t\r\n\"");
                if (ppos != string::npos) rest = rest.substr(ppos);
                // trim trailing quotes/spaces
                size_t epos = rest.find_last_not_of(" \t\r\n\"");
                if (epos != string::npos) rest = rest.substr(0, epos+1);
            }
            add_log(p, string("PRINT: ") + rest, core_id);
        } else if (op == "ADD" || op == "SUB") {
            string target, a, b;
            iss >> target >> a >> b;
            if (target.empty() || a.empty() || b.empty()) {
                add_log(p, string("Malformed ") + op + " instruction", core_id);
                return;
            }
            uint16_t va = resolve_operand(p, a);
            uint16_t vb = resolve_operand(p, b);
            uint16_t res = 0;
            if (op == "ADD") {
                int sum = (int)va + (int)vb;
                if (sum > 65535) sum = 65535;
                res = static_cast<uint16_t>(sum);
            } else {
                res = (va > vb) ? static_cast<uint16_t>(va - vb) : 0;
            }
            {
                lock_guard<mutex> lk(p->mtx);
                p->vars[target] = res;
            }
            add_log(p, op + string(": ") + target + " = " + to_string(res), core_id);
        } else if (op == "FOR") {
            // FOR n - expand into n no-op iterations quickly (we treat FOR as 1 instruction for simplicity)
            string nstr; iss >> nstr;
            int n = 1;
            try { n = stoi(nstr); } catch(...) {}
            // We'll log and sleep briefly to simulate loop overhead
            add_log(p, "FOR start x" + to_string(n), core_id);
            this_thread::sleep_for(chrono::milliseconds(10 * min(5, n)));
            add_log(p, "FOR end", core_id);
        } else {
            // Unknown or operations reserved for interactive session (READ/WRITE/DECLARE)
            add_log(p, string("Skipped instruction (not executed by scheduler): ") + op, core_id);
        }
    }

    void core_loop(int core_id) {
        while (running.load()) {
            // Each pass is one tick
            total_ticks++;

            shared_ptr<ProcessStub> p;

            {
                unique_lock<mutex> lk(mtx);
                // Wait small amount for new work (tick granularity)
                cv.wait_for(lk, chrono::milliseconds(100), [&]() { return !ready_queue.empty() || !running.load(); });

                if (!running.load()) break;

                if (ready_queue.empty()) {
                    // no work this tick
                    idle_ticks++;
                    continue;
                }

                // there is work: take next
                active_ticks++;
                p = ready_queue.front();
                ready_queue.pop();
                core_process[core_id] = p;
                active_cores.fetch_add(1);
            }

            if (!p) {
                // nothing to do
                {
                    lock_guard<mutex> lk(mtx);
                    core_process[core_id] = nullptr;
                    active_cores.fetch_sub(1);
                }
                continue;
            }

            p->assigned_core.store(core_id);
            add_log(p, "Core " + to_string(core_id) + ": Picked process " + p->name, core_id);
            // Ensure all process pages are loaded for demand paging
 if (mem_manager && p->num_pages > 0) {
   for (int page_idx = 0; page_idx < p->num_pages; ++page_idx) {
     mem_manager->ensure_page_loaded(p, page_idx * mem_manager->frame_size());
   }
 }
            // Hybrid model: scheduler executes a limited set of instructions
            if (config.scheduler == "fcfs") {
                // FCFS: run until completion
                while (running.load() && p->current_instruction.load() < p->total_instructions) {
                    int idx = p->current_instruction.load();
                    string instr;
                    {
                        lock_guard<mutex> lk(p->mtx);
                        if (idx < (int)p->code.lines.size()) instr = p->code.lines[idx];
                    }
                    // execute the instruction (only allowed subset will be executed)
                    execute_instruction(p, instr, core_id);

                    // instruction execution takes a base time (simulate)
                    int delay = (config.delay_per_exec > 0) ? config.delay_per_exec : 1;
                    this_thread::sleep_for(chrono::milliseconds(delay));

                    p->current_instruction.fetch_add(1);
                }

                // If FCFS finished
                if (p->current_instruction.load() >= p->total_instructions) {
                    p->finished.store(true);
                    p->assigned_core.store(-1);
                    add_log(p, "Core " + to_string(core_id) + ": FCFS job finished", core_id);

                    // Free process memory if allocated
                    if (mem_manager && p->memory_required > 0) {
                        mem_manager->free_process(p);
                        used_memory -= p->memory_required;
                        free_memory += p->memory_required;
                    }
                }
            } else if (config.scheduler == "rr") {
                // Round Robin: execute for quantum instructions
                int quantum = config.quantum_cycles;
                for (int q = 0; q < quantum && running.load(); ++q) {
                    if (p->current_instruction.load() >= p->total_instructions) break;

                    int idx = p->current_instruction.load();
                    string instr;
                    {
                        lock_guard<mutex> lk(p->mtx);
                        if (idx < (int)p->code.lines.size()) instr = p->code.lines[idx];
                    }

                    execute_instruction(p, instr, core_id);

                    int delay = (config.delay_per_exec > 0) ? config.delay_per_exec : 1;
                    this_thread::sleep_for(chrono::milliseconds(delay));

                    p->current_instruction.fetch_add(1);
                }

                if (p->current_instruction.load() >= p->total_instructions) {
                    p->finished.store(true);
                    p->assigned_core.store(-1);
                    add_log(p, "Core " + to_string(core_id) + ": RR job finished", core_id);

                    // Free process memory if allocated
                    if (mem_manager && p->memory_required > 0) {
                        mem_manager->free_process(p);
                        used_memory -= p->memory_required;
                        free_memory += p->memory_required;
                    }
                } else {
                    // requeue
                    p->assigned_core.store(-1);
                    lock_guard<mutex> lk(mtx);
                    ready_queue.push(p);
                    cv.notify_one();
                }
            }

            {
                lock_guard<mutex> lk(mtx);
                core_process[core_id] = nullptr;
                active_cores.fetch_sub(1);
            }
        }
    }
};

#endif
