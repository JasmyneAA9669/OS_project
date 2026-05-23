#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>

#define MONITOR_PID_FILE ".monitor_pid"

static int running = 1;          // Main loop control flag
static int pipe_fd = -1;         // Pipe fd for hub communication

/* Sends a typed message through the pipe to hub_mon
   Handles partial writes and EINTR interrupts */
static void pipe_msg(const char *type, const char *msg) {
    if (pipe_fd < 0) return;
    
    char full_msg[600];
    int total = snprintf(full_msg, sizeof(full_msg), "%s:%s\n", type, msg);
    if (total > 0 && total < (int)sizeof(full_msg)) {
        int written = 0;
        while (written < total) {
            ssize_t n = write(pipe_fd, full_msg + written, total - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            written += n;
        }
    }
}

/* Creates .monitor_pid file containing current process PID */
int write_pid_file() {
    int fd = open(MONITOR_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to create .monitor_pid: %s", strerror(errno));
        pipe_msg("ERROR", msg);
        return -1;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (write(fd, buf, len) < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to write .monitor_pid: %s", strerror(errno));
        pipe_msg("ERROR", msg);
        close(fd);
        return -1;
    }
    close(fd);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Monitor started (PID %d)", getpid());
    pipe_msg("START", msg);
    if (pipe_fd < 0) printf("%s\n", msg);
    return 0;
}

/* Removes .monitor_pid file on shutdown */
void delete_pid_file() {
    if (unlink(MONITOR_PID_FILE) < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to remove .monitor_pid: %s", strerror(errno));
        pipe_msg("ERROR", msg);
    } else {
        pipe_msg("END", "Monitor shutting down");
        if (pipe_fd < 0) printf("Monitor shutting down\n");
    }
}

/* SIGINT handler - sets running flag to 0 for clean shutdown */
void handle_sigint(int sig) {
    (void)sig;
    if (pipe_fd < 0) printf("Received SIGINT, shutting down...\n");
    pipe_msg("END", "Received SIGINT, shutting down...");
    running = 0;
}

/* SIGUSR1 handler - called when city_manager adds a new report */
void handle_sigusr1(int sig) {
    (void)sig;
    if (pipe_fd < 0) printf("New report notification received\n");
    pipe_msg("EVENT", "New report added");
}

int main(int argc, char *argv[]) {
    // If pipe fd provided as argument, monitor runs under city_hub
    if (argc >= 2) {
        pipe_fd = atoi(argv[1]);
    }

    // Check if another monitor is already running
    int fd = open(MONITOR_PID_FILE, O_RDONLY);
    if (fd >= 0) {
        char buf[32];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        
        if (n > 0) {
            buf[n] = '\0';
            pid_t existing_pid = (pid_t)strtol(buf, NULL, 10);
            
            // Check if the process actually exists
            if (existing_pid > 0 && kill(existing_pid, 0) == 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Monitor already running with PID %d", existing_pid);
                pipe_msg("ERROR", msg);
                if (pipe_fd < 0) {
                    fprintf(stderr, "%s\n", msg);
                }
                if (pipe_fd >= 0) close(pipe_fd);
                return 1;
            }
            // Stale PID file, remove it
            unlink(MONITOR_PID_FILE);
        } else {
            unlink(MONITOR_PID_FILE);  // Empty file
        }
    }

    // Set up signal handlers using sigaction
    struct sigaction sa;
    
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction SIGINT");
        if (pipe_fd >= 0) close(pipe_fd);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        perror("sigaction SIGUSR1");
        if (pipe_fd >= 0) close(pipe_fd);
        return 1;
    }

    if (write_pid_file() < 0) {
        if (pipe_fd >= 0) close(pipe_fd);
        return 1;
    }

    // Main loop - wait for signals
    while (running) {
        pause();  // Suspend until a signal arrives
    }

    delete_pid_file();
    if (pipe_fd >= 0) close(pipe_fd);
    return 0;
}