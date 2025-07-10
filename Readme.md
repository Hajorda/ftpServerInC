
# Epoll-based FTP Server & Client

This project implements a high-performance, single-threaded, non-blocking FTP-like server and a corresponding interactive client using Linux's `epoll` API. The entire application is written in C and is designed to handle multiple concurrent connections efficiently without the overhead of a thread-per-client model.


![Terminal Demo](https://raw.githubusercontent.com/Hajorda/ftpServerInC/refs/heads/main/server/saved/terminal.gif)

## Key Features

-   **High Concurrency**: The server uses `epoll` to manage thousands of simultaneous connections within a single thread.
-   **Fully Asynchronous I/O**: Both server and client use non-blocking sockets to prevent I/O operations from stalling the application.
-   **Responsive Client UI**: The client's terminal remains fully interactive during large file transfers thanks to its `epoll`-based design that monitors both user input and network sockets.
-   **Robust File Transfers**: A custom chunk-based binary protocol ensures reliable transfers of large files, complete with progress bars on the client-side.
-   **Core FTP Functionality**: Supports file uploads, downloads, listing, deletion, renaming, and directory navigation.
-   **System Health Monitoring**: The server provides a `health` command to report its CPU, RAM, disk usage, and uptime.

---

## Getting Started

### Prerequisites

-   A Linux-based operating system (for `epoll`).
-   `gcc` (or another C compiler).
-   `make` (optional, but recommended).

### Compilation

You can compile the server and client using the provided `gcc` commands.

#### Server

```sh
# Compile the server
gcc -o server main_epoll.c epoll_server.c commands.c -Wall -Wextra -O2
```

#### Client

```sh
# Compile the client
gcc -o client epoll_client.c connection.c -Wall -Wextra -O2
```

### Running the Application

1.  **Start the Server**:
    Open a terminal and run the compiled `server` executable. You can optionally specify a port number.

    ```sh
    # Run on the default port 8080
    ./server

    # Run on a custom port (e.g., 9090)
    ./server 9090
    ```

2.  **Start the Client**:
    Open another terminal and run the `client` executable. It will attempt to connect to the server.

    ```sh
sh
    # Connect to the default server (127.0.0.1:8080)
    ./client
    ```

    Once connected, you will see the `ftp>` prompt. Type `help` to see a list of available commands.

---

## Architecture Deep Dive

The project's high performance and responsiveness stem from its event-driven, non-blocking architecture.

### Server Architecture

-   **Epoll Event Loop**: The server's core is a single `while(1)` loop that calls `epoll_wait()`. This allows it to monitor the main listening socket for new connections and all client sockets for incoming data without blocking.
-   **Non-Blocking Sockets**: All sockets are set to non-blocking mode. When `accept()` or `recv()` is called, it returns immediately, even if there's nothing to do. This prevents a single slow client from stalling the entire server.
-   **State Management**: The server maintains a state for each connected client. This is crucial for handling partial reads (a single `recv` may not get a full command or file chunk) and for tracking whether a client is in command mode or file transfer mode.

### Client Architecture

-   **Multiplexed I/O**: The client also uses `epoll` to monitor two different file descriptors at once:
    1.  `STDIN_FILENO`: User input from the keyboard.
    2.  `sock`: The TCP socket connected to the server.
-   **Non-Blocking UI**: Because `stdin` is non-blocking, the client can check for user input without halting. This allows it to process incoming network data (like a file download) and update the UI (like a progress bar) while still being ready for the user to type a command.
-   **State Machine**: The client operates in one of three states:
    1.  `STATE_COMMAND`: Default state for sending and receiving text commands.
    2.  `STATE_SENDING`: Active during a file upload.
    3.  `STATE_RECEIVING`: Active during a file download.

---

## Communication Protocol

A custom protocol is used for all communication, with two distinct modes.

### 1. Command Protocol (Text)

-   Commands are simple, newline-terminated ASCII strings (e.g., `ls\n`).
-   Responses are also newline-terminated text, often prefixed with `SUCCESS:`, `ERROR:`, or `OK:`.

### 2. File Transfer Protocol (Binary)

For `get` and `send` commands, the protocol switches to a binary mode to transfer file data efficiently. Files are broken into chunks, and each chunk is sent as a `[Header][Payload]` pair.

#### FileChunkHeader

This fixed-size binary header precedes every data payload. All multi-byte integers **must** be in network byte order (`htonl`/`ntohl`).

```c
typedef struct {
    uint32_t chunk_id;        // 0-indexed sequence number of the chunk.
    uint32_t chunk_size;      // Size of the payload that follows this header.
    uint32_t total_chunks;    // The total number of chunks for the entire file.
    uint32_t type;            // Reserved for future use (currently 0).
    char filename[64];        // The name of the file being transferred.
} FileChunkHeader;
```

#### Transfer Flow

-   **Upload (`send` command)**:
    1.  Client sends the text command `upload\n`.
    2.  Server receives this and switches the client's state to file-transfer mode.
    3.  Client begins sending a stream of `[Header][Payload]` chunks.
    4.  Server receives the chunks and writes them to a file in its `saved/` directory.
    5.  Once all chunks are received, the server sends a `SUCCESS: File uploaded\n` message.

-   **Download (`get` command)**:
    1.  Client sends the text command `get <filename>\n`.
    2.  Server validates the request. If the file exists, it begins sending a stream of `[Header][Payload]` chunks.
    3.  The client detects the incoming binary stream, switches to receiving state, and writes the incoming data to a local file.

---

## Command Reference

| Command                         | Description & Arguments                                                              | Example Usage              |
| ------------------------------- | ------------------------------------------------------------------------------------ | -------------------------- |
| `list`                          | Lists files on the server (sends `ls`).                                              | `list`                     |
| `get <filename>`                | Downloads a file from the server.                                                    | `get my_document.txt`      |
| `send <filename>`               | Uploads a local file to the server.                                                  | `send report.pdf`          |
| `pwd`                           | Shows the current working directory on the server.                                   | `pwd`                      |
| `cd <directory>`                | Changes the server's current working directory.                                      | `cd /tmp/data`             |
| `delete <filename>`             | Deletes a file on the server.                                                        | `delete old_file.log`      |
| `rename <old> <new>`            | Renames a file on the server.                                                        | `rename file.v1 file.v2`   |
| `health`                        | Retrieves a system health report from the server.                                    | `health`                   |
| `help`                          | Displays the list of available client commands.                                      | `help`                     |
| `clear`                         | Clears the client's terminal screen.                                                 | `clear`                    |
| `exit`                          | Disconnects from the server and closes the client.                                   | `exit`                     |
