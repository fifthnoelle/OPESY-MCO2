#ifndef PROCESS_H
#define PROCESS_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <functional>

using namespace std;

struct CustomProcessLines {
    vector<string> lines = {"DECLARE: uint16_t var1 = 0", "DECLARE: uint16_t var2 = 0", "DECLARE: uint16_t var3 = 0"};
    vector<string> runningLines = {"DECLARE: uint16_t var1 = 0", "DECLARE: uint16_t var2 = 0", "DECLARE: uint16_t var3 = 0"};
    int lineNumber = 0;
};

struct ProcessStub {
    string name;
    int id;
    atomic<bool> finished{false};
    bool attached{false};
    
    struct LogEntry { 
        string timestamp; 
        string message; 
    };
    vector<LogEntry> logs;
    map<string, uint16_t> vars;
    CustomProcessLines code;
    mutex mtx;
    
    // Track instruction execution progress
    atomic<int> current_instruction{0};
    int total_instructions{0};
    string created_timestamp;
    atomic<int> assigned_core{-1};  // -1 = not assigned, 0+ = core number
};

inline map<string, shared_ptr<ProcessStub>> processes;
inline atomic<int> process_counter{0};
inline mutex repository_mutex;

// Timestamp in MM/DD/YYYY HH:MM:SSAM format
inline string timestamp_now() {
    using namespace chrono;
    auto now = system_clock::now();
    time_t t = system_clock::to_time_t(now);
    tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    ostringstream oss;
    oss << setfill('0') << setw(2) << (tm.tm_mon + 1) << "/"
        << setw(2) << tm.tm_mday << "/"
        << (tm.tm_year + 1900) << " "
        << setw(2) << ((tm.tm_hour % 12 == 0) ? 12 : tm.tm_hour % 12) << ":"
        << setw(2) << tm.tm_min << ":"
        << setw(2) << tm.tm_sec
        << (tm.tm_hour < 12 ? "AM" : "PM");
    return oss.str();
}

inline void add_log(const shared_ptr<ProcessStub> &p, const string &msg, int core_id = -1) {
    if (!p) return;
    lock_guard<mutex> lk(p->mtx);
    ProcessStub::LogEntry e;
    e.timestamp = timestamp_now();
    if (core_id >= 0 && msg.find("Core") == string::npos)
        e.message = "Core " + to_string(core_id) + ": " + msg;
    else
        e.message = msg;
    p->logs.push_back(std::move(e));
}

inline shared_ptr<ProcessStub> create_process(const string &name) {
    lock_guard<mutex> lk(repository_mutex);
    auto it = processes.find(name);
    if (it != processes.end()) return it->second;
    
    int id = ++process_counter;
    auto p = make_shared<ProcessStub>();
    p->name = name;
    p->id = id;
    p->finished.store(false);
    p->attached = false;
    p->assigned_core.store(-1);
    p->created_timestamp = timestamp_now();
    add_log(p, string("Hello world from ") + p->name + "!");
    processes[name] = p;
    return p;
}

inline string gen_auto_name() {
    int n = process_counter.load() + 1;
    ostringstream ss;
    ss << "process" << setw(2) << setfill('0') << n;
    return ss.str();
}

inline void generate_dummy_instructions(shared_ptr<ProcessStub> p, int num_instructions) {
    static const vector<string> ops = {"DECLARE", "ADD", "SUBTRACT", "PRINT", "SLEEP", "FOR"};
    p->total_instructions = num_instructions;
    p->current_instruction.store(0);
    
    for (int i = 0; i < num_instructions; ++i) {
        string op = ops[rand() % ops.size()];
        if (op == "DECLARE") {
            string var = "x" + to_string(i);
            int val = rand() % 100;
            p->code.lines.push_back("DECLARE " + var + " " + to_string(val));
        } else if (op == "ADD") {
            p->code.lines.push_back("ADD x0 x1 " + to_string(rand() % 10));
        } else if (op == "SUBTRACT") {
            p->code.lines.push_back("SUBTRACT x0 x1 " + to_string(rand() % 10));
        } else if (op == "PRINT") {
            p->code.lines.push_back("PRINT \"Hello world from " + p->name + "!\"");
        } else if (op == "SLEEP") {
            p->code.lines.push_back("SLEEP " + to_string(rand() % 200));
        } else if (op == "FOR") {
            int repeats = 1 + rand() % 3;
            for (int j = 0; j < repeats; ++j) {
                p->code.lines.push_back("PRINT \"FOR iteration " + to_string(j+1) + "\"");
            }
        }
    }
}

#endif
