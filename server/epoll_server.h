#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include <sys/socket.h>

/**
 * @file epoll_server.h
 * @brief Epoll-based Non-Blocking FTP-like Server
 *
 * =====================================================================================
 *                                  README
 * =====================================================================================
 *
 * This document explains the architecture, protocols, and functionality of the epoll-based server.
 *
 *
 * I. SERVER ARCHITECTURE
 * ----------------------
 * This server is a high-performance, I/O-multiplexed, single-threaded application
 * designed to handle multiple concurrent clients efficiently.
 *
 * Key components:
 * 1.  **Epoll Event Loop**: The core of the server is a main loop built around `epoll_wait()`.
 *     This allows the server to monitor a large number of file descriptors (sockets) for
 *     I/O events (e.g., new connections, incoming data) without blocking or needing a
 *     thread per client.
 *
 * 2.  **Non-Blocking Sockets**: All sockets (both the main listening socket and all client
 *     sockets) are set to non-blocking mode using `fcntl(fd, F_SETFL, O_NONBLOCK)`.
 *     This ensures that I/O calls like `accept()`, `recv()`, and `send()` return
 *     immediately instead of waiting. If an operation cannot be completed, they return -1
 *     with `errno` set to `EAGAIN` or `EWOULDBLOCK`.
 *
 * 3.  **State Management**: To handle non-blocking I/O correctly, the server maintains a
 *     state for each client in a `client_info_t` struct. This is crucial because a single
 *     `recv()` call is not guaranteed to read a complete command or an entire file chunk.
 *     The state tracks:
 *     - Partially received commands in a text buffer.
 *     - The client's current mode (`state`): 0 for command mode, 1 for file transfer mode.
 *     - Partially received binary file transfer chunks, including the header and payload.
 *
 * 4.  **Single-Threaded Model**: By leveraging `epoll`, the server can manage all clients
 *     within a single thread. This avoids the complexity and overhead of multi-threading
 *     (like locks and context switching) while remaining highly concurrent.
 *
 *
 * II. COMMUNICATION PROTOCOLS
 * ---------------------------
 * The server uses two distinct protocols: one for text-based commands and one for
 * binary file transfers.
 *
 *   A. COMMAND PROTOCOL (Text-based)
 *   --------------------------------
 *   -   **Format**: Commands are simple ASCII strings terminated by a newline character (`\n`).
 *       The server also handles optional carriage returns (`\r\n`).
 *   -   **Interaction**: The client sends a command. The server processes it and sends back a
 *       human-readable, newline-terminated response.
 *   -   **Responses**: Server responses typically start with a status indicator:
 *       - `SUCCESS:`: The requested operation completed successfully.
 *       - `ERROR:`: An error occurred. The message provides details.
 *       - `OK:`: A simple acknowledgment of success (e.g., for `cd`).
 *       - For commands like `ls` or `pwd`, the data is sent directly, followed by a
 *         terminator (`END_OF_LIST\n` for `ls`).
 *
 *   B. FILE TRANSFER PROTOCOL (Binary)
 *   ---------------------------------
 *   File transfers are designed to be robust and handle large files efficiently. The file is
 *   broken down into smaller chunks, and each chunk is sent with a header.
 *
 *   -   **Structure**: Each message is a chunk composed of `[Header][Payload]`.
 *   -   **`FileChunkHeader`**: A fixed-size binary header precedes each data payload.
 *       All integer fields MUST be converted to network byte order (`htonl`) by the sender
 *       and converted back to host byte order (`ntohl`) by the receiver.
 *
 *       The header struct is defined as:
 *       typedef struct {
 *           uint32_t chunk_id;        // 0-indexed sequence number of the chunk.
 *           uint32_t chunk_size;      // Size of the payload that follows this header.
 *           uint32_t total_chunks;    // The total number of chunks for the entire file.
 *           uint32_t type;            // Reserved for future use (currently 0).
 *           char filename[64];        // The name of the file being transferred.
 *       } FileChunkHeader;
 *
 *   -   **Upload Flow (`upload` command)**:
 *       1.  Client sends the text command: `upload\n`
 *       2.  The server receives this, acknowledges nothing back, but switches the client's
 *           internal state to file transfer mode (`state = 1`).
 *       3.  The client immediately begins sending the file as a stream of binary chunks
 *           (`Header` + `Payload`, `Header` + `Payload`, ...).
 *       4.  The server reads the incoming binary stream. Because of non-blocking I/O, it
 *           may receive partial headers or payloads, which it buffers in the client's
 *           state struct (`recv_buffer`) until a full chunk is processed.
 *       5.  On receiving the first chunk (`chunk_id == 0`), the server creates the file
 *           in the `saved/` directory.
 *       6.  After the server has received and written `total_chunks`, it sends a final
 *           text confirmation: `SUCCESS: File uploaded\n`.
 *       7.  The server then resets the client's state back to command mode (`state = 0`).
 *
 *   -   **Download Flow (`get` command)**:
 *       1.  Client sends the text command: `get <filename>\n`
 *       2.  The server attempts to open the requested file.
 *           - If not found, it sends: `ERROR: File not found\n`.
 *           - If found, it calculates the number of chunks and begins sending the file
 *             as a stream of binary chunks (`Header` + `Payload`, ...).
 *       3.  The client is responsible for reading this stream, parsing the headers, and
 *           reassembling the file. The client knows the transfer is complete when it
 *           has received `total_chunks`.
 *       4.  The server sends no final "success" message; the completion is implicit.
 *
 *
 * III. COMMAND REFERENCE
 * ----------------------
 * All commands are case-sensitive.
 *
 * - `ls`
 *   - **Description**: Lists files and directories in the server's current working directory.
 *   - **Arguments**: None.
 *   - **Response**: A series of lines, each detailing a file or directory. The list is
 *     terminated by the line `END_OF_LIST\n`.
 *   - **Error**: `ERROR: Cannot list directory\n` if `opendir()` fails.
 *
 * - `get <filename>`
 *   - **Description**: Requests a file from the server.
 *   - **Arguments**: `filename` - The name of the file to download.
 *   - **Response**: The server begins a binary file transfer (see protocol above).
 *   - **Error**: `ERROR: File not found\n` if the file cannot be opened for reading.
 *
 * - `upload`
 *   - **Description**: Informs the server that the client is about to send a file.
 *   - **Arguments**: None. The filename is sent in the first file chunk header.
 *   - **Response**: No immediate response. The server switches to file transfer mode to
 *     receive binary data. A success/error message is sent after the transfer ends.
 *   - **Error**: `ERROR: Cannot create file\n` if the server cannot write the file (e.g.,
 *     permissions). `ERROR: File transfer failed\n` for other transfer issues.
 *
 * - `pwd`
 *   - **Description**: Prints the server's current working directory.
 *   - **Arguments**: None.
 *   - **Response**: The absolute path of the current directory, followed by a newline.
 *   - **Error**: `ERROR: Cannot get current directory\n`.
 *
 * - `cd <path>`
 *   - **Description**: Changes the server's current working directory.
 *   - **Arguments**: `path` - The relative or absolute path to change to.
 *   - **Response**: `OK: Directory changed\n`.
 *   - **Error**: `ERROR: Cannot change directory\n`.
 *
 * - `delete <filename>`
 *   - **Description**: Deletes a file on the server.
 *   - **Arguments**: `filename` - The name of the file to delete.
 *   - **Response**: `SUCCESS: File deleted\n`.
 *   - **Error**: `ERROR: Cannot delete file\n`.
 *
 * - `health`
 *   - **Description**: Retrieves a system health and status report from the server.
 *   - **Arguments**: None.
 *   - **Response**: A multi-line string containing CPU usage/temp, disk usage, RAM usage,
 *     and system uptime.
 *
 *
 * IV. ERROR HANDLING & DISCONNECTION   
 * ----------------------------------
 * - **Command Errors**: Invalid commands or operations that fail (e.g., `cd` to a non-existent
 *   directory) result in an `ERROR:` message sent to the client. The connection remains open.
 *
 * - **Buffer Overflow**: If a client sends a command line that is too long, the server
 *   sends an error message and forcefully disconnects the client to protect itself.
 *
 * - **Client Disconnection**: The server detects a client disconnection when `recv()` returns 0
 *   (graceful shutdown) or -1 with an error other than `EAGAIN`. The `epoll` loop also
 *   detects this via `EPOLLHUP` or `EPOLLERR` flags. When a client disconnects, the server:
 *   1. Closes the client's socket file descriptor.
 *   2. Removes the file descriptor from the `epoll` watch list.
 *   3. Frees the `client_info_t` struct associated with that client.
 *   4. If the client was in the middle of a file upload, the partially written file is
 *      closed and left in a partial state.
 */

// Function declarations for epoll server
void start_epoll_server(int port);
int set_nonblocking(int socket_fd);
int handle_new_connection(int server_fd, int epoll_fd);
int handle_client_data(int client_fd);
void process_client_command(int sock, const char *command);

// Client info structure (Simplified here for header; full definition in .c file)
// This struct is essential for state management in a non-blocking server.
typedef struct
{
    int socket_fd;
    char buffer[1024];
    int buffer_len;
    int state; // 0 = command mode, 1 = file transfer mode
    char client_ip[16];
    // Additional state for file transfers is managed in the implementation file.
} client_info_t;

#endif