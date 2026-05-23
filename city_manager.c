#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>      // File control (open, O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_APPEND, O_TRUNC)
#include <unistd.h>     // POSIX API (read, write, close, lseek, ftruncate, fork, unlink, symlink, chmod)
#include <sys/stat.h>   // File metadata (stat, lstat, fstat, chmod, mkdir, umask)
#include <sys/types.h>  // System types (mode_t, off_t, pid_t)
#include <sys/wait.h>   // Process waiting (wait)
#include <time.h>       // Time functions (time, localtime, strftime)
#include <errno.h>      // Error numbers (errno)
#include <signal.h>     // Signal handling (kill)

#define MAX_NAME     64
#define MAX_CATEGORY 32
#define MAX_DESC     256
#define MAX_REPORTS  10000

/* Fixed-size record structure for binary storage in reports.dat */
typedef struct {
    int     id;
    char    inspector[MAX_NAME];
    double  latitude;
    double  longitude;
    char    category[MAX_CATEGORY];
    int     severity;
    time_t  timestamp;
    char    description[MAX_DESC];
} Report;

/* Safely converts string to integer with error checking
   Returns 1 on success, 0 on failure */
static int parse_int(const char *str, int *out, const char *arg_name) {
    char *endptr;
    errno = 0;
    long val = strtol(str, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || endptr == str) {
        fprintf(stderr, "Error: '%s' is not a valid integer for %s\n", str, arg_name);
        return 0;
    }
    *out = (int)val;
    return 1;
}

/* Checks if a district directory exists */
static int district_exists(const char *district) {
    struct stat st;
    return (stat(district, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Converts permission bits to symbolic string (e.g., rwxr-x---)
   Self-implemented as required by the project specification */
void mode_to_string(mode_t mode, char *str) {
    str[0] = (mode & S_IRUSR) ? 'r' : '-';
    str[1] = (mode & S_IWUSR) ? 'w' : '-';
    str[2] = (mode & S_IXUSR) ? 'x' : '-';
    str[3] = (mode & S_IRGRP) ? 'r' : '-';
    str[4] = (mode & S_IWGRP) ? 'w' : '-';
    str[5] = (mode & S_IXGRP) ? 'x' : '-';
    str[6] = (mode & S_IROTH) ? 'r' : '-';
    str[7] = (mode & S_IWOTH) ? 'w' : '-';
    str[8] = (mode & S_IXOTH) ? 'x' : '-';
    str[9] = '\0';
}

/* Verifies if a role has the required read/write permissions on a file
   Managers are owners (check USR bits), inspectors are group (check GRP bits)
   Called before every role-restricted operation */
int check_permission(const char *path, const char *role, int read, int write) {
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "stat '%s': %s\n", path, strerror(errno));
        return 0;
    }

    if (strcmp(role, "manager") == 0) {
        // Managers are owners - check owner bits
        if (read && !(st.st_mode & S_IRUSR)) {
            fprintf(stderr, "Permission denied: manager lacks read permission on %s\n", path);
            return 0;
        }
        if (write && !(st.st_mode & S_IWUSR)) {
            fprintf(stderr, "Permission denied: manager lacks write permission on %s\n", path);
            return 0;
        }
        return 1;
    } else if (strcmp(role, "inspector") == 0) {
        // Inspectors are group members - check group bits
        if (read && !(st.st_mode & S_IRGRP)) {
            fprintf(stderr, "Permission denied: inspector lacks read permission on %s\n", path);
            return 0;
        }
        if (write && !(st.st_mode & S_IWGRP)) {
            fprintf(stderr, "Permission denied: inspector lacks write permission on %s\n", path);
            return 0;
        }
        return 1;
    }

    fprintf(stderr, "Error: unknown role '%s'\n", role);
    return 0;
}

/* Records all operations in the district's logged_district file
   Format: timestamp  user  role  action
   Only managers can write to log (644 permissions) */
void write_log(const char *district, const char *role, const char *user, const char *action) {
    char path[256];
    snprintf(path, sizeof(path), "%s/logged_district", district);

    // Check write permission before opening
    struct stat st;
    if (stat(path, &st) == 0) {
        if (strcmp(role, "manager") != 0) {
            return;
        }
        if (!(st.st_mode & S_IWUSR)) {
            fprintf(stderr, "Error: log file is not writable by manager\n");
            return;
        }
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        fprintf(stderr, "Warning: could not open log '%s': %s\n", path, strerror(errno));
        return;
    }

    time_t now = time(NULL);
    char entry[512];
    int len = snprintf(entry, sizeof(entry), "%ld\t%s\t%s\t%s\n",
                       (long)now, user, role, action);

    if (write(fd, entry, len) < 0) {
        fprintf(stderr, "Warning: failed to write log entry: %s\n", strerror(errno));
    }

    close(fd);
}

/* Creates symbolic link: active_reports-<district> -> <district>/reports.dat */
void create_symlink(const char *district) {
    char link_name[256];
    char target[256];

    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);
    snprintf(target,    sizeof(target),    "%s/reports.dat",    district);

    unlink(link_name);  // Remove existing symlink if present

    if (symlink(target, link_name) < 0) {
        fprintf(stderr, "symlink '%s': %s\n", link_name, strerror(errno));
        return;
    }

    printf("Symlink '%s' -> '%s' created\n", link_name, target);
}

