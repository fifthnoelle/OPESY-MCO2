//g++ -std=c++17 -O2 -pthread -o osemulator.exe osemulator.cpp
//.\osemulator.exe

/**Need to show in process the lines of code, etc. */
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
#include "config.h"
#include "process.h"
#include "scheduler.h"

using namespace std;
std::atomic<int> active_cores{0};

//ProcessStub and repository helpers are provided in process.h

//Config (from config.txt after initialization)
static Config global_config;
static bool initialized = false;
static unique_ptr<Scheduler> scheduler; 

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
    //Clear screen, implement later?
    //for (int i = 0; i < 60; ++i)  cout << '\n';
    cout << string(50, '\n');
}

//This is not a real scheduler, just simulating process creation and finishing, pls delete later
static void scheduler_loop() {
    int counter = 0;
    while (scheduler_running.load()) {
        this_thread::sleep_for(chrono::milliseconds(global_config.batch_process_freq));

        string name = gen_auto_name();
        auto p = create_process(name);
        if (scheduler) scheduler->add_process(p);

        // Random instruction count between min and max
        int num_ins = global_config.min_ins + rand() % (global_config.max_ins - global_config.min_ins + 1);

        // Fill with dummy instructions
        generate_dummy_instructions(p, num_ins);

        //scheduler.enqueue(p); // Send to Scheduler queue

        cout << "[Scheduler] Generated process " << name
             << " with " << num_ins << " instructions." << endl;
    }
}

//Print summary works for displaying and writing to file
static void print_summary(ostream &out) {
    extern atomic<int> active_cores;
    double utilization = (100.0 * active_cores.load()) / global_config.num_cpu;
    
    out << fixed << setprecision(2);
    out << "CPU Utilization: " << utilization << "%" << endl;
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

static void print_process(const  shared_ptr<ProcessStub>& p) {

    CustomProcessLines cpl;
    cout << "\nProcess name: " << p->name <<  endl;
    cout << "ID: " << p->id <<  endl;
    cout << "Logs: " <<  endl;
    {
         lock_guard< mutex> plk(p->mtx);
        for (const auto &entry : p->logs) {
             cout << "(" << entry.timestamp << ")";
             cout << "\t\"" << entry.message << "\"" <<  endl;
        }
    }
    //cout << "\nCurrent Instruction Line:\n";
    //for (size_t i = 0; i < p->code.runningLines.size(); ++i) {
    //    cout << (i + 1) << "     " << p->code.runningLines[i] << endl;
    //}

    cout << "\nLines of Code:\n";
    for (size_t i = 0; i < p->code.lines.size(); ++i) {
        cout << (i + 1) << "     " << p->code.lines[i] << endl;
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

        // read the whole command line safely
        if (!std::getline(cin, line)) {
            // EOF or Ctrl+D/Ctrl+Z — exit the screen
            cout << "\nInput closed. Exiting process screen." << endl;
            break;
        }

        // trim leading spaces
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
        } else if (cmd == "declare") {
            string var, val_str;
            //cout << "[DEBUG] Entering declare command...\n";
            //cout.flush();

            cout << "Enter variable name: ";
            cout.flush();
            if (!getline(cin, var)) {
                //cout << "[DEBUG] getline for var failed\n";
                cout << "Input aborted.\n";
                continue;
            }
            //cout << "[DEBUG] Raw var input: '" << var << "'\n";

            // trim spaces
            auto trim = [](string &s) {
                size_t start = s.find_first_not_of(" \t\r\n");
                size_t end = s.find_last_not_of(" \t\r\n");
                if (start == string::npos) { s.clear(); return; }
                s = s.substr(start, end - start + 1);
            };
            trim(var);
            //cout << "[DEBUG] Trimmed var: '" << var << "'\n";

            if (var.empty()) {
                cout << "Invalid variable name.\n";
                continue;
            }

            cout << "Enter value: ";
            cout.flush();
            if (!getline(cin, val_str)) {
                //cout << "[DEBUG] getline for value failed\n";
                cout << "Input aborted.\n";
                continue;
            }
            //cout << "[DEBUG] Raw value input: '" << val_str << "'\n";
            trim(val_str);
            //cout << "[DEBUG] Trimmed value: '" << val_str << "'\n";

            if (val_str.empty()) {
                cout << "Invalid value.\n";
                continue;
            }

            try {
                int val = stoi(val_str);

                // Safely store variable
                {
                    lock_guard<mutex> lk(p->mtx);
                    p->vars[var] = static_cast<uint16_t>(val);
                }

                // Log (this function already locks its own mutex)
                add_log(p, "Declared " + var + " = " + to_string(val));

                // Safely append to code lines
                {
                    lock_guard<mutex> lk(p->mtx);
                    ostringstream linebuf;
                    linebuf << "DECLARE:        uint16_t " << var << " = " << val << ";";
                    p->code.lines.push_back(linebuf.str());
                }

                cout << "Variable '" << var << "' = " << val << " declared successfully." << endl;
            }
            catch (const exception &e) {
                cout << "Invalid value: must be an integer. (" << e.what() << ")\n";
            }

        } else if (cmd == "print") {
            // allow "print hello" OR interactive
            string rest;
            if (!getline(ss, rest) || rest.find_first_not_of(" \t\r\n") == string::npos) {
                cout << "Enter message to PRINT: " << flush;
                if (!getline(cin, rest)) break;
            }
            // trim leading spaces
            size_t s = rest.find_first_not_of(" \t\r\n");
            if (s != string::npos) rest = rest.substr(s);
            add_log(p, string("PRINT:       ") + rest);
            ostringstream linebuf;
            linebuf << "PRINT:      " << rest;
            lock_guard<mutex> lk(p->mtx);
            p->code.lines.push_back(linebuf.str());
            cout << "Printed message logged." << endl;

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
                cout << "Slept " << t << " ms." << endl;
            } catch (...) {
                cout << "Invalid number." << endl;
            }
        } else if (cmd == "for") {
            // simple interactive for-loop emulation
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
                return;
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

            // Update variables and code under one lock
            {
                lock_guard<mutex> lk(p->mtx);
                p->vars[var1] = result;

                ostringstream linebuf;
                linebuf << (cmd=="add"?"ADD":"SUB") << ": "
                        << var1 << " = " << var2 << " "
                        << (cmd=="add"?"+":"-") << " "
                        << var3 << " -> " << result;
                p->code.lines.push_back(linebuf.str());
            }

            // Add log AFTER unlocking to avoid recursive locking
            add_log(p, (cmd == "add" ? "ADD:        " : "SUB:       ") + 
                    var1 + " = " + var2 + 
                    (cmd=="add"?" + ":" - ") + 
                    var3 + " -> " + to_string(result));

            cout << (cmd=="add"?"Added":"Subtracted") << " successfully. "
                << var1 << " = " << result << endl;
        }

        else {
            cout << "Unknown command inside screen. Available: process-smi, exit, declare, add, sub, print, sleep, for" << endl;
        }
    }

    if (scheduler && !p->finished) {
        scheduler->add_process(p);
        cout << "[Info] Process " << p->name << " added to scheduler queue.\n";
    }

    clear_console();
}

