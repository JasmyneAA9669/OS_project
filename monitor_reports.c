#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>

#define MONITOR_PID_FILE ".monitor_pid"

static int running = 1;

int write_pid_file(){
    int fd = open(MONITOR_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0){
        perror("open .monitor_pid");
        return -1;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (write(fd, buf, len) < 0) {
        perror("write .monitor_pid");
        close(fd);
        return -1;
    }

    close(fd);
    printf("monitor_reports: started (PID %d), .monitor_pid created\n", getpid());
    return 0;
}

void delete_pid_file(){
    if (unlink(MONITOR_PID_FILE) < 0) {
        perror("unlink .monitor_pid");
    } else {
        printf("monitor_reports: .monitor_pid removed\n");
    }
}

void handle_sigint(int sig){
    (void)sig;
    running = 0;
}

void handle_sigusr1(int sig){
    (void)sig;
    char msg[] = "monitor_reports: received SIGUSR1 - new report added\n";
    write(STDOUT_FILENO, msg, strlen(msg));
}

int main(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction SIGINT");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        perror("sigaction SIGUSR1");
        return 1;
    }

    write_pid_file();

    printf("monitor_reports: waiting for signals\n");

    while (running) {
        pause();
    }

    printf("monitor_reports: received SIGINT, shutting down...\n");
    delete_pid_file();

    return 0;
}