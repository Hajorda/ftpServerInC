#define _GNU_SOURCE
#include "client.h"
#include "epoll_client.h"
#include "colors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>

#define MAX_EVENTS 10
#define CHUNK_SIZE 512
#define FILENAME_MAX_LEN 64

// File transfer structures
typedef struct
{
    uint32_t chunk_id;
    uint32_t chunk_size;
    uint32_t total_chunks;
    uint32_t type;
    char filename[FILENAME_MAX_LEN];
} FileChunkHeader;

// Client state for file transfers
typedef enum
{
    STATE_COMMAND,  // command processing
    STATE_SENDING,  // Sending file to server
    STATE_RECEIVING // Receiving file from server
} client_state_t;

typedef struct
{
    client_state_t state;
    FILE *file_ptr;
    char filename[FILENAME_MAX_LEN];
    int total_chunks;
    int current_chunk;
    int bytes_written;
    long file_size;
    char file_buffer[CHUNK_SIZE];
    int buffer_pos;
    int header_received;
    FileChunkHeader current_header;
} transfer_state_t;

// Function declarations
void init_transfer_state(transfer_state_t *state);
int send_file_chunk_epoll(int sock, transfer_state_t *state);
int receive_file_chunk_epoll(int sock, transfer_state_t *state, char *buffer, int bytes_available);
int start_file_upload(int sock, const char *filename, transfer_state_t *state);
int start_file_download(int sock, const char *filename, transfer_state_t *state);
void process_user_command(int sock, const char *command, transfer_state_t *transfer_state, int epoll_fd);
void progress_bar(int percent);

// Function to set socket to non-blocking mode
int set_nonblocking(int socket_fd)
{
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl F_GETFL");
        return -1;
    }

    if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

// Function to set stdin to non-blocking mode
int set_stdin_nonblocking()
{
    return set_nonblocking(STDIN_FILENO);
}

// Function to restore stdin to blocking mode
void restore_stdin_blocking()
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1)
    {
        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
    }
}

