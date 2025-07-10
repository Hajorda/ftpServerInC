#include "epoll_server.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    int port = 8080; // Default port

    if (argc > 1)
    {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535)
        {
            fprintf(stderr, "Invalid port number. Using default port 8080.\n");
            port = 8080;
        }
    }

    printf("Starting epoll-based FTP server on port %d\n", port);
    start_epoll_server(port);

    return 0;
}