//Main menu loop
static void run_main_menu() {
     string command;

    cout << "Welcome to CSOPESY!" <<  endl;
    cout << "Version Date: November 3, 2025" <<  endl <<  endl;
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
                ss >> pname;
                if (pname.empty()) {
                     cout << "Usage: screen -s <process_name>" <<  endl;
                } else {
                    create_process(pname);
                    run_process_screen(pname);
                }
            } else if (opt == "-r") {
                 string pname;
                ss >> pname;
                if (pname.empty()) {
                     cout << "Usage: screen -r <process_name>" <<  endl;
                } else {
                    run_process_screen(pname);
                }
            } else if (opt == "-ls") {
                print_summary( cout);
            } else {
                 cout << "screen commands: -s <name> (create+attach), -r <name> (attach), -ls (list)" <<  endl;
            }
            continue;
        }

        if (root == "scheduler-start") {
            if (scheduler_running.load()) {
                cout << "Scheduler already running." << endl;
            } else {
                scheduler_running.store(true);
                // scheduler_thread = thread([](){ scheduler_loop(500); });  // temporarily disabled to prevent input freezing
                //cout << "Scheduler started (simulated)." << endl;
                if (scheduler) scheduler->start();
                cout << "Scheduler started." << endl;
            }
            continue;
        }

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

         cout << "Unknown command. Available: initialize, exit, screen, scheduler-start, scheduler-stop, report-util" <<  endl;
    }
}

int main() {
    //May init dito for config and scheduler

    run_main_menu();
    return 0;
}