/* Checks symlink status using lstat() to detect dangling links
   Uses lstat() instead of stat() to avoid following the symlink */
void check_symlink(const char *district) {
    char link_name[256];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);

    struct stat lst;
    if (lstat(link_name, &lst) < 0) {
        printf("No symlink found for district '%s'\n", district);
        return;
    }

    if (S_ISLNK(lst.st_mode)) {
        struct stat st;
        if (stat(link_name, &st) < 0) {
            printf("Warning: symlink '%s' is dangling (target does not exist)\n", link_name);
        } else {
            printf("Symlink '%s' is valid\n", link_name);
        }
    }
}

/* Creates a new district with proper directory structure:
   - District directory (750 - rwxr-x---)
   - reports.dat (664 - rw-rw-r--)
   - district.cfg (640 - rw-r-----) with default threshold=2
   - logged_district (644 - rw-r--r--)
   - active_reports-<district> symlink */
void init_district(const char *district) {
    umask(0);
    char path[256];

    // Create district directory
    if (mkdir(district, 0750) < 0) {
        if (errno != EEXIST) {
            fprintf(stderr, "Error: cannot create district '%s': %s\n", district, strerror(errno));
            return;
        }
    }
    chmod(district, 0750);

    // Create reports.dat
    snprintf(path, sizeof(path), "%s/reports.dat", district);
    int fd = open(path, O_WRONLY | O_CREAT, 0664);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot create '%s': %s\n", path, strerror(errno));
        return;
    }
    close(fd);
    chmod(path, 0664);

    // Create district.cfg with default threshold
    snprintf(path, sizeof(path), "%s/district.cfg", district);
    fd = open(path, O_WRONLY | O_CREAT, 0640);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot create '%s': %s\n", path, strerror(errno));
        return;
    }
    char default_cfg[] = "threshold=2\n";
    if (write(fd, default_cfg, strlen(default_cfg)) < 0) {
        fprintf(stderr, "Warning: could not write default config to '%s'\n", path);
    }
    close(fd);
    chmod(path, 0640);

    // Create logged_district
    snprintf(path, sizeof(path), "%s/logged_district", district);
    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot create '%s': %s\n", path, strerror(errno));
        return;
    }
    close(fd);
    chmod(path, 0644);

    create_symlink(district);
}

/* Comparison function for qsort - sorts reports by ID ascending */
static int compare_reports_by_id(const void *a, const void *b) {
    const Report *ra = (const Report *)a;
    const Report *rb = (const Report *)b;
    return ra->id - rb->id;
}

/* Notifies the monitor process (if running) about new report via SIGUSR1
   Reads monitor PID from .monitor_pid file
   Logs success or failure to the district log */
