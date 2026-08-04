#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>

#define DATA_FILE_PATH "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t exit_flag = 0;
static volatile sig_atomic_t signal_caught = 0;

static void signal_handler(int signum)
{
    (void)signum;
    exit_flag = 1;
    signal_caught = 1;
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    int server_socket_fd = -1;
    int client_socket_fd = -1;

    // Register signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if ((sigaction(SIGINT, &sa, NULL) != 0) || (sigaction(SIGTERM, &sa, NULL) != 0)) {
        return -1;
    }

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
        closelog();
        return -1;
    }

    // Allow immediate rebinding after restart while previous connections may be in TIME_WAIT.
    int reuse_addr = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0) {
        syslog(LOG_ERR, "Failed to set SO_REUSEADDR: %s", strerror(errno));
        close(server_socket_fd);
        closelog();
        return -1;
    }

    // Bind the socket to port 9000
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(9000);
    if (bind(server_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Failed to bind socket: %s", strerror(errno));
        close(server_socket_fd);
        closelog();
        return -1;
    }

    if (listen(server_socket_fd, 5) < 0) {
        syslog(LOG_ERR, "Failed to listen on socket: %s", strerror(errno));
        close(server_socket_fd);
        closelog();
        return -1;
    }

    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "Failed to daemonize: %s", strerror(errno));
            close(server_socket_fd);
            closelog();
            return -1;
        }
        if (pid > 0) {
            close(server_socket_fd);
            closelog();
            return 0;
        }

        if (setsid() < 0) {
            syslog(LOG_ERR, "Failed to create new session: %s", strerror(errno));
            close(server_socket_fd);
            closelog();
            return -1;
        }

        int dev_null = open("/dev/null", O_RDWR);
        if (dev_null >= 0) {
            dup2(dev_null, STDIN_FILENO);
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }

        if (chdir("/") != 0) {
            syslog(LOG_ERR, "Failed to change directory to /: %s", strerror(errno));
        }
    }

    while (!exit_flag) {
        // Accept a connection
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket_fd < 0) {
            if (errno == EINTR && exit_flag) {
                if (signal_caught) {
                    syslog(LOG_INFO, "Caught signal, exiting");
                    signal_caught = 0;
                }
                break;
            }
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == NULL) {
            strncpy(client_ip, "unknown", sizeof(client_ip));
            client_ip[sizeof(client_ip) - 1] = '\0';
        }
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Receive one newline-terminated packet.
        char rxbuf[1024];
        char *packet = NULL;
        size_t packet_len = 0;
        int packet_complete = 0;
        while (!packet_complete) {
            ssize_t bytes_received = recv(client_socket_fd, rxbuf, sizeof(rxbuf), 0);
            if (bytes_received == 0) {
                break;
            }
            if (bytes_received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                syslog(LOG_ERR, "Failed to receive data: %s", strerror(errno));
                break;
            }

            size_t old_len = packet_len;
            size_t new_len = old_len + (size_t)bytes_received;
            char *new_packet = realloc(packet, new_len);
            if (new_packet == NULL) {
                syslog(LOG_ERR, "Out of memory while buffering packet");
                free(packet);
                packet = NULL;
                packet_len = 0;
                break;
            }
            packet = new_packet;
            memcpy(packet + old_len, rxbuf, (size_t)bytes_received);
            packet_len = new_len;

            for (size_t i = old_len; i < packet_len; i++) {
                if (packet[i] == '\n') {
                    packet_len = i + 1;
                    packet_complete = 1;
                    break;
                }
            }
        }

        if (packet_complete && packet_len > 0) {
            FILE *file = fopen(DATA_FILE_PATH, "a");
            if (!file) {
                syslog(LOG_ERR, "Failed to open data file: %s", strerror(errno));
                free(packet);
                close(client_socket_fd);
                syslog(LOG_INFO, "Closed connection from %s", client_ip);
                continue;
            }
            if (fwrite(packet, 1, packet_len, file) != packet_len) {
                syslog(LOG_ERR, "Failed to write packet to data file");
            }
            if (fclose(file) != 0) {
                syslog(LOG_ERR, "Failed to close data file: %s", strerror(errno));
            }
        }
        free(packet);

        if (!packet_complete) {
            syslog(LOG_INFO, "Closed connection from %s", client_ip);
            close(client_socket_fd);
            continue;
        }

        // Send the full content of the file back to the client
        FILE *file = fopen(DATA_FILE_PATH, "r");
        if (!file) {
            syslog(LOG_ERR, "Failed to open file for reading: %s", strerror(errno));
            close(client_socket_fd);
            syslog(LOG_INFO, "Closed connection from %s", client_ip);
            continue;
        }

        while (1) {
            size_t bytes_read = fread(rxbuf, 1, sizeof(rxbuf), file);
            if (bytes_read == 0) {
                if (ferror(file)) {
                    syslog(LOG_ERR, "Failed to read data file");
                }
                break;
            }

            size_t sent_total = 0;
            while (sent_total < bytes_read) {
                ssize_t sent = send(client_socket_fd, rxbuf + sent_total, bytes_read - sent_total, 0);
                if (sent < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    syslog(LOG_ERR, "Failed to send response: %s", strerror(errno));
                    sent_total = bytes_read;
                    break;
                }
                sent_total += (size_t)sent;
            }
        }
        if (fclose(file) != 0) {
            syslog(LOG_ERR, "Failed to close file after reading: %s", strerror(errno));
        }

        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        close(client_socket_fd);
    }

    close(server_socket_fd);

    if (remove(DATA_FILE_PATH) != 0 && errno != ENOENT) {
        syslog(LOG_ERR, "Failed to delete file: %s", strerror(errno));
    }
    closelog();
    return 0;
}