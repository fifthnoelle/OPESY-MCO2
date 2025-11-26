# OPESY-OS-Emulator

This project is a **simulated Operating System process scheduler and interpreter**, supporting:
- Multi-core scheduling
- FCFS (First-Come, First-Served) and RR (Round Robin) algorithms
- Basic process scripting (DECLARE, PRINT, FOR loops, etc.)
- A CLI-based “root shell” with screen attachment and per-process logging

Each “process” runs its own instruction script while the scheduler dispatches them to simulated CPU cores based on the active scheduling algorithm.