# 🏆 Tennis Tournament Manager (OS Mini Project)

## 📌 Description
This project is a **client-server based tennis tournament system** that demonstrates core Operating Systems concepts such as concurrency, synchronization, file locking, IPC, and socket programming.

The system consists of:
- A central **server**
- Multiple **clients**:
  - Admin
  - Players
  - Viewers

---

## ⚙️ Features

- Role-based authentication (Admin / Player / Viewer)
- Real-time match execution between players
- Live score updates for viewers
- Admin-controlled match scheduling
- Concurrent multi-client handling

---

## 🧠 Concepts Implemented

### 1. Role-Based Authorization
- Separate roles: admin, player, viewer
- Access control enforced on server
- Only admin can start matches
- Only players can update scores

---

### 2. File Locking
- `fcntl()` used for:
  - **Write lock (F_WRLCK)** while updating score
  - **Read lock (F_RDLCK)** for viewers
- Prevents:
  - Dirty reads
  - Concurrent write corruption

---

### 3. Concurrency Control
- Multi-threaded server using `pthread`
- Mutexes for shared data:
  - Users
  - Sessions
- Semaphore to limit concurrent connections

---

### 4. Data Consistency
- All shared data protected via mutex
- Score updates written atomically
- Prevents:
  - Race conditions
  - Lost updates

---

### 5. Socket Programming
- TCP-based client-server model
- Server handles multiple clients simultaneously
- Communication via `send()` / `recv()`

---

### 6. Inter-Process Communication (IPC)

#### IPC Mechanism 1: Named FIFOs
- Used for **player-to-player communication**
- Handles real-time rally exchange

#### IPC Mechanism 2: Named Pipe (`/tmp/tm_score_pipe`)
- Player → Server communication
- Used for score update notifications

---

## 🛠️ Compilation

### Using Makefile
```bash
make
```
### Running the programs
```bash
./server
./admin
./player
./viewer
```