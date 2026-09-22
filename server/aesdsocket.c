#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DATA_FILE_PATH "/var/tmp/aesdsocketdata"
#define TIMESTAMP_INTERVAL_SECONDS 10

static volatile sig_atomic_t exit_flag = 0;
static volatile sig_atomic_t signal_caught = 0;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static int server_socket_fd = -1;

typedef struct thread_node {
  pthread_t thread_id;
  int client_socket_fd;
  volatile sig_atomic_t complete;
  struct thread_node* next;
} thread_node_t;

static thread_node_t* thread_list_head = NULL;

static void signal_handler(int signum) {
  (void)signum;
  exit_flag = 1;
  signal_caught = 1;

  if (server_socket_fd >= 0) {
    shutdown(server_socket_fd, SHUT_RDWR);
    close(server_socket_fd);
    server_socket_fd = -1;
  }

  if (remove(DATA_FILE_PATH) != 0 && errno != ENOENT) {
    syslog(LOG_ERR, "Failed to delete file from signal handler: %s",
           strerror(errno));
  }
}

static void add_thread_node(thread_node_t* node) {
  pthread_mutex_lock(&thread_list_mutex);
  node->next = thread_list_head;
  thread_list_head = node;
  pthread_mutex_unlock(&thread_list_mutex);
}

/* Reap threads that have already finished executing so that resources (OS
 * thread handles, valgrind bookkeeping, etc.) are not held onto for the
 * lifetime of the server. Without this, long running servers accumulate an
 * ever growing number of joinable-but-unjoined threads, which especially
 * under valgrind adds enough overhead to make the periodic timestamp thread
 * miss its schedule. */
static void reap_completed_threads(void) {
  thread_node_t* completed_list = NULL;

  pthread_mutex_lock(&thread_list_mutex);
  thread_node_t** indirect = &thread_list_head;
  while (*indirect != NULL) {
    thread_node_t* node = *indirect;
    if (node->complete) {
      *indirect = node->next;
      node->next = completed_list;
      completed_list = node;
    } else {
      indirect = &node->next;
    }
  }
  pthread_mutex_unlock(&thread_list_mutex);

  while (completed_list != NULL) {
    thread_node_t* node = completed_list;
    completed_list = node->next;
    pthread_join(node->thread_id, NULL);
    free(node);
  }
}

static void join_all_threads(void) {
  while (1) {
    thread_node_t* node = NULL;

    pthread_mutex_lock(&thread_list_mutex);
    node = thread_list_head;
    if (node != NULL) {
      thread_list_head = node->next;
    }
    pthread_mutex_unlock(&thread_list_mutex);

    if (node == NULL) {
      break;
    }

    if (node->client_socket_fd >= 0) {
      shutdown(node->client_socket_fd, SHUT_RDWR);
    }

    pthread_join(node->thread_id, NULL);
    free(node);
  }
}

static int append_to_data_file(const char* buffer, size_t len) {
  FILE* data_file = fopen(DATA_FILE_PATH, "a");
  if (data_file == NULL) {
    syslog(LOG_ERR, "Failed to open data file for append: %s", strerror(errno));
    return -1;
  }

  if (len > 0 && fwrite(buffer, 1, len, data_file) != len) {
    syslog(LOG_ERR, "Failed to write packet to data file");
    fclose(data_file);
    return -1;
  }

  if (fclose(data_file) != 0) {
    syslog(LOG_ERR, "Failed to close data file after append: %s",
           strerror(errno));
    return -1;
  }

  return 0;
}

static int send_data_file_content(int client_socket_fd) {
  char send_buffer[1024];
  char* snapshot = NULL;
  size_t snapshot_len = 0;
  int rc = 0;

  /* Read the whole file into memory while holding file_mutex so that the
   * snapshot sent to the client can never observe a partial write from a
   * concurrent append (e.g. a timestamp write in progress). The lock is
   * released before doing any (potentially slow/blocking) network I/O so
   * that a slow client cannot stall other threads' writes to the file. */
  pthread_mutex_lock(&file_mutex);

  FILE* data_file = fopen(DATA_FILE_PATH, "r");
  if (data_file == NULL) {
    syslog(LOG_ERR, "Failed to open data file for reading: %s",
           strerror(errno));
    pthread_mutex_unlock(&file_mutex);
    return -1;
  }

  while (!feof(data_file)) {
    size_t bytes_read = fread(send_buffer, 1, sizeof(send_buffer), data_file);
    if (bytes_read == 0) {
      if (ferror(data_file)) {
        syslog(LOG_ERR, "Failed to read data file");
        rc = -1;
      }
      break;
    }

    char* resized_snapshot = realloc(snapshot, snapshot_len + bytes_read);
    if (resized_snapshot == NULL) {
      syslog(LOG_ERR, "Out of memory while buffering response");
      rc = -1;
      break;
    }
    snapshot = resized_snapshot;
    memcpy(snapshot + snapshot_len, send_buffer, bytes_read);
    snapshot_len += bytes_read;
  }

  if (fclose(data_file) != 0) {
    syslog(LOG_ERR, "Failed to close data file after reading: %s",
           strerror(errno));
    rc = -1;
  }

  pthread_mutex_unlock(&file_mutex);

  if (rc != 0) {
    free(snapshot);
    return -1;
  }

  size_t total_sent = 0;
  while (total_sent < snapshot_len) {
    ssize_t bytes_sent = send(client_socket_fd, snapshot + total_sent,
                              snapshot_len - total_sent, 0);
    if (bytes_sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      syslog(LOG_ERR, "Failed to send response: %s", strerror(errno));
      free(snapshot);
      return -1;
    }
    total_sent += (size_t)bytes_sent;
  }

  free(snapshot);
  return 0;
}

