/*
 * aesdsocket.c  --  AESD Assignment 6
 *
 * Assignment 5 behaviour, plus:
 *   STEP 1  connection work moved out of main() into connection_handler()
 *   STEP 2  per-thread "box of stuff"  (struct thread_data)
 *   STEP 3  singly linked list (sys/queue.h) = the manager's clipboard
 *   STEP 4  one mutex = the single pen for the shared logbook
 *   STEP 5  POSIX timer appends "timestamp:<RFC 2822>" every 10 seconds
 *   STEP 6  graceful shutdown: wake accept(), join every thread, clean up
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PORT            "9000"
#define BACKLOG         10
#define RX_BUFFER_SIZE  1024
#define DATA_FILE       "/var/tmp/aesdsocketdata"
#define TIMER_PERIOD_S  10

/* ------------------------------------------------------------------ */
/* STEP 2: everything one clerk needs to do their job, in one struct.  */
/* ------------------------------------------------------------------ */
struct thread_data {
    pthread_t thread_id;
    int       client_fd;
    char      client_ip[INET6_ADDRSTRLEN];
    bool      thread_complete;              /* "I'm done, sign me out" */
    SLIST_ENTRY(thread_data) entries;       /* link on the clipboard   */
};

SLIST_HEAD(thread_list, thread_data);

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t caught_signal = 0;

/* STEP 4: the single pen. Guards every touch of the data file.       */
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

static int  data_fd   = -1;
static int  listen_fd = -1;

static timer_t timer_id;
static bool    timer_created = false;

static struct thread_list head = SLIST_HEAD_INITIALIZER(head);

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

/* Signal handler: touch nothing but the flag. */
static void signal_handler(int signo)
{
    (void)signo;
    caught_signal = 1;
}

/* write() can be short; keep going until the whole buffer is out. */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

/* send() can be short too. */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/*
 * Block/unblock SIGINT+SIGTERM in the *calling* thread.
 *
 * Why: new threads inherit the signal mask of their creator. We block
 * before pthread_create() and before timer_create() so that only the
 * main thread can ever receive the signal -- otherwise the signal might
 * be delivered to a worker and accept() would never be interrupted.
 */
static void set_sigmask(int how)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(how, &set, NULL);
}

/* ------------------------------------------------------------------ */
/* STEP 4: the only place the data file is ever touched.              */
/*                                                                    */
/* The append AND the read-back happen inside one lock, so another    */
/* clerk (or the clock-keeper) can never slip a write in between.     */
/* ------------------------------------------------------------------ */
static int append_and_echo(int client_fd, const char *packet, size_t len)
{
    char    buf[RX_BUFFER_SIZE];
    ssize_t nread;
    int     rc = -1;

    pthread_mutex_lock(&file_mutex);

    if (write_all(data_fd, packet, len) != 0) {
        syslog(LOG_ERR, "write to %s failed: %s", DATA_FILE, strerror(errno));
        goto unlock;
    }

    if (lseek(data_fd, 0, SEEK_SET) == (off_t)-1) {
        syslog(LOG_ERR, "lseek failed: %s", strerror(errno));
        goto unlock;
    }

    while ((nread = read(data_fd, buf, sizeof(buf))) > 0) {
        if (send_all(client_fd, buf, (size_t)nread) != 0) {
            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            goto unlock;
        }
    }
    if (nread < 0) {
        syslog(LOG_ERR, "read failed: %s", strerror(errno));
        goto unlock;
    }

    rc = 0;

unlock:
    /* Every exit path unlocks. A clerk who walks off with the pen
       deadlocks the whole post office. */
    pthread_mutex_unlock(&file_mutex);
    return rc;
}