// Non-blocking client with epoll
int start_epoll_client(const char *server_ip, int server_port)
{
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("Socket creation failed");
        return -1;
    }

    // Connect to server (blocking for initial connection)
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0)
    {
        perror("Invalid address");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connection failed");
        close(sock);
        return -1;
    }

    printf(GREEN "Connected to server %s:%d\n" RESET, server_ip, server_port);

    // Set socket and stdin to non-blocking
    if (set_nonblocking(sock) == -1)
    {
        close(sock);
        return -1;
    }

    if (set_stdin_nonblocking() == -1)
    {
        close(sock);
        return -1;
    }

    // Create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("epoll_create1");
        restore_stdin_blocking();
        close(sock);
        return -1;
    }

    // Add socket to epoll
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLOUT;
    event.data.fd = sock;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &event) == -1)
    {
        perror("epoll_ctl: socket");
        close(epoll_fd);
        restore_stdin_blocking();
        close(sock);
        return -1;
    }

    // Add stdin to epoll
    event.events = EPOLLIN;
    event.data.fd = STDIN_FILENO;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, STDIN_FILENO, &event) == -1)
    {
        perror("epoll_ctl: stdin");
        close(epoll_fd);
        restore_stdin_blocking();
        close(sock);
        return -1;
    }

    printf("ftp> ");
    fflush(stdout);

    char input_buffer[1024] = {0};
    char server_buffer[4096] = {0};
    int input_len = 0;
    int server_data_len = 0;
    int running = 1;
    // Initialize transfer state
    transfer_state_t transfer_state;
    init_transfer_state(&transfer_state);

    // Event loop
    struct epoll_event events[MAX_EVENTS];
    int no_data_iterations = 0;
    const int MAX_NO_DATA_ITERATIONS = 1000; // Timeout after many iterations without data

    while (running)
    {
        // Use a timeout for epoll_wait when receiving files
        int timeout = (transfer_state.state == STATE_RECEIVING) ? 10000 : -1; // 10 second timeout during download
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout);

        if (num_events == -1)
        {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }
        else if (num_events == 0)
        {
            // Timeout occurred
            if (transfer_state.state == STATE_RECEIVING)
            {
                no_data_iterations++;
                // For very large files, allow longer timeouts
                int max_timeouts = (transfer_state.total_chunks > 100000) ? 10 : 5; // 100 or 50 seconds
                if (no_data_iterations >= max_timeouts)
                {
                    printf(RED "\nTimeout: No data received from server for %d seconds\n" RESET, max_timeouts * 10);
                    printf(RED "Download may have stalled. Current progress: %d/%d chunks (%.1f%%)\n" RESET,
                           transfer_state.current_chunk, transfer_state.total_chunks,
                           (transfer_state.total_chunks > 0) ? ((float)transfer_state.current_chunk / transfer_state.total_chunks) * 100.0 : 0.0);
                    if (transfer_state.file_ptr)
                    {
                        fclose(transfer_state.file_ptr);
                    }
                    init_transfer_state(&transfer_state);
                    printf("ftp> ");
                    fflush(stdout);
                }
                else
                {
                    // Show progress during timeout for large files
                    if (transfer_state.total_chunks > 10000)
                    {
                        printf(".");
                        fflush(stdout);
                    }
                }
                continue;
            }
            else
            {
                continue; // Normal timeout in command mode
            }
        }
        else
        {
            // Reset timeout counter when we receive data
            no_data_iterations = 0;
        }

        for (int i = 0; i < num_events; i++)
        {
            int fd = events[i].data.fd;

            if (fd == STDIN_FILENO)
            {
                // User input available
                char temp_buffer[256];
                ssize_t bytes_read = read(STDIN_FILENO, temp_buffer, sizeof(temp_buffer) - 1);

                if (bytes_read > 0)
                {
                    temp_buffer[bytes_read] = '\0';

                    // Append to input buffer
                    if (input_len + bytes_read < sizeof(input_buffer) - 1)
                    {
                        memcpy(input_buffer + input_len, temp_buffer, bytes_read);
                        input_len += bytes_read;
                        input_buffer[input_len] = '\0';
                    }

                    // Process complete lines
                    char *newline_pos;
                    while ((newline_pos = strchr(input_buffer, '\n')) != NULL)
                    {
                        *newline_pos = '\0';

                        // Remove carriage return if present
                        int cmd_len = strlen(input_buffer);
                        if (cmd_len > 0 && input_buffer[cmd_len - 1] == '\r')
                        {
                            input_buffer[cmd_len - 1] = '\0';
                        }

                        // Process command
                        if (strlen(input_buffer) > 0)
                        {
                            if (strcmp(input_buffer, "exit") == 0)
                            {
                                running = 0;
                                break;
                            }
                            else if (strcmp(input_buffer, "help") == 0)
                            {
                                show_help();
                            }
                            else if (strcmp(input_buffer, "clear") == 0)
                            {
                                printf("\033[H\033[J");
                            }
                            else
                            {
                                // Send command to server
                                process_user_command(sock, input_buffer, &transfer_state, epoll_fd);
                            }
                        }

                        // Shift remaining data in buffer
                        int remaining = input_len - (newline_pos - input_buffer + 1);
                        if (remaining > 0)
                        {
                            memmove(input_buffer, newline_pos + 1, remaining);
                            input_len = remaining;
                            input_buffer[input_len] = '\0';
                        }
                        else
                        {
                            input_len = 0;
                            input_buffer[0] = '\0';
                        }

                        if (running)
                        {
                            printf("ftp> ");
                            fflush(stdout);
                        }
                    }
                }
            }
            else if (fd == sock)
            {
                // Server data available or socket ready for writing
                if (events[i].events & (EPOLLERR | EPOLLHUP))
                {
                    printf(RED "\nServer disconnected\n" RESET);
                    running = 0;
                    break;
                }
                else if (events[i].events & EPOLLOUT && transfer_state.state == STATE_SENDING)
                {
                    // Socket ready for writing during file upload
                    int result = send_file_chunk_epoll(sock, &transfer_state);
                    if (result == 1)
                    {
                        // Upload complete
                        printf(GREEN "\nFile upload completed successfully!\n" RESET);
                        fclose(transfer_state.file_ptr);
                        init_transfer_state(&transfer_state);

                        // Remove EPOLLOUT from socket events
                        event.events = EPOLLIN;
                        event.data.fd = sock;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, sock, &event);

                        printf("ftp> ");
                        fflush(stdout);
                    }
                    else if (result == -1)
                    {
                        // Upload error
                        printf(RED "\nFile upload failed!\n" RESET);
                        if (transfer_state.file_ptr)
                        {
                            fclose(transfer_state.file_ptr);
                        }
                        init_transfer_state(&transfer_state);

                        // Remove EPOLLOUT from socket events
                        event.events = EPOLLIN;
                        event.data.fd = sock;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, sock, &event);

                        printf("ftp> ");
                        fflush(stdout);
                    }
                    // If result == 0 or -2, continue sending in next iteration
                }
                else if (events[i].events & EPOLLIN)
                {
                    char temp_buffer[1024];
                    ssize_t bytes_read = recv(sock, temp_buffer, sizeof(temp_buffer) - 1, 0);

                    if (bytes_read <= 0)
                    {
                        if (bytes_read == 0)
                        {
                            printf(RED "\nServer closed connection\n" RESET);
                        }
                        else if (errno != EWOULDBLOCK && errno != EAGAIN)
                        {
                            perror("recv");
                        }
                        running = 0;
                        break;
                    }

                    if (transfer_state.state == STATE_RECEIVING)
                    {
                        // Handle incoming file data
                        int result = receive_file_chunk_epoll(sock, &transfer_state, temp_buffer, bytes_read);
                        if (result == 1)
                        {
                            // Download complete
                            if (transfer_state.file_ptr)
                            {
                                fclose(transfer_state.file_ptr);
                            }
                            init_transfer_state(&transfer_state);
                            printf("ftp> ");
                            fflush(stdout);
                        }
                        else if (result == -1)
                        {
                            // Download error
                            printf(RED "\nFile download failed!\n" RESET);
                            if (transfer_state.file_ptr)
                            {
                                fclose(transfer_state.file_ptr);
                            }
                            init_transfer_state(&transfer_state);
                            printf("ftp> ");
                            fflush(stdout);
                        }
                    }
                    else
                    {
                        // Check if this might be the start of a file transfer (binary data)
                        if (bytes_read >= sizeof(FileChunkHeader) && transfer_state.state == STATE_COMMAND)
                        {
                            // Try to interpret as file header
                            FileChunkHeader potential_header;
                            memcpy(&potential_header, temp_buffer, sizeof(FileChunkHeader));

                            // Check if this looks like a valid file header
                            uint32_t chunk_id = ntohl(potential_header.chunk_id);
                            uint32_t total_chunks = ntohl(potential_header.total_chunks);
                            uint32_t chunk_size = ntohl(potential_header.chunk_size);

                            // If this looks like a valid file header (first chunk, reasonable values)
                            if (chunk_id == 0 && total_chunks > 0 && total_chunks < 2000000 &&
                                chunk_size > 0 && chunk_size <= CHUNK_SIZE &&
                                potential_header.filename[0] != '\0')
                            {
                                // This is likely the start of a file transfer
                                transfer_state.state = STATE_RECEIVING;
                                transfer_state.current_chunk = 0;
                                transfer_state.total_chunks = 0;
                                memset(transfer_state.filename, 0, FILENAME_MAX_LEN);

                                printf(GREEN "Starting download of file: %s\n" RESET, potential_header.filename);

                                // Process this data as file transfer
                                int result = receive_file_chunk_epoll(sock, &transfer_state, temp_buffer, bytes_read);
                                if (result == 1)
                                {
                                    // Download complete
                                    if (transfer_state.file_ptr)
                                    {
                                        fclose(transfer_state.file_ptr);
                                    }
                                    init_transfer_state(&transfer_state);
                                    printf("ftp> ");
                                    fflush(stdout);
                                }
                                else if (result == -1)
                                {
                                    // Download error
                                    printf(RED "\nFile download failed!\n" RESET);
                                    if (transfer_state.file_ptr)
                                    {
                                        fclose(transfer_state.file_ptr);
                                    }
                                    init_transfer_state(&transfer_state);
                                    printf("ftp> ");
                                    fflush(stdout);
                                }
                                continue; // Skip text processing
                            }
                        }

                        // Handle regular server responses (text)
                        temp_buffer[bytes_read] = '\0';

                        // Append to server buffer
                        if (server_data_len + bytes_read < sizeof(server_buffer) - 1)
                        {
                            memcpy(server_buffer + server_data_len, temp_buffer, bytes_read);
                            server_data_len += bytes_read;
                            server_buffer[server_data_len] = '\0';
                        }

                        // Process complete lines from server
                        char *newline_pos;
                        while ((newline_pos = strchr(server_buffer, '\n')) != NULL)
                        {
                            *newline_pos = '\0';

                            // Print server response
                            if (strlen(server_buffer) > 0)
                            {
                                // Check for error messages and highlight them
                                if (strncmp(server_buffer, "ERROR:", 6) == 0)
                                {
                                    printf(RED "%s\n" RESET, server_buffer);
                                }
                                else if (strncmp(server_buffer, "SUCCESS:", 8) == 0)
                                {
                                    printf(GREEN "%s\n" RESET, server_buffer);
                                }
                                else if (strncmp(server_buffer, "OK:", 3) == 0)
                                {
                                    printf(GREEN "%s\n" RESET, server_buffer);
                                }
                                else
                                {
                                    printf("%s\n", server_buffer);
                                }
                                printf("ftp> ");
                                fflush(stdout);
                            }

                            // Shift remaining data in buffer
                            int remaining = server_data_len - (newline_pos - server_buffer + 1);
                            if (remaining > 0)
                            {
                                memmove(server_buffer, newline_pos + 1, remaining);
                                server_data_len = remaining;
                                server_buffer[server_data_len] = '\0';
                            }
                            else
                            {
                                server_data_len = 0;
                                server_buffer[0] = '\0';
                            }
                        }
                    }
                }
            }
        }
    }

    // Cleanup
    if (transfer_state.file_ptr)
    {
        fclose(transfer_state.file_ptr);
    }
    close(epoll_fd);
    restore_stdin_blocking();
    close(sock);
    return 0;
}

