#include "server.h"
#include "commands.h"
#include "colors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#define MAX_EVENTS 64
#define MAX_CLIENTS 10
#define CHUNK_SIZE 512
#define FILENAME_MAX_LEN 64

// File transfer structures (matching client-side)
typedef struct
{
    uint32_t chunk_id;
    uint32_t chunk_size;
    uint32_t total_chunks;
    uint32_t type;
    char filename[FILENAME_MAX_LEN];
} FileChunkHeader;

// Client state structure
typedef struct
{
    int socket_fd;
    char buffer[1024];
    int buffer_len;
    int state; // 0 = reading command, 1 = file transfer, etc.
    char client_ip[INET_ADDRSTRLEN];
    // File transfer specific fields
    FILE *upload_file;
    char upload_filename[256];
    int expected_chunks;
    int received_chunks;
    // File transfer receive buffer state
    char recv_buffer[sizeof(FileChunkHeader) + CHUNK_SIZE];
    int bytes_in_buffer;
    int header_complete;
    FileChunkHeader current_header;
    int payload_remaining;
} client_info_t;

// Global client storage
static client_info_t clients[MAX_CLIENTS];
static int client_count = 0;

// Function declarations
int set_nonblocking(int socket_fd);
client_info_t *add_client(int socket_fd, const char *client_ip);
void remove_client(int socket_fd);
client_info_t *find_client(int socket_fd);
int handle_new_connection(int server_fd, int epoll_fd);
int handle_client_data(int client_fd);
int handle_file_transfer(int client_fd);
void process_client_command(int sock, const char *command);

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

// Add client to our client array
client_info_t *add_client(int socket_fd, const char *client_ip)
{
    if (client_count >= MAX_CLIENTS)
    {
        printf(RED "Maximum client limit reached\n" RESET);
        return NULL;
    }

    client_info_t *client = &clients[client_count++];
    client->socket_fd = socket_fd;
    client->buffer_len = 0;
    client->state = 0;
    strncpy(client->client_ip, client_ip, INET_ADDRSTRLEN - 1);
    client->client_ip[INET_ADDRSTRLEN - 1] = '\0';
    // Initialize file transfer fields
    client->upload_file = NULL;
    client->upload_filename[0] = '\0';
    client->expected_chunks = 0;
    client->received_chunks = 0;
    // Initialize per-client transfer state
    client->bytes_in_buffer = 0;
    client->header_complete = 0;
    client->payload_remaining = 0;
    // Initialize file transfer receive state
    client->bytes_in_buffer = 0;
    client->header_complete = 0;
    client->payload_remaining = 0;
    memset(&client->current_header, 0, sizeof(FileChunkHeader));

    return client;
}

// Remove client from our client array
void remove_client(int socket_fd)
{
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].socket_fd == socket_fd)
        {
            // Clean up any open file handles
            if (clients[i].upload_file)
            {
                fclose(clients[i].upload_file);
                clients[i].upload_file = NULL;
            }

            // Move last client to this position
            if (i < client_count - 1)
            {
                clients[i] = clients[client_count - 1];
            }
            client_count--;
            break;
        }
    }
}

// Find client by socket fd
client_info_t *find_client(int socket_fd)
{
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].socket_fd == socket_fd)
        {
            return &clients[i];
        }
    }
    return NULL;
}

// Handle new client connection
int handle_new_connection(int server_fd, int epoll_fd)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd == -1)
    {
        if (errno != EWOULDBLOCK && errno != EAGAIN)
        {
            perror("accept");
        }
        return -1;
    }

    // Set client socket to non-blocking
    if (set_nonblocking(client_fd) == -1)
    {
        close(client_fd);
        return -1;
    }

    // Get client IP
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    // Add client to our tracking
    client_info_t *client = add_client(client_fd, client_ip);
    if (!client)
    {
        close(client_fd);
        return -1;
    }

    // Add client socket to epoll
    struct epoll_event event;
    event.events = EPOLLIN; // Level-triggered mode for reliable data handling
    event.data.fd = client_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1)
    {
        perror("epoll_ctl: client");
        remove_client(client_fd);
        close(client_fd);
        return -1;
    }

    printf(GREEN "New client connected from %s (fd: %d)\n" RESET, client_ip, client_fd);
    log_message("INFO", "New client connected");

    return 0;
}

