//g++ -std=c++17 -O2 -pthread -o osemulator.exe osemulator.cpp MemoryManager.cpp
//.\osemulator.exe

/** Need to show in process the lines of code, etc. */
#include <iostream>
#include <sstream>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <vector>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <algorithm>

#include "config.h"
#include "process.h"
#include "scheduler.h"
#include "MemoryManager.h"

using namespace std;
std::atomic<int> active_cores{0};

// Memory statistics
std::atomic<uint64_t> total_memory{0};
std::atomic<uint64_t> used_memory{0};
std::atomic<uint64_t> free_memory{0};

// VMSTAT counters (non-static so other translation units can extern them)
std::atomic<uint64_t> idle_ticks{0};
std::atomic<uint64_t> active_ticks{0};
std::atomic<uint64_t> total_ticks{0};
std::atomic<uint64_t> num_paged_in{0};
std::atomic<uint64_t> num_paged_out{0};

//ProcessStub and repository helpers are provided in process.h

//Config (from config.txt after initialization)
static Config global_config;
static bool initialized = false;
static unique_ptr<Scheduler> scheduler;
//std::unique_ptr<MemoryManager> mem_manager;

/*
Use these to pass config values for scheduler
config.num_cpu = num_cpu
config.scheduler = scheduler
config.quantum_cycles = quantum_cycles
config.batch_process_freq = batch_process_freq
config.min_ins <<  endl;
config.max_ins <<  endl;
config.delay_per_exec <<  endl;
*/

//Flags for display
static  atomic<bool> scheduler_running{false};
static  thread scheduler_thread;
static  condition_variable_any scheduler_cv;

//Util for clearing console
static void clear_console() {
    cout << string(50, '\n');
}

// Forward-declare vmstat so it's callable inside screen
static void vmstat();

//Print summary works for displaying and writing to file
static void print_summary(ostream &out) {
    extern atomic<int> active_cores;
    double utilization = (global_config.num_cpu > 0) ? (100.0 * active_cores.load()) / global_config.num_cpu : 0.0;
    
    out << fixed << setprecision(2);
    out << "CPU Utilization: " << utilization << "%" << endl;
    out << "Memory Summary:" << endl;
    out << "  Total Memory: " << total_memory.load() << " bytes" << endl;
    out << "  Used Memory : " << used_memory.load() << " bytes" << endl;
    out << "  Free Memory : " << free_memory.load() << " bytes" << endl;
    out << "---------------------------------------------------" << endl;
    out << "Cores used: " << active_cores.load() << endl;
    out << "Cores available: " << (global_config.num_cpu - active_cores.load()) << endl;
    out << "---------------------------------------------------" << endl;
    out << "Running Processes:" << endl;
    
    // Display running processes
    lock_guard<mutex> lk(repository_mutex);
    for (auto &kv : processes) {
        auto &p = kv.second;
        if (!p->finished.load() && p->assigned_core.load() >= 0) {
            out << p->name << "\t("
                << p->created_timestamp << ")\t"
                << "Memory: " << p->memory_required << " bytes\t"
                << "Core: " << p->assigned_core.load() << "\t"
                << p->current_instruction.load() << " / " << p->total_instructions
                << endl;
        }
    }
    
    out << "\nFinished Processes:" << endl;
    for (auto &kv : processes) {
        auto &p = kv.second;
        if (p->finished.load()) {
            out << p->name << "\t("
                << p->created_timestamp << ")\t"
                << "Memory: " << p->memory_required << " bytes\t"
                << "Finished\t"
                << p->total_instructions << " / " << p->total_instructions
                << endl;
        }
    }
    out << "---------------------------------------------------" << endl;
}

//Save summary to file for report-util
static void save_report_util(const  string &path) {
    ofstream ofs(path);
    if (!ofs) {
        cout << "Failed to open " << path << " for writing." <<  endl;
        return;
    }
    print_summary(ofs);
    ofs.close();
    cout << "Saved report to " << path <<  endl;
}