void show_help()
{
    printf(YELLOW "Available commands:\n" RESET);
    printf(CYAN "  get <filename> - Download a file from the server\n" RESET);
    printf(CYAN "  send <filename> - Upload a file to the server\n" RESET);
    printf(CYAN "  list - List files on the server\n" RESET);
    printf(CYAN "  pwd - Print current working directory on the server\n" RESET);
    printf(CYAN "  cd <directory> - Change directory on the server\n" RESET);
    printf(CYAN "  delete <filename> - Delete a file on the server\n" RESET);
    printf(CYAN "  health - Show server health information\n" RESET);
    printf(CYAN "  help - Show this help message\n" RESET);
    printf(CYAN "  clear - Clear the console\n" RESET);
    printf(CYAN "  exit - Exit the client\n" RESET);
}

void process_user_command(int sock, const char *command, transfer_state_t *transfer_state, int epoll_fd)
{
    if (strncmp(command, "get ", 4) == 0)
    {
        const char *filename = command + 4;
        if (strlen(filename) == 0)
        {
            printf(RED "Error: 'get' command requires a filename.\n" RESET);
            return;
        }

        if (transfer_state->state != STATE_COMMAND)
        {
            printf(RED "Error: File transfer already in progress.\n" RESET);
            return;
        }

        if (start_file_download(sock, filename, transfer_state) == 0)
        {
            printf(GREEN "Starting download of file: %s\n" RESET, filename);
        }
    }
    else if (strncmp(command, "send ", 5) == 0)
    {
        const char *filename = command + 5;
        if (strlen(filename) == 0)
        {
            printf(RED "Error: 'send' command requires a filename.\n" RESET);
            return;
        }

        if (transfer_state->state != STATE_COMMAND)
        {
            printf(RED "Error: File transfer already in progress.\n" RESET);
            return;
        }

        if (start_file_upload(sock, filename, transfer_state) == 0)
        {
            // Send upload command to server first
            send(sock, "upload\n", 7, MSG_NOSIGNAL);

            // Enable EPOLLOUT for socket to start sending file chunks
            struct epoll_event event;
            event.events = EPOLLIN | EPOLLOUT;
            event.data.fd = sock;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, sock, &event);
        }
    }
    else if (strcmp(command, "list") == 0)
    {
        printf(BLUE "Listing files on the server...\n" RESET);
        send(sock, "ls\n", 3, MSG_NOSIGNAL);
    }
    else if (strcmp(command, "pwd") == 0)
    {
        printf(BLUE "Getting current working directory...\n" RESET);
        send(sock, "pwd\n", 4, MSG_NOSIGNAL);
    }
    else if (strncmp(command, "cd ", 3) == 0)
    {
        const char *path = command + 3;
        if (strlen(path) == 0)
        {
            printf(RED "Error: 'cd' command requires a directory path.\n" RESET);
            return;
        }
        printf(GREEN "Changing directory to: %s\n" RESET, path);
        char cmd_with_newline[256];
        snprintf(cmd_with_newline, sizeof(cmd_with_newline), "%s\n", command);
        send(sock, cmd_with_newline, strlen(cmd_with_newline), MSG_NOSIGNAL);
    }
    else if (strncmp(command, "delete ", 7) == 0)
    {
        const char *filename = command + 7;
        if (strlen(filename) == 0)
        {
            printf(RED "Error: 'delete' command requires a filename.\n" RESET);
            return;
        }
        printf(GREEN "Deleting file: %s\n" RESET, filename);
        char cmd_with_newline[256];
        snprintf(cmd_with_newline, sizeof(cmd_with_newline), "%s\n", command);
        send(sock, cmd_with_newline, strlen(cmd_with_newline), MSG_NOSIGNAL);
    }
    else if (strcmp(command, "health") == 0)
    {
        printf(BLUE "Getting server health information...\n" RESET);
        send(sock, "health\n", 7, MSG_NOSIGNAL);
    }
    else
    {
        printf("Unknown command: \"%s\". Use 'help' for a list of commands.\n", command);
    }
}

