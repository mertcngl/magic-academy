# Magic Academy: Distributed Multi-Client Simulation Engine

A distributed, client-server simulation and network game engine implemented in C (POSIX C11). Built as a systems programming project for **CSE 344 Systems Programming** at **Gebze Technical University (GTU)**.

The system simulates a magical academy with concurrent participants interacting over TCP sockets. The central server (`hogwarts`) coordinates resources, state, and permissions between multiple independent client instances acting as either `WIZARD` or `PROFESSOR`.

---

## Architecture Overview

```
                      +-----------------------------+
                      |       hogwarts (Server)     |
                      |  - Single-threaded event    |
                      |    loop with select()       |
                      |  - Central inventory state  |
                      |  - Idle timeout enforcement |
                      |  - POSIX signal handling    |
                      +--------------+--------------+
                                     |
              +----------------------+----------------------+
              |                                             |
    [TCP Socket Connection]                       [TCP Socket Connection]
              |                                             |
              v                                             v
     +-----------------+                           +-------------------+
     | wizard (Client) |                           | professor (Client)|
     | - Autonomous or |                           | - Course and      |
     |   interactive   |                           |   resource        |
     | - BREW/CONSUME  |                           |   monitoring      |
     | - SPELLBOOK inv |                           | - Inventory audit |
     +-----------------+                           +-------------------+
```

### Key Architectural Characteristics
- **Event-Driven I/O Multiplexing:** Built entirely on a single-threaded `select()` event loop. Does not rely on thread-per-client or fork-per-client models, preventing race conditions and thread synchronization overhead.
- **Robust Wire Protocol:** Line-delimited ASCII protocol (`\n`) designed for zero-copy and streaming scenarios, featuring partial-read accumulators (`line_buf`) and overlong-line protection against buffer overflow exploits.
- **Network Resilience:** Partial-write mitigation via `send_all()` helper routines, ensuring atomic transmission even when interrupted by signals (`EINTR`) or TCP window limits.
- **Graceful Lifecycle Management:** Comprehensive POSIX signal interception (`SIGINT`, `SIGTERM`), ensuring atomic state logging, client notification, and socket cleanup.

---

## Wire Protocol Specification

Clients interact with the server via structured commands:

| Command | Allowed Roles | Description | Response |
| :--- | :--- | :--- | :--- |
| `ENROLL <role> <user>` | Unauthenticated | Registers a unique username under a given role (`WIZARD` or `PROFESSOR`). | `OK ENROLL <user>` / `ERR <reason>` |
| `BREW <ing> <qty>` | `WIZARD` | Crafts an item, augmenting stock and tracking personal mastery in the spellbook. | `OK BREW <ing> <delta> <total>` |
| `CONSUME <ing> <qty>` | `WIZARD` | Expends available ingredients to cast spells or execute tasks. | `OK CONSUME <ing> <delta> <total>` |
| `SPELLBOOK` | `WIZARD` | Returns the current inventory and crafted quantities for the requesting wizard. | `OK SPELLBOOK <items>` |
| `INSPECT <ing>` | Any | Queries the real-time server inventory for a specific ingredient. | `OK INSPECT <ing> <stock>` |
| `SCROLL` | Any | Queries a comprehensive catalog of all known items and current reserves. | `OK SCROLL <catalog>` |
| `APPARATE` | Any | Gracefully terminates session and disconnects socket. | `OK APPARATE` |

---

## Module Breakdown

- **`hogwarts.c`**: Central engine and TCP server managing connected client file descriptors, ingredient reserves, command validation, and logging.
- **`wizard.c`**: Participant client automating or prompting interactions for potion brewing, spellcasting, and inventory tracking.
- **`professor.c`**: Instructor and supervisor client managing resource validation, audits, and curriculum pacing.
- **`shared.h`**: Common protocol limits, role declarations, and signal-safe socket transfer routines (`send_all`).
- **`ingredients.txt`**: Initial seed data specifying baseline magical resources and initial quantities.
- **`report.tex`**: Comprehensive academic report detailing design choices, protocol formalization, and performance benchmarks.

---

## Compilation and Execution

### Prerequisites
- GCC supporting `-std=c11`
- POSIX-compliant environment (Linux, macOS, or WSL on Windows)

### Building
Compile all binaries with strict warning flags (`-Wall -Wextra`):
```bash
make
```

### Running the Server
```bash
./hogwarts -p 8080 -s ingredients.txt -l server.log -n 50 -t 300
```
- `-p`: TCP port to listen on.
- `-s`: Path to initial ingredients seed file.
- `-l`: Path for detailed runtime log outputs.
- `-n`: Maximum concurrent client connections.
- `-t`: Idle timeout in seconds before unauthenticated/dormant connections are closed.

### Running Clients
Connect a wizard:
```bash
./wizard -p 8080 -u HarryPotter
```

Connect a professor:
```bash
./professor -p 8080 -u Dumbledore
```

---

## Author

- **Author:** Mert Canoğlu
- **Institution:** Gebze Technical University (GTU)
- **Department:** Computer Engineering
- **GitHub:** [@mertcngl](https://github.com/mertcngl)