static void print_process(const shared_ptr<ProcessStub>& p) {
    if (!p) return;
    
    cout << "\nProcess name: " << p->name << endl;
    cout << "ID: " << p->id << endl;
    cout << "Memory allocated: " << p->memory_required << " bytes" << endl;
    cout << "Assigned core: " << (p->assigned_core.load() >= 0 ? to_string(p->assigned_core.load()) : "N/A") << endl;
    cout << "Status: " << (p->finished.load() ? "Finished" : "Running") << endl;
    cout << "Progress: " << p->current_instruction.load() << " / " << p->total_instructions << " instructions" << endl;
    
    double cpu_util = (p->total_instructions > 0) 
        ? (100.0 * p->current_instruction.load()) / p->total_instructions 
        : 0.0;
    cout << "CPU Utilization: " << fixed << setprecision(1) << cpu_util << "%" << endl;
    
    cout << "\nLogs:" << endl;
    {
        lock_guard<mutex> plk(p->mtx);
        if (p->logs.empty()) {
            cout << "  (no logs)" << endl;
        } else {
            for (const auto &entry : p->logs) {
                cout << "  (" << entry.timestamp << ") " << entry.message << endl;
            }
        }
    }
    
    cout << "\nLines of Code:" << endl;
    if (p->code.lines.empty()) {
        cout << "  (no instructions)" << endl;
    } else {
        for (size_t i = 0; i < p->code.lines.size() && i < 20; ++i) {
            cout << "  " << (i + 1) << ": " << p->code.lines[i] << endl;
        }
        if (p->code.lines.size() > 20) {
            cout << "  ... (" << (p->code.lines.size() - 20) << " more lines)" << endl;
        }
    }
    cout << endl;
}