// Progress bar function

// Initialize transfer state
void init_transfer_state(transfer_state_t *state)
{
    // printf("DEBUG: Initializing transfer state\n");
    state->state = STATE_COMMAND;
    state->file_ptr = NULL;
    memset(state->filename, 0, FILENAME_MAX_LEN);
    state->total_chunks = 0;
    state->current_chunk = 0;
    state->bytes_written = 0;
    state->file_size = 0;
    state->buffer_pos = 0;
    state->header_received = 0;
    memset(&state->current_header, 0, sizeof(FileChunkHeader));
}

// Send file chunk in non-blocking manner
int send_file_chunk_epoll(int sock, transfer_state_t *state)
{
    if (!state->file_ptr)
    {
        fprintf(stderr, RED "Error: File not open for reading.\n" RESET);
        return -1; // File not open
    }

    // Read chunk from file
    int bytes_read = fread(state->file_buffer, 1, CHUNK_SIZE, state->file_ptr);
    if (bytes_read <= 0)
    {
        return 0; // End of file or error
    }

    //  header
    FileChunkHeader header;
    header.chunk_id = htonl(state->current_chunk);
    header.chunk_size = htonl(bytes_read);
    header.total_chunks = htonl(state->total_chunks);
    header.type = 0;

    if (state->current_chunk == 0)
    {
        strncpy(header.filename, state->filename, FILENAME_MAX_LEN - 1);
        header.filename[FILENAME_MAX_LEN - 1] = '\0';
    }
    else
    {
        memset(header.filename, 0, FILENAME_MAX_LEN);
    }

    // Send header
    if (send(sock, &header, sizeof(header), MSG_NOSIGNAL) != sizeof(header))
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
            return -2; // Would block, try again later
        }
        return -1; // Error
    }

    // Send payload
    if (send(sock, state->file_buffer, bytes_read, MSG_NOSIGNAL) != bytes_read)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
            return -2; // Would block, try again later
        }
        return -1; // Error
    }

    state->current_chunk++;
    int percent = (state->total_chunks > 0) ? (state->current_chunk * 100) / state->total_chunks : 0;
    progress_bar(percent);

    if (state->current_chunk >= state->total_chunks)
    {
        return 1; // Transfer complete
    }

    return 0; // Continue
}