/* ------------------------------------------------------------------ */
/* STEP 1: the clerk. One instance per client connection.             */
/* ------------------------------------------------------------------ */
static void *connection_handler(void *arg)
{
    struct thread_data *td = (struct thread_data *)arg;

    char   *packet      = NULL;   /* grows until a '\n' shows up */
    size_t  packet_cap  = 0;
    size_t  packet_used = 0;
    char    rxbuf[RX_BUFFER_SIZE];

    while (!caught_signal) {
        ssize_t nread = recv(td->client_fd, rxbuf, sizeof(rxbuf), 0);

        if (nread == 0) {
            break;                              /* client closed */
        }
        if (nread < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            break;
        }

        /* Grow the packet buffer to hold what just arrived. */
        if (packet_used + (size_t)nread > packet_cap) {
            size_t new_cap = packet_cap ? packet_cap : RX_BUFFER_SIZE;
            while (new_cap < packet_used + (size_t)nread)
                new_cap *= 2;

            char *tmp = realloc(packet, new_cap);
            if (tmp == NULL) {
                /* Over-length packet: discard it and carry on. */
                syslog(LOG_ERR, "realloc failed, discarding packet");
                free(packet);
                packet      = NULL;
                packet_cap  = 0;
                packet_used = 0;
                continue;
            }
            packet     = tmp;
            packet_cap = new_cap;
        }

        memcpy(packet + packet_used, rxbuf, (size_t)nread);
        packet_used += (size_t)nread;

        /* One recv() may carry several packets, or none at all. */
        char *nl;
        while ((nl = memchr(packet, '\n', packet_used)) != NULL) {
            size_t pkt_len = (size_t)(nl - packet) + 1;

            if (append_and_echo(td->client_fd, packet, pkt_len) != 0)
                goto done;

            packet_used -= pkt_len;
            memmove(packet, packet + pkt_len, packet_used);
        }
    }

done:
    free(packet);
    close(td->client_fd);
    td->client_fd = -1;
    syslog(LOG_DEBUG, "Closed connection from %s", td->client_ip);

    /* Must be the very last thing: the manager reaps on this flag. */
    td->thread_complete = true;
    return td;
}

/* ------------------------------------------------------------------ */
/* STEP 5: the clock-keeper. Grabs the same pen as everyone else.     */
/* ------------------------------------------------------------------ */
static void timer_callback(union sigval sv)
{
    (void)sv;

    char       stamp[128];
    char       line[160];
    time_t     now = time(NULL);
    struct tm  tm_now;

    if (localtime_r(&now, &tm_now) == NULL)
        return;

    /* RFC 2822: "Thu, 06 Aug 2026 14:30:00 +0200" */
    if (strftime(stamp, sizeof(stamp), "%a, %d %b %Y %T %z", &tm_now) == 0)
        return;

    int len = snprintf(line, sizeof(line), "timestamp:%s\n", stamp);
    if (len <= 0)
        return;

    pthread_mutex_lock(&file_mutex);
    if (write_all(data_fd, line, (size_t)len) != 0)
        syslog(LOG_ERR, "timestamp write failed: %s", strerror(errno));
    pthread_mutex_unlock(&file_mutex);
}

static int start_timer(void)
{
    struct sigevent   sev;
    struct itimerspec its;

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify          = SIGEV_THREAD;
    sev.sigev_notify_function = timer_callback;

    if (timer_create(CLOCK_MONOTONIC, &sev, &timer_id) != 0) {
        syslog(LOG_ERR, "timer_create failed: %s", strerror(errno));
        return -1;
    }
    timer_created = true;

    its.it_value.tv_sec     = TIMER_PERIOD_S;
    its.it_value.tv_nsec    = 0;
    its.it_interval.tv_sec  = TIMER_PERIOD_S;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(timer_id, 0, &its, NULL) != 0) {
        syslog(LOG_ERR, "timer_settime failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* STEP 3: walk the clipboard and sign out anyone who has finished.   */
/*                                                                    */
/* force == false : only reap threads that set thread_complete        */
/* force == true  : shutdown + join everybody (shutdown time)         */
/* ------------------------------------------------------------------ */
static void reap_threads(bool force)
{
    struct thread_data *entry = SLIST_FIRST(&head);
    struct thread_data *next;

    while (entry != NULL) {
        next = SLIST_NEXT(entry, entries);

        if (force || entry->thread_complete) {
            /* Nudge a thread still parked in recv() so it returns. */
            if (force && entry->client_fd >= 0)
                shutdown(entry->client_fd, SHUT_RDWR);

            pthread_join(entry->thread_id, NULL);
            SLIST_REMOVE(&head, entry, thread_data, entries);
            free(entry);
        }
        entry = next;
    }
}

/* ------------------------------------------------------------------ */
/* Setup helpers                                                      */
/* ------------------------------------------------------------------ */
static int setup_socket(void)
{
    struct addrinfo hints, *res = NULL;
    int fd = -1, yes = 1, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    rc = getaddrinfo(NULL, PORT, &hints, &res);
    if (rc != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rc));
        return -1;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        goto err;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        goto err;
    }

    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        goto err;
    }

    freeaddrinfo(res);
    return fd;

