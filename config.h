#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <cstdint>
#include <optional>
#include <fstream>
#include <sstream>
#include <iostream>

using namespace std;

struct Config {
    int num_cpu = 1;                    //[1,128]
    string scheduler = "rr";            //"fcfs" or "rr"
    uint32_t quantum_cycles = 5;        //[1, 2^32-1]
    uint32_t batch_process_freq = 1;    //[1, 2^32-1]
    uint32_t min_ins = 1;               //[1, 2^32-1]
    uint32_t max_ins = 1;               //[1, 2^32-1]
    uint32_t delay_per_exec = 0;        //[0, 2^32-1]
    uint32_t max_overall_mem = 65536;   //[2^6, 2^16] power of 2 format
    uint32_t mem_per_frame = 256;       //[2^6, 2^16] power of 2 format
    uint32_t min_mem_per_proc = 256;    //[2^6, 2^16] power of 2 format
    uint32_t max_mem_per_proc = 4096;   //[2^6, 2^16] power of 2 format
};

static inline bool clamp_int(int &v, int lo, int hi) {
    if (v < lo) { v = lo; return false; }
    if (v > hi) { v = hi; return false; }
    return true;
}

static inline optional<string> load_config_from_file(const string &path, Config &out) {
    ifstream ifs(path);
    if (!ifs) return optional<string>("file-not-found");
    
    string line;
    while (getline(ifs, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        string s = line.substr(start, end - start + 1);
        
        // Skip empty lines and comments
        if (s.empty() || s[0] == '#') continue;
        
        istringstream ss(s);
        string key;
        if (!(ss >> key)) continue;
        
        string val;
        ss >> ws;
        if (ss.peek() == '"') { 
            ss.get(); 
            getline(ss, val, '"'); 
        } else {
            ss >> val;
        }
        
        try {
            if (key == "num-cpu") {
                int v = stoi(val);
                if (v < 1) v = 1; 
                if (v > 128) v = 128;
                out.num_cpu = v;
            } 
            else if (key == "scheduler") {
                // Remove quotes if present
                if (!val.empty() && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                
                // Normalize to lowercase
                for (auto &c : val) c = tolower(c);
                
                if (val == "fcfs" || val == "rr") {
                    out.scheduler = val;
                } else {
                    return optional<string>("invalid-scheduler");
                }
            } 
            else if (key == "quantum-cycles" || key == "quantum_cycles") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                if (v < 1) v = 1; 
                out.quantum_cycles = v;
            } 
            else if (key == "batch-process-freq") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                if (v < 1) v = 1; 
                out.batch_process_freq = v;
            } 
            else if (key == "min-ins") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                if (v < 1) v = 1; 
                out.min_ins = v;
            } 
            else if (key == "max-ins") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                if (v < 1) v = 1; 
                out.max_ins = v;
            } 
            else if (key == "delay-per-exec" || key == "delays-per-exec") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                out.delay_per_exec = v;
            }
            else if (key == "max-overall-mem") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                out.max_overall_mem = v;
                 if (v < 64) v = 64; if (v > 65536) v = 65536;
            }
            else if (key == "mem-per-frame") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                out.mem_per_frame = v;
                 if (v < 64) v = 64; if (v > 65536) v = 65536;
            }
            else if (key == "min-mem-per-proc") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                out.min_mem_per_proc = v;
                 if (v < 64) v = 64; if (v > 65536) v = 65536;
            }
            else if (key == "max-mem-per-proc") {
                uint32_t v = static_cast<uint32_t>(stoul(val));
                out.max_mem_per_proc = v;
                 if (v < 64) v = 64; if (v > 65536) v = 65536;
            }
        } catch (...) {
            return optional<string>("parse-error");
        }
    }
    
    //Ensure max_ins >= min_ins
    if (out.max_ins < out.min_ins) out.max_ins = out.min_ins;
    
    return nullopt;  //Success (no error)
}

#endif