// Handle incoming file data in non-blocking manner
int receive_file_chunk_epoll(int sock, transfer_state_t *state, char *buffer, int bytes_available)
{
    static char recv_buffer[sizeof(FileChunkHeader) + CHUNK_SIZE];
    static int recv_pos = 0;
    static int expecting_header = 1;
    static int payload_remaining = 0;
    static int last_chunk_id = -1;
    static int transfer_active = 0;

    // printf("DEBUG: receive_file_chunk_epoll called - bytes_available=%d, expecting_header=%d, recv_pos=%d, transfer_active=%d\n",
    //        bytes_available, expecting_header, recv_pos, transfer_active);

    // Reset static variables only when starting a completely new download
    // This should only happen when the client state is being reset
    if (state->current_chunk == 0 && state->total_chunks == 0 && !transfer_active)
    {
        // printf("DEBUG: Resetting static variables for new download\n");
        recv_pos = 0;
        expecting_header = 1;
        payload_remaining = 0;
        last_chunk_id = -1;
        transfer_active = 1;
    }

    int processed = 0;
    int max_iterations = bytes_available * 2; // Safety limit to prevent infinite loops
    int iteration_count = 0;

    while (processed < bytes_available && iteration_count < max_iterations)
    {
        iteration_count++;
        if (expecting_header)
        {
            // Need to receive header
            int header_needed = sizeof(FileChunkHeader) - recv_pos;
            int to_copy = (bytes_available - processed < header_needed) ? (bytes_available - processed) : header_needed;

            memcpy(recv_buffer + recv_pos, buffer + processed, to_copy);
            recv_pos += to_copy;
            processed += to_copy;

            if (recv_pos >= sizeof(FileChunkHeader))
            {
                // Header complete
                memcpy(&state->current_header, recv_buffer, sizeof(FileChunkHeader));

                // Convert from network byte order
                state->current_header.chunk_id = ntohl(state->current_header.chunk_id);
                state->current_header.chunk_size = ntohl(state->current_header.chunk_size);
                state->current_header.total_chunks = ntohl(state->current_header.total_chunks);

                // printf("DEBUG: Header received - chunk_id=%d, chunk_size=%d, total_chunks=%d, filename='%s'\n",
                //        state->current_header.chunk_id, state->current_header.chunk_size,
                //        state->current_header.total_chunks, state->current_header.filename);

                // Validate chunk size
                if (state->current_header.chunk_size > CHUNK_SIZE || state->current_header.chunk_size <= 0)
                {
                    printf(RED "\nInvalid chunk size: %d\n" RESET, state->current_header.chunk_size);
                    transfer_active = 0; // Reset on error
                    return -1;
                }

                // Validate total chunks for very large files
                if (state->current_header.total_chunks > 2000000)
                {
                    printf(RED "\nFile too large: %d chunks (max: 2,000,000)\n" RESET, state->current_header.total_chunks);
                    transfer_active = 0; // Reset on error
                    return -1;
                }

                // Update last chunk id tracking and validate sequence
                if (state->current_chunk > 0 && state->current_header.chunk_id != last_chunk_id + 1)
                {
                    printf(RED "\nChunk sequence error: expected %d, got %d\n" RESET,
                           last_chunk_id + 1, state->current_header.chunk_id);
                    transfer_active = 0; // Reset on error
                    return -1;
                }
                last_chunk_id = state->current_header.chunk_id;

                // First chunk contains filename
                if (state->current_header.chunk_id == 0)
                {
                    state->current_header.filename[FILENAME_MAX_LEN - 1] = '\0';
                    strncpy(state->filename, state->current_header.filename, FILENAME_MAX_LEN - 1);
                    state->total_chunks = state->current_header.total_chunks;

                    // printf("DEBUG: Opening file for writing: %s\n", state->filename);
                    state->file_ptr = fopen(state->filename, "wb");
                    if (!state->file_ptr)
                    {
                        printf(RED "\nFailed to open file for writing: %s\n" RESET, state->filename);
                        transfer_active = 0; // Reset on error
                        return -1;
                    }
                    printf(GREEN "\nReceiving file: %s (%d chunks)\n" RESET,
                           state->filename, state->total_chunks);
                }

                expecting_header = 0;
                payload_remaining = state->current_header.chunk_size;
                recv_pos = 0;
                // printf("DEBUG: Expecting payload of %d bytes\n", payload_remaining);
            }
        }
        else
        {
            // Receive payload
            int to_copy = (bytes_available - processed < payload_remaining) ? (bytes_available - processed) : payload_remaining;

            memcpy(recv_buffer + recv_pos, buffer + processed, to_copy);
            recv_pos += to_copy;
            processed += to_copy;
            payload_remaining -= to_copy;

            if (payload_remaining == 0)
            {
                // Payload complete, write to file
                // printf("DEBUG: Chunk payload complete, writing %d bytes to file\n", recv_pos);
                if (state->file_ptr && fwrite(recv_buffer, 1, recv_pos, state->file_ptr) != recv_pos)
                {
                    printf(RED "\nFailed to write to file\n" RESET);
                    transfer_active = 0; // Reset on error
                    return -1;
                }

                // Flush file periodically for large files to ensure data is written
                if (state->total_chunks > 10000 && state->current_chunk % 1000 == 0)
                {
                    fflush(state->file_ptr);
                }

                state->current_chunk++;
                int percent = (state->total_chunks > 0) ? (state->current_chunk * 100) / state->total_chunks : 0;
                progress_bar(percent);

                // For large files, show periodic status updates
                if (state->total_chunks > 10000)
                {
                    if (state->current_chunk % 5000 == 0 || state->current_chunk == state->total_chunks)
                    {
                        printf("\nProgress: %d/%d chunks (%d%%) - %.2f MB received",
                               state->current_chunk, state->total_chunks, percent,
                               (float)(state->current_chunk * CHUNK_SIZE) / (1024 * 1024));
                        fflush(stdout);
                    }
                }
                else if (state->total_chunks > 1000 && state->current_chunk % 1000 == 0)
                {
                    printf("\nProgress: %d/%d chunks (%d%%)",
                           state->current_chunk, state->total_chunks, percent);
                    fflush(stdout);
                }

                // For very large files, add extra file flushing to prevent buffer issues
                if (state->total_chunks > 500000 && state->current_chunk % 500 == 0)
                {
                    fflush(state->file_ptr);
                    fsync(fileno(state->file_ptr)); // Force data to disk
                }

                // printf("DEBUG: Completed chunk %d/%d\n", state->current_chunk, state->total_chunks);

                // Check if transfer complete
                if (state->current_chunk >= state->total_chunks)
                {
                    printf(GREEN "\nFile received successfully: %s\n" RESET, state->filename);

                    // Reset static variables for next download
                    // printf("DEBUG: Transfer complete, resetting static variables\n");
                    recv_pos = 0;
                    expecting_header = 1;
                    payload_remaining = 0;
                    last_chunk_id = -1;
                    transfer_active = 0; // Mark transfer as complete

                    return 1; // Transfer complete
                }

                expecting_header = 1;
                recv_pos = 0;
                // printf("DEBUG: Ready for next chunk header\n");
            }
        }
    }

    // Check if we hit the iteration limit (potential infinite loop)
    if (iteration_count >= max_iterations)
    {
        printf(RED "\nError: Hit iteration limit during file receive (potential corruption)\n" RESET);
        transfer_active = 0; // Reset on error
        return -1;
    }

    return 0; // Continue receiving
}

