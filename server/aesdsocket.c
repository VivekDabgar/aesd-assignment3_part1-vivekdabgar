/*
 * aesdsocket.c
 *
 * AESD Assignment 5 - stream socket server on port 9000.
 *
 * Behaviour:
 *   - binds a TCP stream socket to port 9000
 *   - accepts one client at a time, forever
 *   - every newline-terminated packet received is appended to
 *     /var/tmp/aesdsocketdata
 *   - after each complete packet the whole file is streamed back to the client
 *   - SIGINT / SIGTERM cause a graceful shutdown and delete the data file
 *   - "-d" runs the server as a daemon (fork happens only after bind succeeds)
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define PORT        "9000"
#define DATA_FILE   "/var/tmp/aesdsocketdata"
#define BACKLOG     10
#define BUF_SIZE    1024

/* ---------------- global state (needed by the signal handler) ------------- */

static volatile sig_atomic_t exit_requested = 0;
static int sockfd   = -1;   /* listening socket   */
static int clientfd = -1;   /* current client fd  */

/* ---------------------------- signal handling ----------------------------- */

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;

    /* shutdown() is async-signal-safe and unblocks a pending accept()/recv() */
    if (clientfd != -1) {
        shutdown(clientfd, SHUT_RDWR);
    }
    if (sockfd != -1) {
        shutdown(sockfd, SHUT_RDWR);
    }
}

static int setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                 /* no SA_RESTART: we want EINTR */

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------ small helpers ----------------------------- */

static int write_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/*
 * Stream the whole data file back to the client in BUF_SIZE chunks.
 * Chunking matters: the file may be larger than the heap we are allowed to use.
 */
static int send_file_to_client(int fd)
{
    char    buf[BUF_SIZE];
    ssize_t nread;
    int     filefd;

    filefd = open(DATA_FILE, O_RDONLY);
    if (filefd < 0) {
        syslog(LOG_ERR, "open %s for read failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    while ((nread = read(filefd, buf, sizeof(buf))) > 0) {
        if (send_all(fd, buf, (size_t)nread) != 0) {
            syslog(LOG_ERR, "send to client failed: %s", strerror(errno));
            close(filefd);
            return -1;
        }
    }

    close(filefd);

    if (nread < 0) {
        syslog(LOG_ERR, "read %s failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }
    return 0;
}

/* --------------------------- per-client handling -------------------------- */

/*
 * Read from the client until it closes the connection.
 * Bytes are accumulated in a growing heap buffer; every time a '\n' shows up
 * the bytes up to and including it are appended to the data file and the whole
 * file is sent back.
 */
static int handle_client(int fd)
{
    char    recv_buf[BUF_SIZE];
    char   *packet      = NULL;
    size_t  packet_len  = 0;
    ssize_t nread;
    int     ret = 0;

    while ((nread = recv(fd, recv_buf, sizeof(recv_buf), 0)) > 0) {
        char *newline;
        char *tmp = realloc(packet, packet_len + (size_t)nread + 1);

        if (tmp == NULL) {
            syslog(LOG_ERR, "malloc failed (%zu bytes), discarding packet",
                   packet_len + (size_t)nread + 1);
            free(packet);
            packet     = NULL;
            packet_len = 0;
            continue;                  /* drop the over-length packet, keep going */
        }
        packet = tmp;

        memcpy(packet + packet_len, recv_buf, (size_t)nread);
        packet_len += (size_t)nread;
        packet[packet_len] = '\0';

        /* one buffer may contain several complete packets */
        while ((newline = memchr(packet, '\n', packet_len)) != NULL) {
            size_t line_len = (size_t)(newline - packet) + 1;
            int    filefd;

            filefd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (filefd < 0) {
                syslog(LOG_ERR, "open %s for append failed: %s",
                       DATA_FILE, strerror(errno));
                ret = -1;
                goto out;
            }
            if (write_all(filefd, packet, line_len) != 0) {
                syslog(LOG_ERR, "write %s failed: %s", DATA_FILE, strerror(errno));
                close(filefd);
                ret = -1;
                goto out;
            }
            close(filefd);

            if (send_file_to_client(fd) != 0) {
                ret = -1;
                goto out;
            }

            /* shift the leftover (incomplete) bytes to the front */
            memmove(packet, packet + line_len, packet_len - line_len);
            packet_len -= line_len;
            packet[packet_len] = '\0';
        }
    }

    if (nread < 0 && errno != EINTR && !exit_requested) {
        syslog(LOG_ERR, "recv failed: %s", strerror(errno));
        ret = -1;
    }

out:
    free(packet);
    return ret;
}

/* ------------------------------- daemonizing ------------------------------ */

static int daemonize(void)
{
    pid_t pid;
    int   devnull;

    pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);            /* parent leaves, shell gets its prompt back */
    }

    if (setsid() < 0) {                /* become session leader, drop the TTY */
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        return -1;
    }
    if (chdir("/") < 0) {              /* do not hold any directory busy */
        syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
        return -1;
    }

    devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        syslog(LOG_ERR, "open /dev/null failed: %s", strerror(errno));
        return -1;
    }
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }
    return 0;
}

