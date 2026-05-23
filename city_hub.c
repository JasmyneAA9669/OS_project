#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_INPUT 1024
#define MAX_ARGS  64

static pid_t hub_mon_pid = -1;        // PID of hub_mon process
static int monitor_pipe_read = -1;    // Read end of pipe from hub_mon

/* Helper: writes a string to stdout using write() system call */
static void write_msg(const char *msg) {
    write(STDOUT_FILENO, msg, strlen(msg));
}

/* Reads messages from monitor pipe (non-blocking) and displays them
   Handles message types: START, ERROR, EVENT, END */
void read_monitor_messages() {
    if (monitor_pipe_read < 0) return;

    // Set pipe to non-blocking mode for polling
    int flags = fcntl(monitor_pipe_read, F_GETFL, 0);
    fcntl(monitor_pipe_read, F_SETFL, flags | O_NONBLOCK);

    char buf[1024];
    ssize_t n;
    
    while ((n = read(monitor_pipe_read, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        
        char *line_start = buf;
        char *newline;
        
        // Process complete lines - messages are newline-terminated
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            
            // Parse message type prefix
            if (strncmp(line_start, "START:", 6) == 0) {
                char out[1024];
                int len = snprintf(out, sizeof(out), "[MONITOR] %s\n", line_start + 6);
                if (len > 0) write(STDOUT_FILENO, out, len);
            }
            else if (strncmp(line_start, "ERROR:", 6) == 0) {
                char out[1024];
                int len = snprintf(out, sizeof(out), "[MONITOR ERROR] %s\n", line_start + 6);
                if (len > 0) write(STDOUT_FILENO, out, len);
                
                // If monitor already running, clean up
                if (strstr(line_start + 6, "already running") != NULL) {
                    close(monitor_pipe_read);
                    monitor_pipe_read = -1;
                    hub_mon_pid = -1;
                    return;
                }
            }
            else if (strncmp(line_start, "EVENT:", 6) == 0) {
                char out[1024];
                int len = snprintf(out, sizeof(out), "[MONITOR EVENT] %s\n", line_start + 6);
                if (len > 0) write(STDOUT_FILENO, out, len);
            }
            else if (strncmp(line_start, "END:", 4) == 0) {
                char out[1024];
                int len = snprintf(out, sizeof(out), "[MONITOR] %s\n", line_start + 4);
                if (len > 0) write(STDOUT_FILENO, out, len);
                
                // Monitor shut down
                close(monitor_pipe_read);
                monitor_pipe_read = -1;
                hub_mon_pid = -1;
                return;
            }
            else {
                char out[1024];
                int len = snprintf(out, sizeof(out), "[MONITOR] %s\n", line_start);
                if (len > 0) write(STDOUT_FILENO, out, len);
            }
            
            line_start = newline + 1;
        }
    }

    // Restore pipe to blocking mode
    if (monitor_pipe_read >= 0) {
        fcntl(monitor_pipe_read, F_SETFL, flags);
    }
}

/* Creates hub_mon process which in turn forks monitor_reports
   Process hierarchy: city_hub -> hub_mon -> monitor_reports
   hub_mon creates pipe for monitor and forwards messages to city_hub */
void start_monitor() {
    if (hub_mon_pid > 0) {
        char msg[256];
        int len = snprintf(msg, sizeof(msg), "Monitor is already running (hub_mon PID: %d)\n", hub_mon_pid);
        if (len > 0) write(STDOUT_FILENO, msg, len);
        return;
    }

    // Pipe from hub_mon to city_hub
    int city_to_hubmon_pipe[2];
    if (pipe(city_to_hubmon_pipe) < 0) {
        write_msg("Error: pipe failed\n");
        return;
    }

    pid_t pid = fork();

    if (pid < 0) {
        write_msg("Error: fork failed\n");
        close(city_to_hubmon_pipe[0]);
        close(city_to_hubmon_pipe[1]);
        return;
    }

    if (pid == 0) {
        // === hub_mon process ===
        close(city_to_hubmon_pipe[0]);

        // Pipe from monitor to hub_mon
        int hubmon_to_monitor_pipe[2];
        if (pipe(hubmon_to_monitor_pipe) < 0) {
            write_msg("Error: pipe failed\n");
            exit(1);
        }

        pid_t mon_pid = fork();

        if (mon_pid < 0) {
            write_msg("Error: fork monitor failed\n");
            exit(1);
        }

        if (mon_pid == 0) {
            // === monitor_reports process ===
            close(hubmon_to_monitor_pipe[0]);
            
            // Pass pipe fd to monitor
            char pipe_str[16];
            snprintf(pipe_str, sizeof(pipe_str), "%d", hubmon_to_monitor_pipe[1]);
            
            execl("./monitor_reports", "monitor_reports", pipe_str, NULL);
            write_msg("Error: execl monitor_reports failed\n");
            exit(1);
        }

        // hub_mon forwards messages from monitor to city_hub
        close(hubmon_to_monitor_pipe[1]);
        
        char buf[1024];
        ssize_t n;
        
        while ((n = read(hubmon_to_monitor_pipe[0], buf, sizeof(buf))) > 0) {
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(city_to_hubmon_pipe[1], buf + written, n - written);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                written += w;
            }
            if (written < n) break;
        }
        
        close(hubmon_to_monitor_pipe[0]);
        close(city_to_hubmon_pipe[1]);
        
        int status;
        waitpid(mon_pid, &status, 0);
        exit(0);
    }

    // === city_hub (parent) ===
    close(city_to_hubmon_pipe[1]);
    monitor_pipe_read = city_to_hubmon_pipe[0];
    hub_mon_pid = pid;

    char msg[256];
    int len = snprintf(msg, sizeof(msg), "Monitor started (hub_mon PID: %d)\n", pid);
    if (len > 0) write(STDOUT_FILENO, msg, len);
    
    usleep(100000);  // Wait 100ms for monitor to initialize
    read_monitor_messages();
}

