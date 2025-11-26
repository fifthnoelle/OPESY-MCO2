#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <thread>
#include <queue>
#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include "process.h"
#include "config.h"

using namespace std;

extern atomic<int> active_cores;

class Scheduler {
private:
    Config config;
    CustomProcessLines instructions;
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
            // Wait for batch_process_freq seconds
            int wait_ms = config.batch_process_freq * 1000;  // Convert to milliseconds
            this_thread::sleep_for(chrono::milliseconds(wait_ms));
            
            if (!running.load()) break;
            
            // Create a new process
            string pname = gen_auto_name();
            auto p = create_process(pname);
            int num_ins = config.min_ins + (rand() % (config.max_ins - config.min_ins + 1));
            generate_dummy_instructions(p, num_ins);
            add_log(p, "Generated " + to_string(num_ins) + " randomized instructions");
            add_process(p);
        }
    }

    void core_loop(int core_id) {
        while (running.load()) {
            shared_ptr<ProcessStub> p;
            
            {
                unique_lock<mutex> lk(mtx);
                cv.wait_for(lk, chrono::milliseconds(100), 
                    [&]() { return !ready_queue.empty() || !running.load(); });
                
                if (!running.load()) break;
                if (ready_queue.empty()) continue;
                
                p = ready_queue.front();
                ready_queue.pop();
                core_process[core_id] = p;
                active_cores.fetch_add(1);
            }
            
            if (!p) continue;
            
            p->assigned_core.store(core_id);
            add_log(p, "Core " + to_string(core_id) + ": Picked process " + p->name);
            
            if (config.scheduler == "fcfs") {
                // FCFS: execute until completion
                while (p->current_instruction.load() < p->total_instructions && running.load()) {
                    int delay = (config.delay_per_exec > 0) ? config.delay_per_exec : 1;
                    this_thread::sleep_for(chrono::milliseconds(delay));
                    p->current_instruction.fetch_add(1);
                }
                
                p->finished.store(true);
                p->assigned_core.store(-1);
                add_log(p, "Core " + to_string(core_id) + ": FCFS job finished");
                
            } else if (config.scheduler == "rr") {
                // Round Robin: execute for quantum
                int quantum = config.quantum_cycles;
                for (int q = 0; q < quantum && running.load(); ++q) {
                    if (p->current_instruction.load() >= p->total_instructions) break;
                    
                    int delay = (config.delay_per_exec > 0) ? config.delay_per_exec : 1;
                    this_thread::sleep_for(chrono::milliseconds(delay));
                    p->current_instruction.fetch_add(1);
                }
                
                // Check if process finished
                if (p->current_instruction.load() >= p->total_instructions) {
                    p->finished.store(true);
                    p->assigned_core.store(-1);
                    add_log(p, "Core " + to_string(core_id) + ": RR job finished");
                } else {
                    // Requeue if not finished
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