void notify_monitor(const char *district, const char *role, const char *user) {
    int fd = open(".monitor_pid", O_RDONLY);
    if (fd < 0) {
        char action[128];
        snprintf(action, sizeof(action), "add_report (monitor not notified: %s)", strerror(errno));
        write_log(district, role, user, action);
        return;
    }

    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        char action[128];
        snprintf(action, sizeof(action), "add_report (monitor not notified: failed to read PID)");
        write_log(district, role, user, action);
        return;
    }

    buf[n] = '\0';
    
    // Remove trailing newline if present
    if (buf[n-1] == '\n') {
        buf[n-1] = '\0';
    }
    
    char *endptr;
    errno = 0;
    long val = strtol(buf, &endptr, 10);
    
    // Validate PID format
    if (endptr == buf || *endptr != '\0' || val <= 0 || val > 99999 || errno != 0) {
        char action[128];
        snprintf(action, sizeof(action), "add_report (monitor not notified: invalid PID format)");
        write_log(district, role, user, action);
        return;
    }
    
    pid_t monitor_pid = (pid_t)val;

    // Send SIGUSR1 to notify monitor of new report
    if (kill(monitor_pid, SIGUSR1) < 0) {
        char action[128];
        snprintf(action, sizeof(action), "add_report (monitor not notified: %s)", strerror(errno));
        write_log(district, role, user, action);
        return;
    }

    write_log(district, role, user, "add_report (monitor notified)");
}

/* Adds a new report to the district (both roles allowed)
   Auto-initializes district if it doesn't exist
   Prompts for: GPS coordinates, category, severity, description
   Assigns the next available report ID
   Notifies monitor via SIGUSR1 */
void add_report(const char *district, const char *user, const char *role) {
    struct stat dst;
    if (stat(district, &dst) != 0) {
        init_district(district);
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    // Check write permission (both roles can write to reports.dat)
    if (!check_permission(path, role, 0, 1)) {
        write_log(district, role, user, "add_report_denied");
        return;
    }

    double latitude, longitude;
    char   category[MAX_CATEGORY];
    int    severity;
    char   description[MAX_DESC];

    // Collect report data from user
    printf("X: ");
    if (scanf("%lf", &latitude) != 1) {
        fprintf(stderr, "Error: invalid latitude\n");
        return;
    }

    printf("Y: ");
    if (scanf("%lf", &longitude) != 1) {
        fprintf(stderr, "Error: invalid longitude\n");
        return;
    }

    printf("Category (road/lighting/flooding/other): ");
    if (scanf("%31s", category) != 1) {
        fprintf(stderr, "Error: invalid category\n");
        return;
    }

    printf("Severity level (1/2/3): ");
    if (scanf("%d", &severity) != 1 || severity < 1 || severity > 3) {
        fprintf(stderr, "Error: severity must be 1, 2, or 3\n");
        return;
    }

    printf("Description: ");
    // Clear any leftover characters from stdin
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
    if (fgets(description, MAX_DESC, stdin) == NULL) {
        fprintf(stderr, "Error: could not read description\n");
        return;
    }
    description[strcspn(description, "\n")] = '\0';

    // Find next available report ID
    int next_id = 1;
    int fd_read = open(path, O_RDONLY);
    if (fd_read >= 0) {
        int *ids = calloc(MAX_REPORTS + 1, sizeof(int));
        if (!ids) {
            fprintf(stderr, "Error: out of memory\n");
            close(fd_read);
            return;
        }
        Report temp;
        while (read(fd_read, &temp, sizeof(Report)) == sizeof(Report)) {
            if (temp.id >= 1 && temp.id <= MAX_REPORTS) {
                ids[temp.id] = 1;
            }
        }
        close(fd_read);
        while (next_id <= MAX_REPORTS && ids[next_id] == 1) next_id++;
        free(ids);
    }

    // Write report to file
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0664);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    Report r;
    memset(&r, 0, sizeof(Report));
    r.id        = next_id;
    strncpy(r.inspector,   user,        MAX_NAME     - 1);
    strncpy(r.category,    category,    MAX_CATEGORY - 1);
    strncpy(r.description, description, MAX_DESC     - 1);
    r.severity  = severity;
    r.timestamp = time(NULL);
    r.latitude  = latitude;
    r.longitude = longitude;

    if (write(fd, &r, sizeof(Report)) != (ssize_t)sizeof(Report)) {
        fprintf(stderr, "Error: failed to write report to '%s': %s\n", path, strerror(errno));
        close(fd);
        return;
    }
    close(fd);

    // Ensure permissions are correct after append
    struct stat report_st;
    if (stat(path, &report_st) == 0) {
        if ((report_st.st_mode & 0777) != 0664) {
            chmod(path, 0664);
        }
    }

    printf("Report #%d added to district '%s'\n", r.id, district);
    notify_monitor(district, role, user);
}