// Handle client data - non-blocking version of handle_client
int handle_client_data(int client_fd)
{
    client_info_t *client = find_client(client_fd);
    if (!client)
    {
        printf(RED "Client not found for fd %d\n" RESET, client_fd);
        return -1;
    }

    // If client is in file transfer mode, handle binary data
    if (client->state == 1)
    {
        return handle_file_transfer(client_fd);
    }

    // Normal command handling
    char temp_buffer[1024];
    ssize_t bytes_read = recv(client_fd, temp_buffer, sizeof(temp_buffer) - 1, 0);

    if (bytes_read <= 0)
    {
        if (bytes_read == 0)
        {
            printf(YELLOW "Client %s disconnected (fd: %d)\n" RESET, client->client_ip, client_fd);
            log_message("INFO", "Client disconnected");
        }
        else if (errno != EWOULDBLOCK && errno != EAGAIN)
        {
            perror("recv");
            printf(RED "Error reading from client %s (fd: %d)\n" RESET, client->client_ip, client_fd);
            log_message("ERROR", "Error reading from client");
        }
        return -1;
    }

    temp_buffer[bytes_read] = '\0';

    // Detect if this looks like binary file data instead of commands
    if (bytes_read >= (int)sizeof(FileChunkHeader))
    {
        // Check if this might be a file header by examining the first few bytes
        FileChunkHeader *potential_header = (FileChunkHeader *)temp_buffer;
        uint32_t chunk_size = ntohl(potential_header->chunk_size);
        uint32_t total_chunks = ntohl(potential_header->total_chunks);

        // Reasonable bounds check for file transfer header - prevent division by zero
        if (chunk_size > 0 && chunk_size <= 8192 && total_chunks > 0 && total_chunks <= 2000000)
        {
            printf(CYAN "Detected file transfer data in command mode for client %s, switching to file mode\n" RESET, client->client_ip);
            client->state = 1; // Switch to file transfer mode

           // process the header data directly using temp_buffer
            int processed = 0;
            while (processed < bytes_read)
            {
                if (!client->header_complete)
                {
                    int header_needed = sizeof(FileChunkHeader) - client->bytes_in_buffer;
                    int to_copy = (bytes_read - processed < header_needed) ? (bytes_read - processed) : header_needed;

                    memcpy(client->recv_buffer + client->bytes_in_buffer, temp_buffer + processed, to_copy);
                    client->bytes_in_buffer += to_copy;
                    processed += to_copy;

                    if (client->bytes_in_buffer >= (int)sizeof(FileChunkHeader))
                    {
                        memcpy(&client->current_header, client->recv_buffer, sizeof(FileChunkHeader));
                        client->header_complete = 1;
                        client->payload_remaining = ntohl(client->current_header.chunk_size);
                        client->bytes_in_buffer = 0;

                        // If first chunk, open file
                        if (ntohl(client->current_header.chunk_id) == 0)
                        {
                            mkdir("saved", 0777);
                            char full_path[512];
                            snprintf(full_path, sizeof(full_path), "saved/%s", client->current_header.filename);
                            client->upload_file = fopen(full_path, "wb");
                            if (client->upload_file)
                            {
                                strncpy(client->upload_filename, client->current_header.filename, sizeof(client->upload_filename) - 1);
                                client->expected_chunks = ntohl(client->current_header.total_chunks);
                                client->received_chunks = 0;
                                printf(BLUE "Starting upload of '%s' (%d chunks)\n" RESET,
                                       client->current_header.filename, client->expected_chunks);
                            }
                        }
                    }
                }
                else
                {
                    int to_copy = (bytes_read - processed < client->payload_remaining) ? (bytes_read - processed) : client->payload_remaining;
                    if (client->upload_file)
                    {
                        fwrite(temp_buffer + processed, 1, to_copy, client->upload_file);
                        fflush(client->upload_file);
                    }
                    processed += to_copy;
                    client->payload_remaining -= to_copy;

                    if (client->payload_remaining == 0)
                    {
                        client->received_chunks++;
                        client->header_complete = 0;
                        client->bytes_in_buffer = 0;
                        printf("Received chunk %d/%d\n", client->received_chunks, client->expected_chunks);

                        if (client->received_chunks >= client->expected_chunks)
                        {
                            // File transfer complete
                            if (client->upload_file)
                            {
                                fclose(client->upload_file);
                                client->upload_file = NULL;
                            }
                            printf(GREEN "File received successfully: %s\n" RESET, client->upload_filename);
                            send(client_fd, "SUCCESS: File uploaded\n", 23, 0);

                            // Reset client state
                            client->state = 0;
                            client->bytes_in_buffer = 0;
                            client->header_complete = 0;
                            client->payload_remaining = 0;
                            client->expected_chunks = 0;
                            client->received_chunks = 0;
                            client->upload_filename[0] = '\0';
                        }
                    }
                }
            }
            return 0;
        }
    }

    // Append to client buffer
    if (client->buffer_len + bytes_read < sizeof(client->buffer) - 1)
    {
        memcpy(client->buffer + client->buffer_len, temp_buffer, bytes_read);
        client->buffer_len += bytes_read;
        client->buffer[client->buffer_len] = '\0';
    }
    else
    {
        printf(YELLOW "Buffer overflow for client %s, disconnecting\n" RESET, client->client_ip);
        log_message("WARNING", "Client buffer overflow - disconnecting client");

        // Send error message to client before disconnecting
        const char *error_msg = "ERROR: Buffer overflow - connection terminated\n";
        send(client_fd, error_msg, strlen(error_msg), MSG_NOSIGNAL);

        // Force the message to be sent immediately
        fsync(client_fd);

        // Give client a moment to receive the message before disconnecting
        usleep(100000); // 100ms delay

        // Return -1 to signal that client should be disconnected
        return -1;
    }

    // Process complete commands (ending with newline)
    char *newline_pos;
    while ((newline_pos = strchr(client->buffer, '\n')) != NULL)
    {
        *newline_pos = '\0';

        // Remove carriage return if present
        int cmd_len = strlen(client->buffer);
        if (cmd_len > 0 && client->buffer[cmd_len - 1] == '\r')
        {
            client->buffer[cmd_len - 1] = '\0';
        }

        printf(CYAN "Command from %s (fd: %d): '%s'\n" RESET, client->client_ip, client_fd, client->buffer);

        // Process the command (reuse existing command handling logic)
        process_client_command(client_fd, client->buffer);

        // Shift remaining data in buffer
        int remaining = client->buffer_len - (newline_pos - client->buffer + 1);
        if (remaining > 0)
        {
            memmove(client->buffer, newline_pos + 1, remaining);
            client->buffer_len = remaining;
            client->buffer[client->buffer_len] = '\0';
        }
        else
        {
            client->buffer_len = 0;
            client->buffer[0] = '\0';
        }
    }

    return 0;
}

