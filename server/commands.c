#define _DEFAULT_SOURCE
#include "commands.h"
#include "colors.h"
#include "server.h" // Ensure the log_message function is accessible
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <errno.h>
#include <netinet/tcp.h>

#define CHUNK_SIZE 512
#define FILENAME_MAX_LEN 64

typedef struct
{
    uint32_t chunk_id;
    uint32_t chunk_size;
    uint32_t total_chunks;
    uint32_t type;
    char filename[FILENAME_MAX_LEN];
} FileChunkHeader;

ssize_t read_line(int sock, char *buf, size_t max_len)
{
    size_t i = 0;
    char c;
    while (i < max_len - 1)
    {
        ssize_t n = recv(sock, &c, 1, 0);
        if (n <= 0)
            return n;
        if (c == '\n')
            break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    if (i == max_len - 1)
    {
        // Ensure no buffer overflow
        while (recv(sock, &c, 1, 0) > 0 && c != '\n')
            ;
    }
    return i;
}

void receive_file(int sock)
{
    FILE *fp = NULL;
    int total_chunks = 0;
    int received_chunks = 0;
    char filename[FILENAME_MAX_LEN] = {0};

    mkdir("saved", 0777);

    while (1)
    {
        FileChunkHeader header;
        int header_bytes = recv(sock, &header, sizeof(header), MSG_WAITALL);
        if (header_bytes <= 0)
            break;

        int chunk_id = ntohl(header.chunk_id);
        int chunk_size = ntohl(header.chunk_size);
        total_chunks = ntohl(header.total_chunks);

        if (chunk_id == 0)
        {
            strncpy(filename, header.filename, FILENAME_MAX_LEN - 1);
            filename[FILENAME_MAX_LEN - 1] = '\0'; // Ensure null-termination
            // save on 'saved' directory
            char full_path[256];
            snprintf(full_path, sizeof(full_path), "saved/%s", filename);
            fp = fopen(full_path, "wb");
            if (!fp)
            {
                printf(RED "Error: Cannot create file '%s'\n" RESET, full_path);
                send(sock, "ERROR: Cannot create file\n", 26, 0);
                perror("fopen");
                return;
            }
        }

        uint8_t buffer[CHUNK_SIZE];
        ssize_t data_bytes = recv(sock, buffer, chunk_size, MSG_WAITALL);
        if (data_bytes != chunk_size)
            break;

        if (fp)
            fwrite(buffer, 1, chunk_size, fp);

        received_chunks++;
        if (received_chunks >= total_chunks)
            break;
    }

    if (fp)
    {
        fclose(fp);
        printf(GREEN "File received successfully: %s\n" RESET, filename);
        send(sock, "SUCCESS: File uploaded\n", 23, 0);
    }
    else
    {
        printf(RED "Error: File transfer failed\n" RESET);
        send(sock, "ERROR: File transfer failed\n", 28, 0);
    }
}

void send_file(int sock, const char *filename)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp)
    {
        printf(RED "Error: Cannot open file '%s' for reading\n" RESET, filename);
        send(sock, "ERROR: File not found\n", 22, 0);
        return;
    }

    printf(BLUE "Sending file: %s\n" RESET, filename);

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    int total_chunks = (fsize + CHUNK_SIZE - 1) / CHUNK_SIZE;

    printf(YELLOW "File size: %ld bytes, Total chunks: %d\n" RESET, fsize, total_chunks);

    // For very large files, optimize TCP socket buffer sizes
    if (total_chunks > 100000)
    {
        int send_buffer_size = 256 * 1024; // 256KB send buffer
        int recv_buffer_size = 256 * 1024; // 256KB receive buffer

        if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) < 0)
        {
            printf(YELLOW "Warning: Could not set send buffer size\n" RESET);
        }
        if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recv_buffer_size, sizeof(recv_buffer_size)) < 0)
        {
            printf(YELLOW "Warning: Could not set receive buffer size\n" RESET);
        }

        printf(YELLOW "Optimized TCP settings for large file transfer\n" RESET);
    }

    for (int i = 0; i < total_chunks; i++)
    {
        uint8_t buffer[CHUNK_SIZE] = {0};
        int read_bytes = fread(buffer, 1, CHUNK_SIZE, fp);

        FileChunkHeader header = {
            .chunk_id = htonl(i),
            .chunk_size = htonl(read_bytes),
            .total_chunks = htonl(total_chunks),
            .type = htonl(0)};
        strncpy(header.filename, filename, FILENAME_MAX_LEN - 1);
        header.filename[FILENAME_MAX_LEN - 1] = '\0';

        // Send header + data as a single atomic operation to prevent corruption
        char chunk_data[sizeof(FileChunkHeader) + CHUNK_SIZE];
        memcpy(chunk_data, &header, sizeof(FileChunkHeader));
        memcpy(chunk_data + sizeof(FileChunkHeader), buffer, read_bytes);

        int total_to_send = sizeof(FileChunkHeader) + read_bytes;
        int sent = 0;
        int retry_count = 0;
        const int MAX_RETRIES = 10;

        while (sent < total_to_send && retry_count < MAX_RETRIES)
        {
            ssize_t result = send(sock, chunk_data + sent, total_to_send - sent, MSG_NOSIGNAL);
            if (result <= 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // Socket buffer full, wait and retry
                    usleep(1000 * (retry_count + 1)); // Progressive delay: 1ms, 2ms, 3ms...
                    retry_count++;
                    continue;
                }
                else if (errno == EPIPE || errno == ECONNRESET)
                {
                    printf(RED "Error: Connection lost during chunk %d (client disconnected)\n" RESET, i);
                    fclose(fp);
                    return;
                }
                else
                {
                    printf(RED "Error: Failed to send chunk %d: %s\n" RESET, i, strerror(errno));
                    fclose(fp);
                    return;
                }
            }
            else
            {
                sent += result;
                retry_count = 0; // Reset retry count on successful send
            }
        }

        if (sent < total_to_send)
        {
            printf(RED "Error: Failed to send complete chunk %d after %d retries\n" RESET, i, MAX_RETRIES);
            fclose(fp);
            return;
        }

        // Progressive delays for very large files
        if (total_chunks > 500000 && i % 100 == 0)
        {
            usleep(900); // 0.5ms delay every 100 chunks for very large files
        }
        else if (total_chunks > 100000 && i % 500 == 0)
        {
            usleep(600); // 0.2ms delay every 500 chunks for large files
        }

        // Progress reporting
        if (total_chunks > 50000 && i % 1000 == 0)
        {
            printf("Sent chunk %d/%d (%.1f%%)\n", i + 1, total_chunks,
                   ((float)(i + 1) / total_chunks) * 100.0);
        }
        else if (total_chunks > 10000 && i % 5000 == 0)
        {
            printf("Sent chunk %d/%d (%.1f%%)\n", i + 1, total_chunks,
                   ((float)(i + 1) / total_chunks) * 100.0);
        }
    }

    fclose(fp);
    printf(GREEN "File sent successfully: %s (%d chunks)\n" RESET, filename, total_chunks);
}