err:
    if (fd >= 0) close(fd);
    freeaddrinfo(res);
    return -1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    /* NOTE: no SA_RESTART -- we WANT accept() to return EINTR. */
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    return 0;
}

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0)
        exit(EXIT_SUCCESS);        /* parent leaves */

    if (setsid() < 0) {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/") != 0)
        syslog(LOG_WARNING, "chdir(/) failed: %s", strerror(errno));

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }
    return 0;
}

static void cleanup(void)
{
    if (timer_created)
        timer_delete(timer_id);

    reap_threads(true);            /* STEP 6: everybody goes home */

    if (listen_fd >= 0) close(listen_fd);
    if (data_fd   >= 0) close(data_fd);

    pthread_mutex_destroy(&file_mutex);
    unlink(DATA_FILE);
    closelog();
}

/* ------------------------------------------------------------------ */
/* main: hire clerks, never do the work itself                        */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    bool daemon_mode = (argc == 2 && strcmp(argv[1], "-d") == 0);

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (install_signal_handlers() != 0) {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    listen_fd = setup_socket();
    if (listen_fd < 0) {
        closelog();
        return -1;                 /* required: -1 on setup failure */
    }

    /* Daemonize AFTER a successful bind... */
    if (daemon_mode && daemonize() != 0) {
        close(listen_fd);
        closelog();
        return -1;
    }

    /* ...and everything below happens in the CHILD.
       fork() does not inherit POSIX timers or threads. Start the timer
       in the parent and it silently disappears -- zero timestamps. */

    if (listen(listen_fd, BACKLOG) != 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(listen_fd);
        closelog();
        return -1;
    }

    data_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (data_fd < 0) {
        syslog(LOG_ERR, "open %s failed: %s", DATA_FILE, strerror(errno));
        close(listen_fd);
        closelog();
        return -1;
    }

    SLIST_INIT(&head);

    /* Timer helper thread must not catch our signals either. */
    set_sigmask(SIG_BLOCK);
    if (start_timer() != 0) {
        set_sigmask(SIG_UNBLOCK);
        cleanup();
        return -1;
    }
    set_sigmask(SIG_UNBLOCK);

    /* ---------------- accept loop ---------------- */
    while (!caught_signal) {
        struct sockaddr_in client_addr;
        socklen_t          addr_len = sizeof(client_addr);

        /* Block here. Do NOT poll -- a busy loop starves the workers
           under valgrind and the multithread test fails. */
        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;          /* signal woke us */
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        struct thread_data *td = calloc(1, sizeof(struct thread_data));
        if (td == NULL) {
            syslog(LOG_ERR, "calloc failed for thread data");
            close(client_fd);
            continue;
        }

        td->client_fd       = client_fd;
        td->thread_complete = false;
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  td->client_ip, sizeof(td->client_ip));

        syslog(LOG_DEBUG, "Accepted connection from %s", td->client_ip);

        /* Worker inherits a mask with SIGINT/SIGTERM blocked. */
        set_sigmask(SIG_BLOCK);
        int rc = pthread_create(&td->thread_id, NULL, connection_handler, td);
        set_sigmask(SIG_UNBLOCK);

        if (rc != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
            close(client_fd);
            free(td);
            continue;
        }

        SLIST_INSERT_HEAD(&head, td, entries);

        /* Sign out anyone who finished while we were busy. Done right
           after starting the new thread, not in a polling loop. */
        reap_threads(false);
    }

    syslog(LOG_DEBUG, "Caught signal, exiting");
    cleanup();
    return 0;
}