static int write_timestamp_to_file(void) {
  time_t now = time(NULL);
  struct tm current_time;
  char timestamp[256];

  if (localtime_r(&now, &current_time) == NULL) {
    syslog(LOG_ERR, "Failed to get local time");
    return -1;
  }

  if (strftime(timestamp, sizeof(timestamp), "%a, %d %b %Y %H:%M:%S %z",
               &current_time) == 0) {
    syslog(LOG_ERR, "Timestamp formatting failed");
    return -1;
  }

  pthread_mutex_lock(&file_mutex);
  if (append_to_data_file("timestamp:", strlen("timestamp:")) == 0 &&
      append_to_data_file(timestamp, strlen(timestamp)) == 0 &&
      append_to_data_file("\n", 1) == 0) {
    pthread_mutex_unlock(&file_mutex);
    return 0;
  }
  pthread_mutex_unlock(&file_mutex);
  return -1;
}

static void* timestamp_thread(void* arg) {
  (void)arg;
  struct timespec next_deadline;

  if (clock_gettime(CLOCK_MONOTONIC, &next_deadline) != 0) {
    syslog(LOG_ERR, "Failed to initialize timestamp timer: %s",
           strerror(errno));
    return NULL;
  }

  while (!exit_flag) {
    if (write_timestamp_to_file() != 0) {
      /* keep running but do not block forever on a bad timestamp */
    }

    next_deadline.tv_sec += TIMESTAMP_INTERVAL_SECONDS;

    /* Sleep in short chunks (rather than one clock_nanosleep() call all the
     * way out to next_deadline) so exit_flag is re-checked frequently. A
     * signal delivered to another thread does not interrupt this thread's
     * clock_nanosleep(), so sleeping for the full interval in one call can
     * delay process shutdown (and the pthread_join() waiting on this
     * thread) by up to TIMESTAMP_INTERVAL_SECONDS. */
    const long kPollIntervalNs = 100L * 1000 * 1000; /* 100 ms */
    while (!exit_flag) {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        break;
      }

      if (now.tv_sec > next_deadline.tv_sec ||
          (now.tv_sec == next_deadline.tv_sec &&
           now.tv_nsec >= next_deadline.tv_nsec)) {
        break;
      }

      struct timespec chunk_deadline = now;
      chunk_deadline.tv_nsec += kPollIntervalNs;
      if (chunk_deadline.tv_nsec >= 1000000000L) {
        chunk_deadline.tv_nsec -= 1000000000L;
        chunk_deadline.tv_sec += 1;
      }
      if (chunk_deadline.tv_sec > next_deadline.tv_sec ||
          (chunk_deadline.tv_sec == next_deadline.tv_sec &&
           chunk_deadline.tv_nsec > next_deadline.tv_nsec)) {
        chunk_deadline = next_deadline;
      }

      int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &chunk_deadline,
                               NULL);
      if (rc != 0 && rc != EINTR) {
        syslog(LOG_ERR, "Failed to delay timestamp thread: %s", strerror(rc));
        break;
      }
    }
  }

  return NULL;
}