// Process individual client command (extracted from handle_client)
void process_client_command(int sock, const char *command)
{
    log_message("INFO", "Processing command");

    if (strncmp(command, "upload", 6) == 0)
    {
        log_message("INFO", "Handling upload command");
        // Switch client to file transfer mode
        client_info_t *client = find_client(sock);
        if (client)
        {
            client->state = 1; // Set to file transfer mode
            printf(CYAN "Client %s switched to file transfer mode\n" RESET, client->client_ip);
        }
    }
    else if (strncmp(command, "get ", 4) == 0)
    {
        log_message("INFO", "Handling get command");
        send_file(sock, command + 4);
    }
    else if (strcmp(command, "ls") == 0)
    {
        log_message("INFO", "Handling ls command");
        send_list(sock);
    }
    else if (strcmp(command, "pwd") == 0)
    {
        log_message("INFO", "Handling pwd command");
        send_pwd(sock);
    }
    else if (strncmp(command, "cd ", 3) == 0)
    {
        log_message("INFO", "Handling cd command");
        change_dir(sock, command + 3);
    }
    else if (strncmp(command, "delete ", 7) == 0)
    {
        log_message("INFO", "Handling delete command");
        delete_file(sock, command + 7);
    }
    else if (strncmp(command, "rename ", 7) == 0)
    {
        log_message("INFO", "Handling rename command");
        char cmd_copy[128];
        strncpy(cmd_copy, command, sizeof(cmd_copy) - 1);
        cmd_copy[sizeof(cmd_copy) - 1] = '\0';

        char *old_name = strtok(cmd_copy + 7, " ");
        char *new_name = strtok(NULL, " ");
        if (old_name && new_name)
        {
            rename_file(sock, old_name, new_name);
        }
        else
        {
            log_message("ERROR", "Invalid rename command");
            send(sock, "ERROR: Invalid rename command\n", 30, MSG_NOSIGNAL);
        }
    }
    else if (strcmp(command, "health") == 0)
    {
        log_message("INFO", "Handling health command");
        send_health_info(sock);
    }
    else
    {
        log_message("WARNING", "Unknown command received");
        printf(YELLOW "Unknown command: '%s'\n" RESET, command);
        send(sock, "ERROR: Unknown command\n", 23, MSG_NOSIGNAL);
    }
}