/* --------------------------------- cleanup -------------------------------- */

static void cleanup(void)
{
    if (clientfd != -1) {
        close(clientfd);
        clientfd = -1;
    }
    if (sockfd != -1) {
        close(sockfd);
        sockfd = -1;
    }
    unlink(DATA_FILE);
    closelog();
}

/* ----------------------------------- main --------------------------------- */

int main(int argc, char **argv)
{
    bool             daemon_mode = false;
    struct addrinfo  hints;
    struct addrinfo *servinfo = NULL;
    int              yes = 1;
    int              rc;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    /* ---- argument parsing ---- */
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    } else if (argc > 1) {
        syslog(LOG_ERR, "usage: aesdsocket [-d]");
        fprintf(stderr, "usage: %s [-d]\n", argv[0]);
        closelog();
        return -1;
    }

    if (setup_signals() != 0) {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    /* ---- resolve the local address to bind to ---- */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    rc = getaddrinfo(NULL, PORT, &hints, &servinfo);
    if (rc != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rc));
        closelog();
        return -1;
    }

    /* ---- socket ---- */
    sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (sockfd < 0) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        freeaddrinfo(servinfo);
        closelog();
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        freeaddrinfo(servinfo);
        cleanup();
        return -1;
    }

    /* ---- bind ---- */
    if (bind(sockfd, servinfo->ai_addr, servinfo->ai_addrlen) != 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        freeaddrinfo(servinfo);
        cleanup();
        return -1;
    }
    freeaddrinfo(servinfo);
    servinfo = NULL;

    /* ---- daemonize only AFTER a successful bind ---- */
    if (daemon_mode) {
        if (daemonize() != 0) {
            cleanup();
            return -1;
        }
    }

    /* ---- listen ---- */
    if (listen(sockfd, BACKLOG) != 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        cleanup();
        return -1;
    }

    /* ---- accept loop ---- */
    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t          addr_len = sizeof(client_addr);
        char               ip_str[INET_ADDRSTRLEN];

        clientfd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);
        if (clientfd < 0) {
            if (errno == EINTR || exit_requested) {
                break;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str)) == NULL) {
            strncpy(ip_str, "unknown", sizeof(ip_str));
            ip_str[sizeof(ip_str) - 1] = '\0';
        }
        syslog(LOG_INFO, "Accepted connection from %s", ip_str);

        handle_client(clientfd);

        close(clientfd);
        clientfd = -1;
        syslog(LOG_INFO, "Closed connection from %s", ip_str);
    }

    if (exit_requested) {
        syslog(LOG_INFO, "Caught signal, exiting");
    }

    cleanup();
    return 0;
}