//Run process interactive screen
static void run_process_screen(const string& process_name) {
    shared_ptr<ProcessStub> p;
    {
        lock_guard<mutex> lk(repository_mutex);
        auto it = processes.find(process_name);
        if (it == processes.end()) {
            cout << "Process " << process_name << " not found." << endl;
            return;
        }
        p = it->second;
    }

    if (p->finished) {
        cout << "Process " << process_name << " has already finished execution, but you can still view its logs." << endl;
        return;
    }

    clear_console();
    print_process(p);

    string line;
    while (true) {
        cout << "root:\\" << process_name << "\\> " << flush;

        if (!std::getline(cin, line)) {
            cout << "\nInput closed. Exiting process screen." << endl;
            break;
        }

        size_t pos = line.find_first_not_of(" \t\r\n");
        if (pos == string::npos) continue;
        if (pos > 0) line = line.substr(pos);

        istringstream ss(line);
        string cmd;
        ss >> cmd;
        if (cmd.empty()) continue;

        if (cmd == "exit") {
            break;
        } else if (cmd == "process-smi") {
            print_process(p);
        } else if (cmd == "vmstat") {
            // allow vmstat inside screen
            vmstat();
        } else if (cmd == "declare") {
            string var, val_str;
            cout << "Enter variable name: ";
            cout.flush();
            if (!getline(cin, var)) { cout << "Input aborted.\n"; continue; }
            auto trim = [](string &s) {
                size_t start = s.find_first_not_of(" \t\r\n");
                size_t end = s.find_last_not_of(" \t\r\n");
                if (start == string::npos) { s.clear(); return; }
                s = s.substr(start, end - start + 1);
            };
            trim(var);
            if (var.empty()) { cout << "Invalid variable name.\n"; continue; }

            cout << "Enter value: ";
            cout.flush();
            if (!getline(cin, val_str)) { cout << "Input aborted.\n"; continue; }
            trim(val_str);
            if (val_str.empty()) { cout << "Invalid value.\n"; continue; }

            try {
                int val = stoi(val_str);

                {
                    lock_guard<mutex> lk(p->mtx);
                    // symbol table limit: 32 variables
                    if (p->vars.size() >= 32) {
                        cout << "Symbol table full (32 variables). Declaration ignored.\n";
                    } else {
                        p->vars[var] = static_cast<uint16_t>(max(0, min(65535, val)));
                        add_log(p, "Declared " + var + " = " + to_string(val));
                        ostringstream linebuf;
                        linebuf << "DECLARE:        uint16_t " << var << " = " << val << ";";
                        p->code.lines.push_back(linebuf.str());
                        p->total_instructions = static_cast<int>(p->code.lines.size());
                        cout << "Variable '" << var << "' = " << val << " declared successfully." << endl;
                    }
                }
            }
            catch (const exception &e) {
                cout << "Invalid value: must be an integer. (" << e.what() << ")\n";
            }

        } else if (cmd == "print") {
            string rest;
            if (!getline(ss, rest) || rest.find_first_not_of(" \t\r\n") == string::npos) {
                cout << "Enter message to PRINT: " << flush;
                if (!getline(cin, rest)) break;
            }
            size_t s = rest.find_first_not_of(" \t\r\n");
            if (s != string::npos) rest = rest.substr(s);
            add_log(p, string("PRINT:       ") + rest);
            ostringstream linebuf;
            linebuf << "PRINT:      " << rest;
            lock_guard<mutex> lk(p->mtx);
            p->code.lines.push_back(linebuf.str());
            p->total_instructions = static_cast<int>(p->code.lines.size());
            cout << "Printed message logged." << endl;

        } else if (cmd == "read") {
            // usage: read <var> <hexaddress>
            string var, addrstr;
            if (!(ss >> var >> addrstr)) {
                cout << "Usage: read <var> <hexaddress>\n" << flush;
                continue;
            }

            uint32_t addr = 0;
            try { addr = std::stoul(addrstr, nullptr, 0); } catch (...) { 
                cout << "Invalid address\n" << flush; 
                continue; 
            }

            if (!mem_manager) {
                cout << "Memory manager not available\n" << flush;
                continue;
            }

            uint16_t val = 0;
            bool ok = mem_manager->read_u16(p, addr, val); // make sure you have this function

            if (!ok) {
                cout << "Memory access violation at " << addrstr << "\n" << flush;
                p->finished.store(true);
                add_log(p, string("Memory access violation at ") + addrstr);

                // reclaim memory
                mem_manager->free_process(p);
                used_memory -= p->memory_required;
                free_memory += p->memory_required;

                cout << "Process " << p->name << " shut down due to memory access violation.\n";
                break;
            }

            {
                lock_guard<mutex> lk(p->mtx);
                // store only if table has space
                if (p->vars.size() < 32) {
                    p->vars[var] = val;
                } else {
                    cout << "[Warning] Symbol table full (32 variables). Value not stored, but read will display.\n";
                }

                add_log(p, string("READ: ") + var + " <- " + to_string(val));

                // always print
                cout << var << " = " << val << endl;
                cout.flush();
                fflush(stdout);
            }
        } else if (cmd == "write") {
            // usage: write <hexaddress> <value>
            string addrstr, valstr;
            if (!(ss >> addrstr >> valstr)) {
                cout << "Usage: write <hexaddress> <value>\n";
                continue;
            }
            uint32_t addr = 0;
            try { addr = std::stoul(addrstr, nullptr, 0); } catch (...) { cout << "Invalid address\n"; continue; }
            int v = 0;
            try { v = stoi(valstr); } catch(...) { cout << "Invalid value\n"; continue; }
            uint16_t uv = static_cast<uint16_t>(std::max(0, std::min(65535, v)));
            if (!mem_manager) { cout << "Memory manager not available\n"; continue; }
            if (!mem_manager->write_u16(p, addr, uv)) {
                cout << "Memory access violation at " << addrstr << endl;
                p->finished.store(true);
                add_log(p, string("Memory access violation at ") + addrstr);
                mem_manager->free_process(p);
                used_memory -= p->memory_required;
                free_memory += p->memory_required;
                cout << "Process " << p->name << " shut down due to memory access violation error at " << timestamp_now() << ". " << addrstr << " invalid." << endl;
                break;
            }
            add_log(p, string("WRITE: ") + addrstr + " <- " + to_string(uv));
            cout << "Wrote " << uv << " to " << addrstr << endl;
        } else if (cmd == "sleep") {
            string tstr;
            if (!(ss >> tstr)) {
                cout << "Enter sleep time in ms: " << flush;
                if (!getline(cin, tstr)) break;
            }
            try {
                int t = stoi(tstr);
                add_log(p, "SLEEP start for " + to_string(t) + " ms");
                this_thread::sleep_for(chrono::milliseconds(t));
                add_log(p, "SLEEP end");
                lock_guard<mutex> lk(p->mtx);
                p->code.lines.push_back(string("SLEEP:      ") + to_string(t) + "ms");
                p->total_instructions = static_cast<int>(p->code.lines.size());
                cout << "Slept " << t << " ms." << endl;
            } catch (...) {
                cout << "Invalid number." << endl;
            }
        } else if (cmd == "for") {
            string countstr;
            if (!(ss >> countstr)) {
                cout << "Enter repeat count: " << flush;
                if (!getline(cin, countstr)) break;
            }
            int cnt = 0;
            try { cnt = stoi(countstr); } catch(...) { cout << "Invalid count\n"; continue; }
            add_log(p, "FOR start x" + to_string(cnt));
            for (int i = 0; i < cnt; ++i) {
                add_log(p, "FOR iteration " + to_string(i+1));
                this_thread::sleep_for(chrono::milliseconds(50));
            }
            add_log(p, "FOR end");
            lock_guard<mutex> lk(p->mtx);
            p->code.lines.push_back(string("FOR x") + to_string(cnt));
            p->total_instructions = static_cast<int>(p->code.lines.size());
            cout << "For loop executed " << cnt << " times." << endl;
        } else if (cmd == "add" || cmd == "sub") {
            string var1, var2, var3;
            cout << "Enter target variable: ";
            getline(cin, var1);
            cout << "Enter first operand (variable or value): ";
            getline(cin, var2);
            cout << "Enter second operand (variable or value): ";
            getline(cin, var3);

            auto trim = [](string &s) {
                size_t start = s.find_first_not_of(" \t\r\n");
                size_t end = s.find_last_not_of(" \t\r\n");
                if (start == string::npos) { s.clear(); return; }
                s = s.substr(start, end - start + 1);
            };
            trim(var1); trim(var2); trim(var3);
            if (var1.empty() || var2.empty() || var3.empty()) {
                cout << "Invalid input.\n"; 
                continue;
            }

            auto get_val = [&](const string &s) -> uint16_t {
                try {
                    int v = stoi(s);
                    if (v < 0) return 0;
                    if (v > 65535) return 65535;
                    return static_cast<uint16_t>(v);
                } catch (...) {
                    lock_guard<mutex> lk(p->mtx);
                    if (p->vars.find(s) == p->vars.end()) p->vars[s] = 0;
                    return p->vars[s];
                }
            };

            uint16_t v2 = get_val(var2);
            uint16_t v3 = get_val(var3);
            uint16_t result = (cmd == "add")
                ? static_cast<uint16_t>(min(65535, (int)v2 + (int)v3))
                : static_cast<uint16_t>((v2 > v3) ? (v2 - v3) : 0);

            {
                lock_guard<mutex> lk(p->mtx);
                p->vars[var1] = result;
                ostringstream linebuf;
                linebuf << (cmd=="add"?"ADD":"SUB") << ": "
                        << var1 << " = " << var2 << " "
                        << (cmd=="add"?"+":"-") << " "
                        << var3 << " -> " << result;
                p->code.lines.push_back(linebuf.str());
                p->total_instructions = static_cast<int>(p->code.lines.size());
            }

            add_log(p, (cmd == "add" ? "ADD:        " : "SUB:       ") + 
                    var1 + " = " + var2 + 
                    (cmd=="add"?" + ":" - ") + 
                    var3 + " -> " + to_string(result));

            cout << (cmd=="add"?"Added":"Subtracted") << " successfully. "
                << var1 << " = " << result << endl;
        }

        else {
            cout << "Unknown command inside screen. Available: process-smi, vmstat, exit, declare, add, sub, print, sleep, for, read, write" << endl;
        }
    }

    if (scheduler && !p->finished) {
        scheduler->add_process(p);
        cout << "[Info] Process " << p->name << " added to scheduler queue.\n";
    }

    clear_console();
}