/* Spawns scorer processes for each district and collects results
   Uses pipes with dup2() to redirect scorer stdout */
void calculate_scores(char *districts[], int count) {
    if (count == 0) {
        write_msg("Usage: calculate_scores <district1> [district2 ...]\n");
        return;
    }

    write_msg("\n=== Calculating Workload Scores ===\n\n");

    int pipes[count][2];
    pid_t pids[count];
    
    // Initialize arrays
    for (int i = 0; i < count; i++) {
        pipes[i][0] = -1;
        pipes[i][1] = -1;
        pids[i] = -1;
    }

    // Fork scorer for each district
    for (int i = 0; i < count; i++) {
        if (pipe(pipes[i]) < 0) {
            write_msg("Error: pipe failed\n");
            for (int j = 0; j < i; j++) {
                if (pipes[j][0] >= 0) close(pipes[j][0]);
                if (pipes[j][1] >= 0) close(pipes[j][1]);
            }
            return;
        }

        pid_t pid = fork();

        if (pid < 0) {
            write_msg("Error: fork failed\n");
            close(pipes[i][0]);
            close(pipes[i][1]);
            for (int j = 0; j < i; j++) {
                kill(pids[j], SIGTERM);
                waitpid(pids[j], NULL, 0);
                if (pipes[j][0] >= 0) close(pipes[j][0]);
            }
            return;
        }

        if (pid == 0) {
            // Scorer process
            close(pipes[i][0]);

            // Redirect stdout to pipe using dup2()
            if (dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                write_msg("Error: dup2 failed\n");
                exit(1);
            }
            close(pipes[i][1]);

            execl("./scorer", "scorer", districts[i], NULL);
            write_msg("Error: execl scorer failed\n");
            exit(1);
        }

        close(pipes[i][1]);
        pipes[i][1] = -1;
        pids[i] = pid;
    }

    // Collect and display results from all scorers
    for (int i = 0; i < count; i++) {
        char header[256];
        int len = snprintf(header, sizeof(header), "--- District: %s ---\n", districts[i]);
        if (len > 0) write(STDOUT_FILENO, header, len);
        
        char buf[1024];
        ssize_t n;
        int has_data = 0;
        
        while ((n = read(pipes[i][0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            write(STDOUT_FILENO, buf, n);
            has_data = 1;
        }
        
        if (!has_data) {
            write_msg("  No data received from scorer\n");
        }
        
        close(pipes[i][0]);
        pipes[i][0] = -1;
        
        int status;
        waitpid(pids[i], &status, 0);
        
        // Check scorer exit status
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            char warn[256];
            len = snprintf(warn, sizeof(warn), 
                          "  Warning: Scorer for '%s' exited with code %d\n", 
                          districts[i], WEXITSTATUS(status));
            if (len > 0) write(STDOUT_FILENO, warn, len);
        } else if (WIFSIGNALED(status)) {
            char warn[256];
            len = snprintf(warn, sizeof(warn), 
                          "  Warning: Scorer for '%s' killed by signal %d\n", 
                          districts[i], WTERMSIG(status));
            if (len > 0) write(STDOUT_FILENO, warn, len);
        }
        
        write_msg("\n");
    }
    
    write_msg("=== Workload Report Complete ===\n\n");
}

int main() {
    char input[MAX_INPUT];
    
    write_msg("╔════════════════════════════════════╗\n");
    write_msg("║   City Infrastructure Hub v1.0    ║\n");
    write_msg("╚════════════════════════════════════╝\n\n");
    write_msg("Commands:\n");
    write_msg("  start_monitor                     - Start background monitor\n");
    write_msg("  calculate_scores <districts...>   - Calculate inspector workloads\n");
    write_msg("  exit                              - Exit the hub\n\n");

    while (1) {
        read_monitor_messages();
        
        write(STDOUT_FILENO, "city_hub> ", 10);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            write_msg("\n");
            break;
        }

        // Handle input that exceeds buffer size
        if (strchr(input, '\n') == NULL) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
            write_msg("Warning: Input too long, truncated\n");
        }

        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) continue;

        // Parse command and arguments
        char *args[MAX_ARGS];
        int arg_count = 0;
        
        char *token = strtok(input, " ");
        while (token != NULL && arg_count < MAX_ARGS - 1) {
            args[arg_count++] = token;
            token = strtok(NULL, " ");
        }
        args[arg_count] = NULL;

        if (arg_count == 0) continue;

        // Execute commands
        if (strcmp(args[0], "exit") == 0) {
            // Stop monitor before exiting
            if (hub_mon_pid > 0) {
                write_msg("Stopping monitor...\n");
                
                // Read monitor PID and send SIGINT
                int fd = open(".monitor_pid", O_RDONLY);
                if (fd >= 0) {
                    char buf[32];
                    ssize_t n = read(fd, buf, sizeof(buf) - 1);
                    close(fd);
                    
                    if (n > 0) {
                        buf[n] = '\0';
                        pid_t mon_pid = (pid_t)strtol(buf, NULL, 10);
                        if (mon_pid > 0) {
                            kill(mon_pid, SIGINT);
                        }
                    }
                }
                
                int status;
                waitpid(hub_mon_pid, &status, 0);
                
                if (monitor_pipe_read >= 0) {
                    close(monitor_pipe_read);
                    monitor_pipe_read = -1;
                }
                hub_mon_pid = -1;
            }
            write_msg("Goodbye!\n");
            break;
        }
        else if (strcmp(args[0], "start_monitor") == 0) {
            start_monitor();
        }
        else if (strcmp(args[0], "calculate_scores") == 0) {
            if (arg_count < 2) {
                write_msg("Usage: calculate_scores <district1> [district2 ...]\n");
            } else {
                calculate_scores(&args[1], arg_count - 1);
            }
        }
        else {
            char err[256];
            int len = snprintf(err, sizeof(err), "Unknown command: %s\n", args[0]);
            if (len > 0) write(STDOUT_FILENO, err, len);
            write_msg("Available commands: start_monitor, calculate_scores, exit\n");
        }
    }

    return 0;
}