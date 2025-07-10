#ifndef EPOLL_CLIENT_H
#define EPOLL_CLIENT_H

/**
 * @file epoll_client.h
 * @brief Epoll-based Non-Blocking FTP-like Client
 *
 * =====================================================================================
 *                                  README
 * =====================================================================================
 *
 * This document explains the architecture, protocols, and functionality of the epoll-based client.
 *
 *
 * I. CLIENT ARCHITECTURE
 * ----------------------
 * This client is a single-threaded, event-driven application that uses `epoll` to handle
 * I/O multiplexing. This design allows it to remain responsive to user input while
 * simultaneously handling network communication, including large file transfers, without
 * freezing.
 *
 * Key components:
 * 1.  **Epoll Event Loop**: The core is an `epoll_wait()` loop that monitors two file
 *     descriptors for I/O readiness:
 *     - `STDIN_FILENO`: For user input from the terminal.
 *     - `sock`: The TCP socket connected to the server, for both incoming data (`EPOLLIN`)
 *       and readiness to send data (`EPOLLOUT`).
 *
 * 2.  **Non-Blocking I/O**: Both `STDIN_FILENO` and the server socket are set to non-blocking
 *     mode. This is critical:
 *     - `read()` from stdin returns immediately, even if the user hasn't typed anything.
 *     - `recv()` from the socket returns immediately, preventing the UI from locking up while
 *       waiting for a server response.
 *     - `send()` also returns immediately. If the network buffer is full, it returns
 *       `EAGAIN`/`EWOULDBLOCK` instead of waiting.
 *
 * 3.  **State Machine**: The client uses a state machine (`client_state_t`) to manage its
 *     current operation. This is essential for a non-blocking design. The states are:
 *     - `STATE_COMMAND`: The default state. The client is ready to accept user commands and
 *       process text-based responses from the server.
 *     - `STATE_SENDING`: The client is actively uploading a file. In this state, it listens
 *       for `EPOLLOUT` events to know when it can send the next file chunk.
 *     - `STATE_RECEIVING`: The client is actively downloading a file. It processes all
 *       incoming data as part of a binary file stream.
 *
 *
 * II. COMMUNICATION PROTOCOLS (Client Perspective)
 * ------------------------------------------------
 * The client implements the same protocols as the server.
 *
 *   A. COMMAND PROTOCOL (Text-based)
 *   --------------------------------
 *   -   The client sends newline-terminated ASCII commands (e.g., `list\n`).
 *   -   It reads newline-terminated ASCII responses from the server and prints them to the
 *       console. Responses starting with `ERROR:`, `SUCCESS:`, or `OK:` are color-coded.
 *
 *   B. FILE TRANSFER PROTOCOL (Binary)
 *   ---------------------------------
 *   The client uses the same `[Header][Payload]` chunk structure as the server. All integer
 *   fields in the header are converted to network byte order (`htonl`) before sending.
 *
 *   -   **Upload Flow (`send <filename>`)**:
 *       1.  The user issues the `send <filename>` command.
 *       2.  The client first sends the text command `upload\n` to the server to signal the
 *           start of a transfer.
 *       3.  It transitions to the `STATE_SENDING` state.
 *       4.  It modifies its `epoll` registration for the socket to include the `EPOLLOUT` flag.
 *       5.  The `epoll` loop will now trigger an `EPOLLOUT` event when the socket's send buffer
 *           has space.
 *       6.  The event handler calls `send_file_chunk_epoll()`, which reads a chunk from the
 *           local file, prepares the header, and sends the `[Header][Payload]` pair.
 *       7.  This process repeats until all chunks are sent. The client displays a progress bar.
 *       8.  Upon completion, the client removes the `EPOLLOUT` flag and transitions back to
 *           `STATE_COMMAND`.
 *
 *   -   **Download Flow (`get <filename>`)**:
 *       1.  The user issues the `get <filename>` command.
 *       2.  The client sends the text command `get <filename>\n` to the server. It remains
 *           in `STATE_COMMAND`.
 *       3.  The client waits for data from the server.
 *       4.  The `EPOLLIN` event handler inspects the first few bytes of incoming data.
 *           - If it's a text response (e.g., `ERROR: File not found\n`), it's printed normally.
 *           - **If the data heuristically matches the structure of a `FileChunkHeader`**, the
 *             client assumes a file transfer is beginning.
 *       5.  The client transitions to `STATE_RECEIVING`.
 *       6.  The `receive_file_chunk_epoll()` function is called for all subsequent `EPOLLIN` events.
 *           This function is stateful and designed to reassemble headers and payloads from
 *           potentially fragmented TCP packets.
 *       7.  It opens a local file for writing and writes each received payload to it. A
 *           progress bar is displayed.
 *       8.  When the number of received chunks matches `total_chunks` from the first header,
 *           the download is complete. The client closes the file and transitions back to
 *           `STATE_COMMAND`.
 *
 *
 * III. CLIENT COMMAND REFERENCE
 * -----------------------------
 * - `get <filename>`: Downloads a file from the server.
 * - `send <filename>`: Uploads a local file to the server.
 * - `list`: Lists files on the server (sends `ls`).
 * - `pwd`: Shows the current directory on the server.
 * - `cd <directory>`: Changes directory on the server.
 * - `delete <filename>`: Deletes a file on the server.
 * - `health`: Retrieves a system health report from the server.
 * - `help`: Displays this list of commands.
 * - `clear`: Clears the terminal screen.
 * - `exit`: Disconnects from the server and closes the client.
 *
 *
 * IV. ERROR HANDLING
 * ------------------
 * - **Server Disconnection**: `epoll` detects a closed connection via `EPOLLHUP` or `EPOLLERR`,
 *   prompting the client to shut down gracefully.
 * - **Download Timeout**: During file downloads, `epoll_wait` uses a timeout. If no data is
 *   received for an extended period, the client assumes the download has stalled, prints an
 *   error, and aborts the transfer.
 * - **File Errors**: If a local file for an upload cannot be opened, an error is printed, and
 *   the transfer is aborted.
 * - **Protocol Errors**: The download handler includes validation to check for invalid chunk
 *   sizes or out-of-sequence chunks, which will abort a corrupt transfer.
 */

// Function declarations for epoll client
int start_epoll_client(const char *server_ip, int server_port);
int set_nonblocking(int socket_fd);
int set_stdin_nonblocking();
void restore_stdin_blocking();
void show_help();

#endif