/* Lists all reports in a district with file metadata
   Displays: permissions, size, last modified time for reports.dat
   Sorts reports by ID before displaying */
void list_reports(const char *district, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    check_symlink(district);

    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    // Check read permission
    if (!check_permission(path, role, 1, 0)) {
        write_log(district, role, user, "list_reports_denied");
        return;
    }

    // Display file metadata
    struct stat st;
    if (stat(path, &st) == 0) {
        char perms[10];
        mode_to_string(st.st_mode, perms);

        char timebuf[64];
        struct tm *t = localtime(&st.st_mtime);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", t);

        printf("File: %s | Permissions: %s | Size: %ld bytes | Last modified: %s\n\n",
               path, perms, (long)st.st_size, timebuf);
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    // Read all reports into memory
    Report *reports = malloc(MAX_REPORTS * sizeof(Report));
    if (!reports) {
        fprintf(stderr, "Error: out of memory\n");
        close(fd);
        return;
    }

    int count = 0;
    while (read(fd, &reports[count], sizeof(Report)) == sizeof(Report)) {
        count++;
        if (count >= MAX_REPORTS) break;
    }
    close(fd);

    // Sort by ID and display
    qsort(reports, count, sizeof(Report), compare_reports_by_id);

    printf("%-5s %-20s %-12s %-10s %s\n", "ID", "Inspector", "Category", "Severity", "Timestamp");
    printf("\n");

    if (count == 0) {
        printf("No reports found in district '%s'\n", district);
        write_log(district, role, user, "list_reports");
        free(reports);
        return;
    }

    for (int i = 0; i < count; i++) {
        char timebuf[64];
        struct tm *t = localtime(&reports[i].timestamp);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", t);
        printf("%-5d %-20s %-12s %-10d %s\n",
               reports[i].id, reports[i].inspector,
               reports[i].category, reports[i].severity, timebuf);
    }

    printf("\nTotal: %d report(s)\n", count);
    free(reports);
    write_log(district, role, user, "list_reports");
}

/* Displays full details of a specific report by ID
   Both roles can view reports */
void view_report(const char *district, int report_id, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    
    if (report_id < 1) {
        fprintf(stderr, "Error: invalid report ID\n");
        return;
    }
    
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    if (!check_permission(path, role, 1, 0)) {
        write_log(district, role, user, "view_report_denied");
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    Report r;
    int found = 0;

    // Search for report by ID
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        if (r.id == report_id) {
            found = 1;

            char timebuf[64];
            struct tm *t = localtime(&r.timestamp);
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

            printf("=== Report #%d ===\n", r.id);
            printf("Inspector:   %s\n", r.inspector);
            printf("Category:    %s\n", r.category);
            printf("Severity:    %d\n", r.severity);
            printf("Timestamp:   %s\n", timebuf);
            printf("Latitude:    %.6f\n", r.latitude);
            printf("Longitude:   %.6f\n", r.longitude);
            printf("Description: %s\n", r.description);
            break;
        }
    }

    close(fd);

    if (!found) {
        printf("Report #%d not found in district '%s'\n", report_id, district);
    }
    write_log(district, role, user, "view_report");
}

/* Removes a report by ID (manager only)
   Uses lseek() to shift subsequent records and ftruncate() to resize file */
void remove_report(const char *district, int report_id, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    
    if (strcmp(role, "manager") != 0) {
        printf("Error: only managers can remove reports\n");
        write_log(district, role, user, "remove_report_denied");
        return;
    }
    
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    // Check owner write permission for manager
    if (!check_permission(path, role, 1, 1)) {
        write_log(district, role, user, "remove_report_denied");
        return;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    // Get file size and validate
    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "fstat '%s': %s\n", path, strerror(errno));
        close(fd);
        return;
    }

    if (st.st_size % sizeof(Report) != 0) {
        fprintf(stderr, "Warning: '%s' size is not a multiple of record size — possible corruption\n", path);
    }

    int total = (int)(st.st_size / sizeof(Report));
    if (total == 0) {
        printf("No reports in district '%s'\n", district);
        close(fd);
        return;
    }

    // Find the report position
    int found_pos = -1;
    Report r;
    for (int i = 0; i < total; i++) {
        lseek(fd, (off_t)(i * sizeof(Report)), SEEK_SET);
        if (read(fd, &r, sizeof(Report)) != (ssize_t)sizeof(Report)) {
            fprintf(stderr, "Error: read failed while scanning records: %s\n", strerror(errno));
            close(fd);
            return;
        }
        if (r.id == report_id) {
            found_pos = i;
            break;
        }
    }

    if (found_pos == -1) {
        printf("Report #%d not found in district '%s'\n", report_id, district);
        write_log(district, role, user, "remove_report_failed");
        close(fd);
        return;
    }

    // Shift remaining records one position earlier
    for (int i = found_pos + 1; i < total; i++) {
        lseek(fd, (off_t)(i * sizeof(Report)), SEEK_SET);
        if (read(fd, &r, sizeof(Report)) != (ssize_t)sizeof(Report)) {
            fprintf(stderr, "Error: read failed during record shift: %s\n", strerror(errno));
            close(fd);
            return;
        }
        lseek(fd, (off_t)((i - 1) * sizeof(Report)), SEEK_SET);
        if (write(fd, &r, sizeof(Report)) != (ssize_t)sizeof(Report)) {
            fprintf(stderr, "Error: write failed during record shift: %s\n", strerror(errno));
            close(fd);
            return;
        }
    }

    // Truncate file to new size
    if (ftruncate(fd, (off_t)((total - 1) * sizeof(Report))) < 0) {
        fprintf(stderr, "Error: ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return;
    }
    close(fd);

    printf("Report #%d removed from district '%s'\n", report_id, district);
    write_log(district, role, user, "remove_report");
}

/* Updates severity threshold in district.cfg (manager only)
   Verifies file permissions are exactly 640 before writing*/
void update_threshold(const char *district, int value, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    
    if (strcmp(role, "manager") != 0) {
        printf("Error: only managers can update threshold\n");
        write_log(district, role, user, "update_threshold_denied");
        return;
    }
    
    char path[256];
    snprintf(path, sizeof(path), "%s/district.cfg", district);

    // Check permissions BEFORE opening file
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "Error: cannot stat '%s': %s\n", path, strerror(errno));
        return;
    }
    
    // Verify permissions match required 640
    if ((st.st_mode & 0777) != 0640) {
        printf("Error: district.cfg permissions have been changed (current: %o), refusing to write\n", 
               st.st_mode & 0777);
        write_log(district, role, user, "update_threshold_denied");
        return;
    }
    
    // Check manager has write permission (owner)
    if (!(st.st_mode & S_IWUSR)) {
        printf("Error: manager lacks write permission on %s\n", path);
        write_log(district, role, user, "update_threshold_denied");
        return;
    }

    int fd = open(path, O_WRONLY | O_TRUNC, 0640);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "threshold=%d\n", value);
    if (write(fd, buf, strlen(buf)) < 0) {
        fprintf(stderr, "Error: failed to write threshold: %s\n", strerror(errno));
        close(fd);
        return;
    }
    close(fd);

    printf("Threshold updated to %d in district '%s'\n", value, district);
    write_log(district, role, user, "update_threshold");
}

