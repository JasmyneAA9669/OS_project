#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <stdarg.h>

#define MONITOR_PID_FILE ".monitor_pid"

static int running = 1;
static int pipe_fd = -1;

static void pipe_msg(const char *type, const char *format, ...) {
    if (pipe_fd < 0) return;
    
    char buf[512];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    
    if (len > 0 && len < (int)sizeof(buf)) {
        char full_msg[600];
        int total = snprintf(full_msg, sizeof(full_msg), "%s:%s", type, buf);
        if (total > 0) {
            write(pipe_fd, full_msg, total);
        }
    }
}

int write_pid_file() {
    int fd = open(MONITOR_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        pipe_msg("ERROR", "Failed to create .monitor_pid: %s", strerror(errno));
        return -1;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    write(fd, buf, len);
    close(fd);
    
    pipe_msg("START", "Monitor started (PID %d)", getpid());
    return 0;
}

void delete_pid_file() {
    if (unlink(MONITOR_PID_FILE) < 0) {
        pipe_msg("ERROR", "Failed to remove .monitor_pid: %s", strerror(errno));
    } else {
        pipe_msg("END", "Monitor shutting down");
    }
}

void handle_sigint(int sig) {
    (void)sig;
    pipe_msg("END", "Received SIGINT, shutting down...");
    running = 0;
}

void handle_sigusr1(int sig) {
    (void)sig;
    pipe_msg("EVENT", "New report added");
}

int main(int argc, char *argv[]) {
    if (argc >= 2) {
        pipe_fd = atoi(argv[1]);
    }

    // Check for existing monitor
    int fd = open(MONITOR_PID_FILE, O_RDONLY);
    if (fd >= 0) {
        char buf[32];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        
        if (n > 0) {
            buf[n] = '\0';
            pid_t existing_pid = (pid_t)strtol(buf, NULL, 10);
            
            if (kill(existing_pid, 0) == 0) {
                pipe_msg("ERROR", "Monitor already running with PID %d", existing_pid);
                if (pipe_fd >= 0) close(pipe_fd);
                return 1;
            }
            // Stale PID file
            unlink(MONITOR_PID_FILE);
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    if (write_pid_file() < 0) {
        if (pipe_fd >= 0) close(pipe_fd);
        return 1;
    }

    while (running) {
        pause();
    }

    delete_pid_file();
    if (pipe_fd >= 0) close(pipe_fd);
    return 0;
}