void send_list(int sock)
{
    DIR *d = opendir(".");
    if (!d)
    {
        printf(RED "Error: Cannot open current directory\n" RESET);
        send(sock, "ERROR: Cannot list directory\n", 29, 0);

        return;
    }

    printf(BLUE "Listing directory contents\n" RESET);
    struct dirent *dir;
    char buffer[512] = {0};
    while ((dir = readdir(d)) != NULL)
    {
        // Skip the current and parent directory entries
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
            continue;

        struct stat st;
        // Get file type and name
        if (strlen(dir->d_name) >= FILENAME_MAX_LEN)
        {
            printf(RED "Error: Filename '%s' is too long\n" RESET, dir->d_name);
            snprintf(buffer, sizeof(buffer), "%s (Error: Filename too long)\n", dir->d_name);
            send(sock, buffer, strlen(buffer), 0);
            continue;
        }

        // TODO LOOK ??
        if (stat(dir->d_name, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
            {
                snprintf(buffer, sizeof(buffer), "- 📁 %s (Directory)\n", dir->d_name);
            }
            else if (S_ISREG(st.st_mode))
            {
                snprintf(buffer, sizeof(buffer), "- 📄 %s (File)\n", dir->d_name);
            }
            else
            {
                snprintf(buffer, sizeof(buffer), "%s (Other)\n", dir->d_name);
            }
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "%s (Error getting type)\n", dir->d_name);
        }

        send(sock, buffer, strlen(buffer), 0);
        printf(CYAN "Sent: %s" RESET, buffer);

        // Clear buffer for next entry
        memset(buffer, 0, sizeof(buffer));
    }
    closedir(d);
    send(sock, "END_OF_LIST\n", 12, 0);
}