/* AI-assisted function: splits "field:operator:value" string into three parts
   Example: "severity:>=:2" -> field="severity", op=">=", value="2"
   Returns 1 on success, 0 on malformed input */
int parse_condition(const char *input, char *field, char *op, char *value) {
    if (input == NULL || field == NULL || op == NULL || value == NULL) {
        return 0;
    }

    const char *first_colon = strchr(input, ':');
    if (first_colon == NULL) return 0;

    const char *second_colon = strchr(first_colon + 1, ':');
    if (second_colon == NULL) return 0;

    size_t field_len = first_colon - input;
    size_t op_len    = second_colon - (first_colon + 1);

    if (field_len == 0 || field_len >= 64) return 0;
    if (op_len    == 0 || op_len    >= 8)  return 0;

    memset(field, 0, 64);
    memset(op, 0, 8);
    strncpy(field, input,            field_len);  field[field_len] = '\0';
    strncpy(op,    first_colon + 1,  op_len);     op[op_len]       = '\0';

    memset(value, 0, 64);
    strncpy(value, second_colon + 1, 63);
    value[63] = '\0';

    return 1;
}

/* Compares two strings based on operator (== or !=) */
static int compare_strings(const char *actual, const char *expected, const char *op) {
    int cmp = strcmp(actual, expected);
    if (strcmp(op, "==") == 0) return cmp == 0;
    if (strcmp(op, "!=") == 0) return cmp != 0;
    return 0;
}