// Handle file transfer data (binary chunks)
int handle_file_transfer(int client_fd)
{
    client_info_t *client = find_client(client_fd);
    if (!client)
    {
        return -1;
    }

    // Process only available data - removed blocking while(1) loop
    char temp_buffer[2048];
    ssize_t bytes_read = recv(client_fd, temp_buffer, sizeof(temp_buffer), 0);

    if (bytes_read <= 0)
    {
        if (bytes_read == 0)
        {
            printf(YELLOW "Client %s disconnected during file transfer\n" RESET, client->client_ip);
            // Clean up file transfer
            if (client->upload_file)
            {
                fclose(client->upload_file);
                client->upload_file = NULL;
            }
            client->state = 0;
            return -1;
        }
        else if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
            // No more data available right now, return to event loop
            return 0;
        }
        else
        {
            perror("recv during file transfer");
            // Clean up file transfer on actual error
            if (client->upload_file)
            {
                fclose(client->upload_file);
                client->upload_file = NULL;
            }
            client->state = 0;
            return -1;
        }
    }

    printf("DEBUG: Received %zd bytes in file transfer mode\n", bytes_read);

    // Process the received data
    int processed = 0;
    while (processed < bytes_read)
    {
        if (!client->header_complete)
        {
            // Still receiving header
            int header_needed = sizeof(FileChunkHeader) - client->bytes_in_buffer;
            int to_copy = (bytes_read - processed < header_needed) ? (bytes_read - processed) : header_needed;

            memcpy(client->recv_buffer + client->bytes_in_buffer, temp_buffer + processed, to_copy);
            client->bytes_in_buffer += to_copy;
            processed += to_copy;

            if (client->bytes_in_buffer >= (int)sizeof(FileChunkHeader))
            {
                // Header complete
                memcpy(&client->current_header, client->recv_buffer, sizeof(FileChunkHeader));
                client->header_complete = 1;

                // Convert from network byte order
                uint32_t chunk_id = ntohl(client->current_header.chunk_id);
                uint32_t chunk_size = ntohl(client->current_header.chunk_size);
                uint32_t total_chunks = ntohl(client->current_header.total_chunks);

                // Validate header values to prevent client crashes
                if (chunk_size == 0 || total_chunks == 0 || chunk_size > 8192 || total_chunks > 2000000)
                {
                    printf(RED "Invalid file transfer header: chunk_size=%u, total_chunks=%u\n" RESET, chunk_size, total_chunks);
                    send(client_fd, "ERROR: Invalid file transfer header\n", 36, 0);
                    client->state = 0;
                    return -1;
                }

                client->payload_remaining = chunk_size;

                printf("DEBUG: Header complete - chunk %d/%d, size %d\n", chunk_id + 1, total_chunks, chunk_size);

                // If this is the first chunk, open the file
                if (chunk_id == 0)
                {
                    mkdir("saved", 0777);
                    char full_path[512];
                    snprintf(full_path, sizeof(full_path), "saved/%s", client->current_header.filename);

                    client->upload_file = fopen(full_path, "wb");
                    if (!client->upload_file)
                    {
                        printf(RED "Error: Cannot create file '%s'\n" RESET, full_path);
                        send(client_fd, "ERROR: Cannot create file\n", 26, 0);
                        client->state = 0;
                        return -1;
                    }

                    strncpy(client->upload_filename, client->current_header.filename, sizeof(client->upload_filename) - 1);
                    client->expected_chunks = total_chunks;
                    client->received_chunks = 0;

                    printf(BLUE "Starting upload of '%s' (%d chunks)\n" RESET,
                           client->current_header.filename, total_chunks);
                }

                // Reset buffer for payload
                client->bytes_in_buffer = 0;
            }
        }
        else
        {
            // Receiving payload
            int to_copy = (bytes_read - processed < client->payload_remaining) ? (bytes_read - processed) : client->payload_remaining;

            if (client->upload_file)
            {
                fwrite(temp_buffer + processed, 1, to_copy, client->upload_file);
                fflush(client->upload_file); // Ensure data is written immediately
            }

            processed += to_copy;
            client->payload_remaining -= to_copy;

            printf("DEBUG: Payload progress: %d/%d bytes for current chunk\n",
                   (int)(ntohl(client->current_header.chunk_size) - client->payload_remaining),
                   (int)ntohl(client->current_header.chunk_size));

            if (client->payload_remaining == 0)
            {
                // Chunk complete
                client->received_chunks++;
                client->header_complete = 0;
                client->bytes_in_buffer = 0;

                printf("Received chunk %d/%d\n", client->received_chunks, client->expected_chunks);

                if (client->received_chunks >= client->expected_chunks)
                {
                    // File transfer complete
                    if (client->upload_file)
                    {
                        fclose(client->upload_file);
                        client->upload_file = NULL;
                    }

                    printf(GREEN "File received successfully: %s\n" RESET, client->upload_filename);
                    send(client_fd, "SUCCESS: File uploaded\n", 23, 0);

                    // Reset client state completely
                    client->state = 0;
                    client->bytes_in_buffer = 0;
                    client->header_complete = 0;
                    client->payload_remaining = 0;
                    client->expected_chunks = 0;
                    client->received_chunks = 0;
                    client->upload_filename[0] = '\0';

                    printf(CYAN "Client %s switched back to command mode\n" RESET, client->client_ip);
                    return 0;
                }
            }
        }
    }

    // Return to event loop to handle other clients
    return 0;
}