// Start file upload
int start_file_upload(int sock, const char *filename, transfer_state_t *state)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp)
    {
        printf(RED "Error: Cannot open file '%s' for reading\n" RESET, filename);
        return -1;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    rewind(fp);

    if (filesize <= 0)
    {
        printf(RED "Error: File '%s' is empty or cannot determine size\n" RESET, filename);
        fclose(fp);
        return -1;
    }

    // Initialize upload state
    state->state = STATE_SENDING;
    state->file_ptr = fp;
    strncpy(state->filename, filename, FILENAME_MAX_LEN - 1);
    state->filename[FILENAME_MAX_LEN - 1] = '\0';
    state->file_size = filesize;
    state->total_chunks = (filesize > 0) ? (filesize + CHUNK_SIZE - 1) / CHUNK_SIZE : 1;
    state->current_chunk = 0;

    printf(GREEN "Starting upload of '%s' (%ld bytes, %d chunks)\n" RESET,
           filename, filesize, state->total_chunks);

    return 0;
}

// Start file download
int start_file_download(int sock, const char *filename, transfer_state_t *state)
{
    // Send get command to server
    char command[256];
    snprintf(command, sizeof(command), "get %s\n", filename);

    if (send(sock, command, strlen(command), MSG_NOSIGNAL) <= 0)
    {
        printf(RED "Error: Failed to send get command\n" RESET);
        return -1;
    }

    // Don't switch to receiving state immediately - wait for server response
    // The state will be changed in the response handler if the file exists

    printf(GREEN "Requesting file: %s\n" RESET, filename);
    return 0;
}

int main()
{
    printf(GREEN "Starting epoll-based FTP client...\n" RESET);
    return start_epoll_client("127.0.0.1", 8080);
}

// Progress bar function
void progress_bar(int percent)
{
    const int length = 30;
    int filled = (percent * length) / 100;
    printf("\r[");
    for (int i = 0; i < filled; i++)
        printf("█");
    for (int i = filled; i < length; i++)
        printf("▒");
    printf("] %d%%", percent);
    fflush(stdout);
}