/* Compares two integers based on operator (==, !=, <, <=, >, >=) */
static int compare_ints(int actual, int expected, const char *op) {
    if (strcmp(op, "==") == 0) return actual == expected;
    if (strcmp(op, "!=") == 0) return actual != expected;
    if (strcmp(op, "<")  == 0) return actual <  expected;
    if (strcmp(op, "<=") == 0) return actual <= expected;
    if (strcmp(op, ">")  == 0) return actual >  expected;
    if (strcmp(op, ">=") == 0) return actual >= expected;
    return 0;
}

/* Compares two timestamps based on operator */
static int compare_time(time_t actual, time_t expected, const char *op) {
    if (strcmp(op, "==") == 0) return actual == expected;
    if (strcmp(op, "!=") == 0) return actual != expected;
    if (strcmp(op, "<")  == 0) return actual <  expected;
    if (strcmp(op, "<=") == 0) return actual <= expected;
    if (strcmp(op, ">")  == 0) return actual >  expected;
    if (strcmp(op, ">=") == 0) return actual >= expected;
    return 0;
}

/* AI-assisted function: checks if a report matches a single condition
   Converts value string to appropriate type before comparison
   Returns 1 if match, 0 if no match or error */
int match_condition(Report *r, const char *field, const char *op, const char *value) {
    if (r == NULL || field == NULL || op == NULL || value == NULL) return 0;

    if (strcmp(field, "severity") == 0) {
        char *endptr;
        int int_value = (int)strtol(value, &endptr, 10);
        if (*endptr != '\0') return 0;
        return compare_ints(r->severity, int_value, op);
    }

    if (strcmp(field, "category") == 0)
        return compare_strings(r->category, value, op);

    if (strcmp(field, "inspector") == 0)
        return compare_strings(r->inspector, value, op);

    if (strcmp(field, "timestamp") == 0) {
        char *endptr;
        time_t time_value = (time_t)strtol(value, &endptr, 10);
        if (*endptr != '\0') return 0;
        return compare_time(r->timestamp, time_value, op);
    }

    return 0;
}

/* Filters reports in a district based on one or more conditions
   Conditions are combined with AND logic (all must match) */
void filter_reports(const char *district, const char *role, const char *user,
                    int argc, char *argv[], int condition_start) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    // Check read permission
    if (!check_permission(path, role, 1, 0)) {
        write_log(district, role, user, "filter_denied");
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    // Allocate memory for matching results
    Report *results = malloc(MAX_REPORTS * sizeof(Report));
    if (!results) {
        fprintf(stderr, "Error: out of memory\n");
        close(fd);
        return;
    }

    int count = 0;
    Report r;

    // Read each report and test against all conditions
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        int match = 1;  // Assume match until proven otherwise

        // Test this report against each condition (AND logic)
        for (int i = condition_start; i < argc; i++) {
            char field[64], op[8], value[64];
            if (parse_condition(argv[i], field, op, value)) {
                if (!match_condition(&r, field, op, value)) {
                    match = 0;  // Failed one condition, skip
                    break;
                }
            }
        }

        // If all conditions matched, add to results
        if (match) {
            results[count++] = r;
            if (count >= MAX_REPORTS) break;
        }
    }

    close(fd);

    // Sort by ID and display
    qsort(results, count, sizeof(Report), compare_reports_by_id);

    for (int i = 0; i < count; i++) {
        char timebuf[64];
        struct tm *t = localtime(&results[i].timestamp);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", t);
        printf("%-5d %-20s %-12s %-10d %s\n",
               results[i].id, results[i].inspector,
               results[i].category, results[i].severity, timebuf);
    }

    printf("\nTotal matching: %d report(s)\n", count);
    free(results);
    write_log(district, role, user, "filter");
}

/* Removes entire district directory and symlink (manager only)
   Uses fork() + execlp("rm") to delete the directory
   Safety checks prevent deleting root or current directory */