void send_pwd(int sock)
{
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)))
    {
        printf(BLUE "Current directory: %s\n" RESET, cwd);
        strcat(cwd, "\n");
        send(sock, cwd, strlen(cwd), 0);
    }
    else
    {
        printf(RED "Error: Cannot get current directory\n" RESET);
        send(sock, "ERROR: Cannot get current directory\n", 36, 0);
    }
}

void change_dir(int sock, const char *path)
{
    if (chdir(path) == 0)
    {
        printf(GREEN "Changed directory to: %s\n" RESET, path);
        send(sock, "OK: Directory changed\n", 22, 0);
    }
    else
    {
        printf(RED "Error: Cannot change to directory '%s'\n" RESET, path);
        send(sock, "ERROR: Cannot change directory\n", 31, 0);
    }
}

void delete_file(int sock, const char *filename)
{
    if (unlink(filename) == 0)
    {
        printf(GREEN "Deleted file: %s\n" RESET, filename);
        send(sock, "SUCCESS: File deleted\n", 23, 0);
    }
    else
    {
        printf(RED "Error: Cannot delete file '%s'\n" RESET, filename);
        send(sock, "ERROR: Cannot delete file\n", 26, 0);
    }
}

void rename_file(int sock, const char *old_name, const char *new_name)
{
    if (rename(old_name, new_name) == 0)
    {
        printf(GREEN "Renamed file: %s -> %s\n" RESET, old_name, new_name);
        send(sock, "SUCCESS: File renamed\n", 23, 0);
    }
    else
    {
        printf(RED "Error: Cannot rename file '%s' to '%s'\n" RESET, old_name, new_name);
        send(sock, "ERROR: Cannot rename file\n", 26, 0);
    }
}

void handle_client(int sock)
{
    char command[128];
    log_message("INFO", "Client handler started");

    while (1)
    {
        memset(command, 0, sizeof(command));
        ssize_t bytes_read = read_line(sock, command, sizeof(command));

        if (bytes_read <= 0)
        {
            log_message("WARNING", "Client disconnected or read error");
            break;
        }

        log_message("INFO", "Received command");
        printf(CYAN "Received command: '%s'\n" RESET, command);

        if (strncmp(command, "upload", 6) == 0)
        {
            log_message("INFO", "Handling upload command");
            receive_file(sock);
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
            char *old_name = strtok(command + 7, " ");
            char *new_name = strtok(NULL, " ");
            if (old_name && new_name)
            {
                rename_file(sock, old_name, new_name);
            }
            else
            {
                log_message("ERROR", "Invalid rename command");
                send(sock, "ERROR: Invalid rename command\n", 30, 0);
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
            send(sock, "ERROR: Unknown command\n", 23, 0);
        }
    }

    log_message("INFO", "Client handler finished");
    printf(YELLOW "Client handler finished\n" RESET);
}

// Health monitoring functions
double get_cpu_usage()
{
    FILE *f;
    unsigned long long prevIdle = 0, prevTotal = 0;
    unsigned long long idle, total;
    char buf[256];

    f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    fgets(buf, sizeof(buf), f);
    fclose(f);

    char cpu[5];
    unsigned long long user, nice, system, irq, softirq, steal, guest, guest_nice;
    sscanf(buf, "%s %llu %llu %llu %llu %llu %llu %llu %llu",
           cpu, &user, &nice, &system, &idle, &irq, &softirq, &steal, &guest);

    total = user + nice + system + idle + irq + softirq + steal;
    prevIdle = idle;
    prevTotal = total;

    usleep(100000); // 100ms delay instead of 1 second to avoid blocking

    f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    fgets(buf, sizeof(buf), f);
    fclose(f);
    sscanf(buf, "%s %llu %llu %llu %llu %llu %llu %llu %llu",
           cpu, &user, &nice, &system, &idle, &irq, &softirq, &steal, &guest);
    total = user + nice + system + idle + irq + softirq + steal;

    unsigned long long deltaTotal = total - prevTotal;
    unsigned long long deltaIdle = idle - prevIdle;

    if (deltaTotal == 0)
        return 0;
    return (double)(deltaTotal - deltaIdle) / deltaTotal * 100.0;
}

int get_cpu_temp()
{
    FILE *f;
    int temp_milli;
    f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f)
        return -1;
    fscanf(f, "%d", &temp_milli);
    fclose(f);
    return temp_milli / 1000;
}