static void* connection_handler(void* arg) {
  thread_node_t* thread_node = (thread_node_t*)arg;
  int client_socket_fd = thread_node->client_socket_fd;
  char receive_buffer[1024];
  char* packet_buffer = NULL;
  size_t packet_length = 0;
  bool packet_complete = false;

  while (!packet_complete) {
    ssize_t bytes_received =
        recv(client_socket_fd, receive_buffer, sizeof(receive_buffer), 0);
    if (bytes_received == 0) {
      break;
    }
    if (bytes_received < 0) {
      if (errno == EINTR) {
        if (exit_flag) {
          break;
        }
        continue;
      }
      syslog(LOG_ERR, "Failed to receive client data: %s", strerror(errno));
      break;
    }

    size_t previous_length = packet_length;
    size_t new_length = previous_length + (size_t)bytes_received;
    char* resized_packet = realloc(packet_buffer, new_length);
    if (resized_packet == NULL) {
      syslog(LOG_ERR, "Out of memory while buffering packet");
      free(packet_buffer);
      packet_buffer = NULL;
      packet_length = 0;
      break;
    }

    packet_buffer = resized_packet;
    memcpy(packet_buffer + previous_length, receive_buffer,
           (size_t)bytes_received);
    packet_length = new_length;

    for (size_t i = previous_length; i < packet_length; i++) {
      if (packet_buffer[i] == '\n') {
        packet_length = i + 1;
        packet_complete = true;
        break;
      }
    }
  }

  if (packet_complete && packet_length > 0) {
    pthread_mutex_lock(&file_mutex);
    if (append_to_data_file(packet_buffer, packet_length) != 0) {
      pthread_mutex_unlock(&file_mutex);
      free(packet_buffer);
      close(client_socket_fd);
      thread_node->client_socket_fd = -1;
      thread_node->complete = 1;
      return NULL;
    }
    pthread_mutex_unlock(&file_mutex);

    if (send_data_file_content(client_socket_fd) != 0) {
      free(packet_buffer);
      close(client_socket_fd);
      thread_node->client_socket_fd = -1;
      thread_node->complete = 1;
      return NULL;
    }
  }

  free(packet_buffer);
  close(client_socket_fd);
  thread_node->client_socket_fd = -1;
  thread_node->complete = 1;
  return NULL;
}

int main(int argc, char* argv[]) {
  int daemon_mode = 0;
  pthread_t timestamp_tid;

  if (argc > 1 && strcmp(argv[1], "-d") == 0) {
    daemon_mode = 1;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if ((sigaction(SIGINT, &sa, NULL) != 0) ||
      (sigaction(SIGTERM, &sa, NULL) != 0)) {
    return -1;
  }

  openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

  if (remove(DATA_FILE_PATH) != 0 && errno != ENOENT) {
    syslog(LOG_ERR, "Failed to remove stale data file: %s", strerror(errno));
  }

  server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_fd < 0) {
    syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
    closelog();
    return -1;
  }

  int reuse_addr = 1;
  if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr,
                 sizeof(reuse_addr)) < 0) {
    syslog(LOG_ERR, "Failed to set SO_REUSEADDR: %s", strerror(errno));
    close(server_socket_fd);
    closelog();
    return -1;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(9000);
  if (bind(server_socket_fd, (struct sockaddr*)&server_addr,
           sizeof(server_addr)) < 0) {
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

  if (pthread_create(&timestamp_tid, NULL, timestamp_thread, NULL) != 0) {
    syslog(LOG_ERR, "Failed to create timestamp thread: %s", strerror(errno));
    close(server_socket_fd);
    closelog();
    return -1;
  }

  while (!exit_flag) {
    reap_completed_threads();

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_socket_fd = accept(
        server_socket_fd, (struct sockaddr*)&client_addr, &client_addr_len);

    if (client_socket_fd < 0) {
      if (errno == EINTR) {
        if (exit_flag) {
          if (signal_caught) {
            syslog(LOG_INFO, "Caught signal, exiting");
            signal_caught = 0;
          }
          break;
        }
        continue;
      }
      syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
      continue;
    }

    char client_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip,
                  sizeof(client_ip)) == NULL) {
      strncpy(client_ip, "unknown", sizeof(client_ip));
      client_ip[sizeof(client_ip) - 1] = '\0';
    }
    syslog(LOG_INFO, "Accepted connection from %s", client_ip);

    thread_node_t* thread_node = malloc(sizeof(*thread_node));
    if (thread_node == NULL) {
      syslog(LOG_ERR, "Failed to allocate thread node");
      close(client_socket_fd);
      continue;
    }

    thread_node->client_socket_fd = client_socket_fd;
    thread_node->complete = 0;
    thread_node->next = NULL;

    if (pthread_create(&thread_node->thread_id, NULL, connection_handler,
                       thread_node) != 0) {
      syslog(LOG_ERR, "Failed to create client thread: %s", strerror(errno));
      free(thread_node);
      close(client_socket_fd);
      continue;
    }

    add_thread_node(thread_node);
  }

  if (server_socket_fd >= 0) {
    shutdown(server_socket_fd, SHUT_RDWR);
    close(server_socket_fd);
    server_socket_fd = -1;
  }

  if (pthread_join(timestamp_tid, NULL) != 0) {
    syslog(LOG_ERR, "Failed to join timestamp thread");
  }

  join_all_threads();

  /* Note: DATA_FILE_PATH is already removed by signal_handler() as soon as
   * shutdown begins. Removing it again here is redundant, and dangerous:
   * thread join above can take a little while, and a second process could
   * already have started and be writing to a freshly created file by the
   * time we get here (the listening socket was freed for reuse the moment
   * signal_handler() closed it). Deleting the file again here would wipe
   * out that new process's data. */

  closelog();
  return 0;
}