void remove_district(const char *district, const char *role, const char *user) {
    // Manager only
    if (strcmp(role, "manager") != 0) {
        printf("Error: only managers can remove districts\n");
        return;
    }

    // Safety check - prevent dangerous deletions
    if (strlen(district) == 0 || strcmp(district, "/") == 0 || strcmp(district, ".") == 0) {
        printf("Error: invalid district name\n");
        return;
    }

    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }

    // Remove symlink first
    char link_name[256];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);
    unlink(link_name);
    printf("Symlink '%s' removed\n", link_name);

    // Fork child process to execute rm -rf
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return;
    } else if (pid == 0) {
        // Child process - replace with rm -rf
        execlp("rm", "rm", "-rf", district, NULL);
        perror("execlp");
        exit(1);
    } else {
        // Parent process - wait for child to finish
        int status;
        wait(&status);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("District '%s' removed successfully\n", district);
        } else {
            printf("Error: failed to remove district '%s'\n", district);
        }
    }
}

/* Required arguments: --role <role> --user <user> --<command> <district> [args]
   Roles: inspector, manager
   Commands: add, list, view, remove_report, update_threshold, filter, remove_district */
int main(int argc, char *argv[]) {
    char *role            = NULL;
    char *user            = NULL;
    char *command         = NULL;
    char *district        = NULL;
    int   report_id       = -1;
    int   threshold_value = -1;

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {

        if (strcmp(argv[i], "--role") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --role requires a value\n"); return 1; }
            role = argv[i];

        } else if (strcmp(argv[i], "--user") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --user requires a value\n"); return 1; }
            user = argv[i];

        } else if (strcmp(argv[i], "--add") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --add requires a district\n"); return 1; }
            command  = "add";
            district = argv[i];

        } else if (strcmp(argv[i], "--list") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --list requires a district\n"); return 1; }
            command  = "list";
            district = argv[i];

        } else if (strcmp(argv[i], "--view") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --view requires a district\n"); return 1; }
            command  = "view";
            district = argv[i];
            if (++i >= argc) { fprintf(stderr, "Error: --view requires a report ID\n"); return 1; }
            if (!parse_int(argv[i], &report_id, "report ID")) return 1;

        } else if (strcmp(argv[i], "--remove_report") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --remove_report requires a district\n"); return 1; }
            command  = "remove_report";
            district = argv[i];
            if (++i >= argc) { fprintf(stderr, "Error: --remove_report requires a report ID\n"); return 1; }
            if (!parse_int(argv[i], &report_id, "report ID")) return 1;

        } else if (strcmp(argv[i], "--update_threshold") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --update_threshold requires a district\n"); return 1; }
            command  = "update_threshold";
            district = argv[i];
            if (++i >= argc) { fprintf(stderr, "Error: --update_threshold requires a value\n"); return 1; }
            if (!parse_int(argv[i], &threshold_value, "threshold value")) return 1;

        } else if (strcmp(argv[i], "--filter") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --filter requires a district\n"); return 1; }
            command  = "filter";
            district = argv[i];
        } else if (strcmp(argv[i], "--remove_district") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --remove_district requires a district\n"); return 1; }
            command  = "remove_district";
            district = argv[i];
        }
    }

    // Validate required arguments
    if (!role || !user || !command || !district) {
        printf("Usage: city_manager --role <role> --user <user> --<command> <district> [args]\n");
        return 1;
    }

    // Dispatch to appropriate function
    if (strcmp(command, "add") == 0) {
        add_report(district, user, role);

    } else if (strcmp(command, "list") == 0) {
        list_reports(district, role, user);

    } else if (strcmp(command, "view") == 0) {
        view_report(district, report_id, role, user);

    } else if (strcmp(command, "remove_report") == 0) {
        remove_report(district, report_id, role, user);

    } else if (strcmp(command, "update_threshold") == 0) {
        update_threshold(district, threshold_value, role, user);

    } else if (strcmp(command, "filter") == 0) {
        // Find where conditions start in argv
        int condition_start = 0;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--filter") == 0) {
                condition_start = i + 2;  // Skip --filter and district name
                break;
            }
        }
        filter_reports(district, role, user, argc, argv, condition_start);
    } else if (strcmp(command, "remove_district") == 0) {
        remove_district(district, role, user);
    }

    return 0;
}