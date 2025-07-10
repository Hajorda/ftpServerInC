
# Epoll-Based FTP-like Server

This project implements a high-performance, single-threaded, non-blocking FTP-like server using Linux's `epoll` API for I/O multiplexing. It is designed to handle multiple concurrent clients efficiently without the overhead of a thread-per-client model.

## Key Features

-   **High Concurrency**: Manages thousands of connections in a single thread using `epoll`.
-   **Non-Blocking I/O**: All socket operations are non-blocking to prevent the server from stalling.
-   **File Operations**: Supports file uploads, downloads, deletion, and renaming.
-   **Directory Navigation**: Allows clients to list directory contents (`ls`), print the current directory (`pwd`), and change directories (`cd`).
-   **Robust File Transfer**: Uses a custom chunk-based binary protocol to handle large files reliably.
-   **System Health Monitoring**: Provides a `health` command to check the server's CPU, RAM, and disk status.

## How to Compile and Run

### Compilation

You will need a C compiler like `gcc`. Compile all source files together:

```sh
gcc -o server main_epoll.c epoll_server.c commands.c -Wall -Wextra -O2
```

### Running the Server

Execute the compiled binary. By default, it runs on port 8080.

```sh
# Run on default port 8080
./ftp_server_epoll

# Run on a custom port (e.g., 9090)
./ftp_server_epoll 9090
```

---

## Server Architecture

The server's design is centered around efficiency and scalability.

### 1. Epoll Event Loop

The core of the server is a main loop built around `epoll_wait()`. This allows the server to monitor a large number of file descriptors (sockets) for I/O events (e.g., new connections, incoming data) without blocking or needing a thread per client.

### 2. Non-Blocking Sockets

All sockets, including the main listening socket and all client sockets, are set to non-blocking mode using `fcntl(fd, F_SETFL, O_NONBLOCK)`. This ensures that I/O calls like `accept()`, `recv()`, and `send()` return immediately. If an operation cannot be completed (e.g., no data to read), they return `-1` with `errno` set to `EAGAIN` or `EWOULDBLOCK`, allowing the event loop to continue processing other clients.

### 3. State Management

To handle non-blocking I/O, the server maintains a state for each client in a `client_info_t` struct. This is crucial because a single `recv()` call is not guaranteed to read a complete message. The state tracks:
-   Partially received commands in a text buffer.
-   The client's current mode (`state`): `0` for command mode, `1` for file transfer mode.
-   Partially received binary file transfer chunks, including the header and payload.

### 4. Single-Threaded Model

By leveraging `epoll`, the server manages all clients within a single thread. This avoids the complexity and overhead of multi-threading (like locks and context switching) while remaining highly concurrent.

---

## Communication Protocols

The server uses two distinct protocols: one for text-based commands and one for binary file transfers.

### 1. Command Protocol (Text-based)

-   **Format**: Commands are simple ASCII strings terminated by a newline character (`\n`).
-   **Interaction**: The client sends a command. The server processes it and sends back a human-readable, newline-terminated response.
-   **Responses**: Server responses typically start with a status indicator like `SUCCESS:`, `ERROR:`, or `OK:`.

### 2. File Transfer Protocol (Binary)

File transfers are designed to be robust. The file is broken down into smaller chunks, and each chunk is sent with a header.

#### FileChunkHeader
A fixed-size binary header precedes each data payload. All multi-byte integer fields **must** be in network byte order (`htonl`).

```c
typedef struct {
    uint32_t chunk_id;        // 0-indexed sequence number of the chunk.
    uint32_t chunk_size;      // Size of the payload that follows this header.
    uint32_t total_chunks;    // The total number of chunks for the entire file.
    uint32_t type;            // Reserved for future use (currently 0).
    char filename[64];        // The name of the file being transferred.
} FileChunkHeader;
```

#### Upload Flow (`upload` command)

1.  Client sends the text command: `upload\n`.
2.  The server receives this, sends no immediate response, but switches the client's internal state to file transfer mode (`state = 1`).
3.  The client immediately begins sending the file as a stream of binary chunks: `[Header][Payload]`, `[Header][Payload]`, ...
4.  On receiving the first chunk (`chunk_id == 0`), the server creates the file in the `saved/` directory.
5.  After the server has received and written `total_chunks`, it sends a final text confirmation: `SUCCESS: File uploaded\n` and switches the client back to command mode.

#### Download Flow (`get` command)

1.  Client sends the text command: `get <filename>\n`.
2.  The server attempts to open the file.
    -   If not found, it sends: `ERROR: File not found\n`.
    -   If found, it begins sending the file as a stream of binary chunks (`[Header][Payload]`, ...).
3.  The client reassembles the file and knows the transfer is complete after receiving `total_chunks`.

---

## Command Reference

| Command                         | Description & Arguments                                                                                                 | Example Client Input       |
| ------------------------------- | ----------------------------------------------------------------------------------------------------------------------- | -------------------------- |
| `ls`                            | Lists files and directories in the server's current directory.                                                          | `ls`                       |
| `get <filename>`                | Requests a file from the server. The server responds with a binary stream.                                              | `get my_document.txt`      |
| `upload`                        | Informs the server that a binary file transfer is about to begin. The filename is sent in the first chunk's header.     | `upload`                   |
| `pwd`                           | Prints the server's current working directory.                                                                          | `pwd`                      |
| `cd <path>`                     | Changes the server's current working directory.                                                                         | `cd /tmp/test_data`        |
| `delete <filename>`             | Deletes a file on the server.                                                                                           | `delete old_file.log`      |
| `rename <old_name> <new_name>`  | Renames a file on the server.                                                                                           | `rename file.v1 file.v2`   |
| `health`                        | Retrieves a system health report from the server (CPU, RAM, Disk, Uptime).                                              | `health`                   |

---

## Error Handling and Disconnection

-   **Command Errors**: Invalid commands or failed operations (e.g., `cd` to a non-existent directory) result in an `ERROR:` message sent to the client. The connection remains open.

-   **Buffer Overflow**: If a client sends a command that is too long, the server sends an error message and forcefully disconnects the client to protect itself.

-   **Client Disconnection**: The server detects disconnection when `recv()` returns `0` or `epoll` signals `EPOLLHUP`/`EPOLLERR`. When a client disconnects, the server cleans up all associated resources (socket, state object, open file handles). Any file upload in progress will be aborted.