double get_disk_usage(const char *path)
{
    struct statvfs st;
    if (statvfs(path, &st) != 0)
        return -1;
    unsigned long long used = (st.f_blocks - st.f_bfree) * st.f_frsize;
    unsigned long long total = st.f_blocks * st.f_frsize;
    return (double)used / total * 100.0;
}

void send_health_info(int sock)
{
    char response[1024];
    char temp_buf[256];

    strcpy(response, "=== SERVER HEALTH INFORMATION ===\n");

    // CPU Usage
    double cpu_usage = get_cpu_usage();
    if (cpu_usage >= 0)
    {
        snprintf(temp_buf, sizeof(temp_buf), "CPU Usage: %.2f%%\n", cpu_usage);
        strcat(response, temp_buf);
    }
    else
    {
        strcat(response, "CPU Usage: Unable to read\n");
    }

    // CPU Temperature
    int cpu_temp = get_cpu_temp();
    if (cpu_temp >= 0)
    {
        snprintf(temp_buf, sizeof(temp_buf), "CPU Temperature: %d °C\n", cpu_temp);
        strcat(response, temp_buf);
    }
    else
    {
        strcat(response, "CPU Temperature: Unable to read\n");
    }

    // Disk Usage
    double disk = get_disk_usage("/");
    if (disk >= 0)
    {
        snprintf(temp_buf, sizeof(temp_buf), "Disk Usage ('/'): %.2f%%\n", disk);
        strcat(response, temp_buf);
    }
    else
    {
        strcat(response, "Disk Usage: Unable to read\n");
    }

    // RAM Usage
    struct sysinfo mem;
    if (sysinfo(&mem) == 0)
    {
        unsigned long long total = mem.totalram;
        unsigned long long free = mem.freeram;
        double ram_usage = (double)(total - free) / total * 100.0;
        snprintf(temp_buf, sizeof(temp_buf), "RAM Usage: %.2f%%\n", ram_usage);
        strcat(response, temp_buf);

        // Total RAM in MB
        snprintf(temp_buf, sizeof(temp_buf), "Total RAM: %.2f MB\n", (double)total / (1024 * 1024));
        strcat(response, temp_buf);

        // Free RAM in MB
        snprintf(temp_buf, sizeof(temp_buf), "Free RAM: %.2f MB\n", (double)free / (1024 * 1024));
        strcat(response, temp_buf);
    }
    else
    {
        strcat(response, "RAM Usage: Unable to read\n");
    }

    // System uptime
    if (sysinfo(&mem) == 0)
    {
        long uptime_hours = mem.uptime / 3600;
        long uptime_minutes = (mem.uptime % 3600) / 60;
        snprintf(temp_buf, sizeof(temp_buf), "System Uptime: %ld hours, %ld minutes\n", uptime_hours, uptime_minutes);
        strcat(response, temp_buf);
    }

    strcat(response, "================================\n");

    send(sock, response, strlen(response), MSG_NOSIGNAL);
    log_message("INFO", "Sent health information to client");
}