// Main epoll-based server function
void start_epoll_server(int port)
{
    // Create server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("socket");
        exit(1);
    }

    // Set server socket to non-blocking
    if (set_nonblocking(server_fd) == -1)
    {
        close(server_fd);
        exit(1);
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        perror("setsockopt");
        close(server_fd);
        exit(1);
    }

    // Bind socket
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        close(server_fd);
        exit(1);
    }

    // Listen
    if (listen(server_fd, SOMAXCONN) == -1)
    {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    // Create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("epoll_create1");
        close(server_fd);
        exit(1);
    }

    // Add server socket to epoll
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1)
    {
        perror("epoll_ctl: server");
        close(epoll_fd);
        close(server_fd);
        exit(1);
    }

    printf(GREEN "Epoll-based server listening on port %d...\n" RESET, port);
    log_message("INFO", "Epoll server started");

    // Event loop
    struct epoll_event events[MAX_EVENTS];

    while (1)
    {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (num_events == -1)
        {
            if (errno == EINTR)
                continue; // Interrupted by signal, continue
            perror("epoll_wait");
            break;
        }

        // Process all ready events
        for (int i = 0; i < num_events; i++)
        {
            int fd = events[i].data.fd;

            if (fd == server_fd)
            {
                // New connection
                handle_new_connection(server_fd, epoll_fd);
            }
            else
            {
                // Client data ready
                if (events[i].events & (EPOLLERR | EPOLLHUP))
                {
                    // Client disconnected or error
                    printf(YELLOW "Client disconnected (fd: %d)\n" RESET, fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    remove_client(fd);
                    close(fd);
                }
                else if (events[i].events & EPOLLIN)
                {
                    // Data available for reading
                    if (handle_client_data(fd) == -1)
                    {
                        // Client disconnected or error
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        remove_client(fd);
                        close(fd);
                    }
                }
            }
        }
    }

    // Cleanup
    close(epoll_fd);
    close(server_fd);
    printf(GREEN "Server shutdown complete\n" RESET);
}
