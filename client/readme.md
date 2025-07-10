# Epoll-Based FTP-like Client

This project is the client-side application for the Epoll-Based Server. It is a single-threaded, event-driven terminal application that uses `epoll` to handle I/O multiplexing. This design allows it to remain responsive to user input while simultaneously handling network communication, including large file transfers, without freezing the user interface.

## Key Features

-   **Asynchronous I/O**: Uses `epoll` to monitor both user input (`stdin`) and the server socket concurrently.
-   **Non-Blocking UI**: The terminal remains fully responsive during uploads and downloads.
-   **Stateful Transfers**: Implements a robust state machine to manage command mode, file uploads, and file downloads.
-   **Full Command Set**: Supports all server commands, including `get`, `send`, `ls`, `cd`, etc.
-   **Progress Indicators**: Displays a real-time progress bar for both uploads and downloads.
-   **Error Handling**: Includes timeouts for stalled downloads and validation for file transfer integrity.

## How to Compile and Run

### Compilation

You will need a C compiler like `gcc`. Compile all client-side source files together. Note that `epoll_client.c` contains a `main` function for the epoll client, so you should not link `client.c`'s `main` at the same time.

```sh
# Compile the epoll-based client
gcc -o client epoll_client.c connection.c -Wall -Wextra -O2
```

### Running the Client

Execute the compiled binary. By default, it connects to `127.0.0.1` on port `8080`.

```sh
# Connect to the default server (127.0.0.1:8080)
./epollClient

# Note: To connect to a different IP/port, you would need to modify
# the main() function in epoll_client.c and recompile.
```

---

## Client Architecture

The client's design prioritizes responsiveness and efficient network handling.

### 1. Epoll Event Loop

The core is an `epoll_wait()` loop that monitors two file descriptors for I/O readiness:
-   **`STDIN_FILENO`**: For user input from the terminal.
-   **`sock`**: The TCP socket connected to the server, for both incoming data (`EPOLLIN`) and readiness to send data (`EPOLLOUT`).

### 2. Non-Blocking I/O

Both `stdin` and the server socket are set to non-blocking mode. This is critical for preventing the UI from locking up while waiting for user input or network data.

### 3. State Machine

The client uses a state machine (`client_state_t`) to manage its current operation:
-   **`STATE_COMMAND`**: The default state. The client is ready to accept user commands and process text-based responses from the server.
-   **`STATE_SENDING`**: The client is actively uploading a file. It listens for `EPOLLOUT` events to know when the network buffer is ready for the next file chunk.
-   **`STATE_RECEIVING`**: The client is actively downloading a file. It processes all incoming data as part of a binary file stream.

---

## Communication Protocols (Client Perspective)

The client implements the same protocols as the server, acting as the peer in all transactions.

### 1. Command Protocol (Text-based)

The client sends newline-terminated ASCII commands (e.g., `list\n`) and reads newline-terminated ASCII responses from the server, which are then printed to the console. Responses are color-coded for clarity (`ERROR:`, `SUCCESS:`, `OK:`).

### 2. File Transfer Protocol (Binary)

The client uses the same `[Header][Payload]` chunk structure as the server. All multi-byte integer fields in the header are converted to network byte order (`htonl`) before sending.

#### Upload Flow (`send <filename>`)

1.  User enters the `send <filename>` command.
2.  The client sends the text command `upload\n` to the server.
3.  It transitions to the `STATE_SENDING` state and registers for `EPOLLOUT` events on the socket.
4.  When `epoll` indicates the socket is ready for writing, the client reads a chunk from the local file, prepares the header, and sends the `[Header][Payload]` pair.
5.  This repeats until the file is fully sent, with a progress bar updating in the terminal.

#### Download Flow (`get <filename>`)

1.  User enters the `get <filename>` command.
2.  The client sends the text command `get <filename>\n` to the server.
3.  The client's `EPOLLIN` handler inspects incoming data:
    -   If it's a text response (e.g., `ERROR: File not found\n`), it's printed normally.
    -   **If the data heuristically matches a `FileChunkHeader`**, the client transitions to `STATE_RECEIVING`.
4.  In the `RECEIVING` state, a stateful handler reassembles headers and payloads from the TCP stream, writes the data to a local file, and updates a progress bar.
5.  The download is complete when the number of received chunks matches the `total_chunks` value from the first header.

---

## Client Command Reference

| Command                         | Description                                                              |
| ------------------------------- | ------------------------------------------------------------------------ |
| `get <filename>`                | Downloads a file from the server.                                        |
| `send <filename>`               | Uploads a local file to the server.                                      |
| `list`                          | Lists files on the server (sends `ls`).                                  |
| `pwd`                           | Shows the current directory on the server.                               |
| `cd <directory>`                | Changes directory on the server.                                         |
| `delete <filename>`             | Deletes a file on the server.                                            |
| `health`                        | Retrieves a system health report from the server.                        |
| `help`                          | Displays the list of available commands.                                 |
| `clear`                         | Clears the terminal screen.                                              |
| `exit`                          | Disconnects from the server and closes the client.                       |

---

## Error Handling

-   **Server Disconnection**: `epoll` detects a closed connection, prompting the client to shut down gracefully.
-   **Download Timeout**: If no data is received for an extended period during a download, the client aborts the transfer to prevent an indefinite stall.
-   **File Errors**: If a local file for an upload cannot be opened, an error is printed, and the transfer is aborted.
-   **Protocol Errors**: The download handler validates chunk sizes and sequence numbers to detect and abort corrupt transfers.