static void vmstat() {
    uint64_t total_mem = total_memory.load();
    uint64_t used_mem = used_memory.load();
    uint64_t free_mem = free_memory.load();
    
    double mem_util = (total_mem > 0) ? (100.0 * used_mem) / total_mem : 0.0;
    
    cout << "\n===== VMSTAT (Memory & Paging Statistics) =====\n\n";
    
    cout << "Memory Summary:\n";
    cout << "  Total Memory: " << total_mem << " bytes" << endl;
    cout << "  Used Memory : " << used_mem << " bytes (" << fixed << setprecision(1) << mem_util << "%)" << endl;
    cout << "  Free Memory : " << free_mem << " bytes" << endl;
    
    cout << "\nCPU Ticks Summary:\n";
    cout << "  Idle Ticks  : " << idle_ticks.load() << endl;
    cout << "  Active Ticks: " << active_ticks.load() << endl;
    cout << "  Total Ticks : " << total_ticks.load() << endl;
    
    uint64_t total_cpu = total_ticks.load();
    if (total_cpu > 0) {
        double idle_pct = (100.0 * idle_ticks.load()) / total_cpu;
        double active_pct = (100.0 * active_ticks.load()) / total_cpu;
        cout << "  CPU Usage   : " << fixed << setprecision(1) << active_pct << "% (Idle: " << idle_pct << "%)" << endl;
    }
    
    cout << "\nPaging Statistics:\n";
    cout << "  Pages In  : " << num_paged_in.load() << endl;
    cout << "  Pages Out : " << num_paged_out.load() << endl;
    cout << "  Total Page Faults: " << (num_paged_in.load() + num_paged_out.load()) << endl;
    
    cout << "\n===============================================\n" << endl;
}



//Main menu loop
static void run_main_menu() {
    string command;

    cout << "Welcome to CSOPESY!" <<  endl;
    cout << "Version Date: December 1, 2025" <<  endl <<  endl;
    cout.flush();

    while (true) {
        cout << "root:\\> " << flush;
        if (! getline( cin, command)) break;

        stringstream ss(command);
        string root;
        ss >> root;
        if (root.empty()) continue;

        if (root == "exit") {
            //Stop scheduler if running
            if (scheduler_running.load()) {
                scheduler_running.store(false);
                if (scheduler_thread.joinable()) scheduler_thread.join();
            }
            break;
        }

        if (root == "initialize") {
            //Load config.txt
            auto err = load_config_from_file("config.txt", global_config);
            if (err.has_value()) {
                cout << "Failed to initialize: " << err.value() <<  endl;
            } else {
                initialized = true;
                cout << "Initialized from config.txt" <<  endl;
                cout << " num-cpu=" << global_config.num_cpu  <<  endl;
                cout << " scheduler=" << global_config.scheduler <<  endl;
                cout << " quantum-cycles=" << global_config.quantum_cycles <<  endl;
                cout << " batch-process-freq=" << global_config.batch_process_freq <<  endl;
                cout << " min-ins=" << global_config.min_ins <<  endl;
                cout << " max-ins=" << global_config.max_ins <<  endl;
                cout << " delay-per-exec=" << global_config.delay_per_exec <<  endl;
                 cout << " max-overall-mem=" << global_config.max_overall_mem << endl;
                 cout << " mem-per-frame=" << global_config.mem_per_frame << endl;
                 cout << " min-mem-per-proc=" << global_config.min_mem_per_proc << endl;
                 cout << " max-mem-per-proc=" << global_config.max_mem_per_proc << endl;

                total_memory.store(global_config.max_overall_mem);
                free_memory.store(global_config.max_overall_mem);
                used_memory.store(0);

                // Initialize memory manager
                mem_manager = std::make_unique<MemoryManager>();
                mem_manager->init(global_config.max_overall_mem, global_config.mem_per_frame);

                scheduler = make_unique<Scheduler>(global_config);
                cout << "Scheduler object created successfully." << endl;
            }
            continue;
        }

        if (!initialized && root != "exit") {
            cout << "Error: Must run 'initialize' first." <<  endl;
            continue;
        }

        if (root == "screen") {
            string opt;
            ss >> opt;
            if (opt == "-s") {
                string pname; 
                uint32_t mem;
                ss >> pname >> mem;

                if (pname.empty() || mem == 0) {
                    cout << "Usage: screen -s <name> <memory_size>" << endl;
                    continue;
                }

                // Validate memory
                if (mem < 64 || mem > 65536 || (mem & (mem - 1)) != 0) {
                    cout << "invalid memory allocation" << endl;
                    continue;
                }

                auto p = create_process(pname);
                p->memory_required = mem;

                if (!mem_manager) {
                    cout << "Memory manager not initialized. Run initialize first." << endl;
                    continue;
                }
                if (!mem_manager->allocate_process(p, mem)) {
                    cout << "Failed to allocate page table for process." << endl;
                    continue;
                }

                // Update memory counters
                if (free_memory.load() < mem) {
                    cout << "Not enough memory available." << endl;
                    continue;
                }

                used_memory += mem;
                free_memory -= mem;

                run_process_screen(pname);
                continue;
            }
            else if (opt == "-r") {
                string pname;
                ss >> pname;
                if (pname.empty()) {
                    cout << "Usage: screen -r <process_name>" <<  endl;
                } else {
                    run_process_screen(pname);
                }
            } else if (opt == "-c") {
                string pname;
                uint32_t mem;
                string instr;

                ss >> pname >> mem;
                getline(ss, instr); // includes quotes
                size_t start = instr.find('"');
                size_t end = instr.find_last_of('"');

                if (start == string::npos || end == string::npos || end <= start)
                {
                    cout << "invalid command" << endl;
                    continue;
                }

                string full_instructions = instr.substr(start + 1, end - start - 1);

                // Split by semicolon
                vector<string> ins_list;
                string temp;
                stringstream s2(full_instructions);
                while (getline(s2, temp, ';')) {
                    size_t p = temp.find_first_not_of(" \t\r\n");
                    if (p != string::npos)
                        ins_list.push_back(temp.substr(p));
                }

                if (ins_list.empty() || ins_list.size() > 50) {
                    cout << "invalid command" << endl;
                    continue;
                }

                // validate memory like -s
                if (mem < 64 || mem > 65536 || (mem & (mem - 1)) != 0) {
                    cout << "invalid memory allocation" << endl;
                    continue;
                }

                auto p = create_process(pname);
                p->memory_required = mem;

                if (!mem_manager) {
                    cout << "Memory manager not initialized. Run initialize first." << endl;
                    continue;
                }
                if (!mem_manager->allocate_process(p, mem)) {
                    cout << "Failed to allocate page table for process." << endl;
                    continue;
                }

                if (free_memory.load() < mem) {
                    cout << "Not enough memory available." << endl;
                    continue;
                }

                used_memory += mem;
                free_memory -= mem;

                // ADD INSTRUCTIONS TO CODE
                for (auto &i : ins_list) {
                    p->code.lines.push_back(i);
                }
                p->total_instructions = static_cast<int>(p->code.lines.size());

                cout << "Process " << pname << " created with custom instructions." << endl;
                continue;
            }
            else if (opt == "-ls") {
                print_summary( cout);
            } else {
                cout << "screen commands: -s <name> (create+attach), -r <name> (attach), -c <name> <mem> \"instr...\" , -ls (list)" <<  endl;
            }
            continue;
        }

        if (root == "scheduler-start") {
            if (scheduler_running.load()) {
                cout << "Scheduler already running." << endl;
            } else {
                scheduler_running.store(true);
                if (scheduler) scheduler->start();
                cout << "Scheduler started." << endl;
            }
            continue;
        }

        if (root == "scheduler-test") {
            // Run scheduler for a single test batch - create processes once
            if (scheduler_running.load()) {
                cout << "Scheduler already running." << endl;
            } else {
                scheduler_running.store(true);
                // Run batch_process_loop once to create a single batch of processes
                if (scheduler) scheduler->batch_process_loop();
                cout << "Scheduler test batch created." << endl;
            }
            continue;

        if (root == "scheduler-stop") {
            if (!scheduler || !scheduler->is_running()) {
                cout << "Scheduler is not running." << endl;
            } else {
                if (scheduler) scheduler->stop();
            }
            continue;
        }

        if (root == "report-util") {
            save_report_util("csopesy-log.txt");
            continue;
        }

        if (root == "vmstat") {
            vmstat();
            continue;
        }

        if (root == "process-smi") {
    lock_guard<mutex> lk(repository_mutex);
    if (processes.empty()) {
        cout << "No processes found." << endl;
    } else {
        cout << "\n===== PROCESS SUMMARY =====\n";
        for (auto &kv : processes) {
            print_process(kv.second);
        }
        cout << "==========================\n" << endl;
    }
    continue;
}

        cout << "Unknown command. Available: initialize, exit, screen, scheduler-start, scheduler-stop, report-util, vmstattat, vmstat, process-smi" << endl;
    }
}

int main() {
    run_main_menu();
    return 0;
}
