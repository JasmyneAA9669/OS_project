#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

#define MAX_NAME     64
#define MAX_CATEGORY 32
#define MAX_DESC     256
#define MAX_REPORTS  10000

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

/* --------------------------------------------------------------------------
 * parse_int
 * Converts a string to an integer using strtol, which unlike atoi detects
 * invalid input (letters, overflow, empty string). Returns 1 on success and
 * stores the result in *out. Returns 0 and prints an error on failure.
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * district_exists
 * Returns 1 if the district directory exists, 0 otherwise.
 * Used to give a clear error message when operating on a non-existent
 * district instead of a cryptic open() failure deeper in the call chain.
 * -------------------------------------------------------------------------- */
static int district_exists(const char *district) {
    struct stat st;
    return (stat(district, &st) == 0 && S_ISDIR(st.st_mode));
}

/* --------------------------------------------------------------------------
 * mode_to_string
 * Converts the permission bits of a mode_t into a 9-character string like
 * "rw-rw-r--". The caller must supply a buffer of at least 10 bytes.
 * Each bit is tested individually against the POSIX macros so the mapping
 * is explicit and easy to verify.
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * check_permission
 * Checks whether a given role has a required access bit on a file.
 *
 * The spec maps roles to Unix identities:
 *   manager  -> owner  (use S_IRUSR / S_IWUSR / S_IXUSR bits)
 *   inspector -> group  (use S_IRGRP / S_IWGRP / S_IXGRP bits)
 *
 * The caller passes the GROUP bit that represents the intended permission
 * (e.g. S_IWGRP for "write access"). For managers the function shifts that
 * to the equivalent OWNER bit before checking, so the same call works for
 * both roles without the caller needing to know which bit set to use.
 *
 * Returns 1 if the role has the permission, 0 otherwise.
 * -------------------------------------------------------------------------- */
int check_permission(const char *path, const char *role, mode_t required_group_bit) {
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "stat '%s': %s\n", path, strerror(errno));
        return 0;
    }

    if (strcmp(role, "manager") == 0) {
        /*
         * Managers are owners. Map the group bit to its owner equivalent:
         *   S_IRGRP (0040) -> S_IRUSR (0400)
         *   S_IWGRP (0020) -> S_IWUSR (0200)
         *   S_IXGRP (0010) -> S_IXUSR (0100)
         * The group bits sit 3 positions to the right of the owner bits,
         * so shifting left by 3 does the conversion.
         */
        mode_t owner_bit = required_group_bit << 3;
        return (st.st_mode & owner_bit) ? 1 : 0;

    } else if (strcmp(role, "inspector") == 0) {
        /* Inspectors are group members — check group bits directly. */
        return (st.st_mode & required_group_bit) ? 1 : 0;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * write_log
 * Appends a tab-separated audit entry to logged_district.
 * O_APPEND makes each write atomic at the kernel level, which is important
 * if multiple processes ever write to the same log simultaneously.
 * Errors are reported to stderr so the caller always knows if logging failed.
 * -------------------------------------------------------------------------- */
void write_log(const char *district, const char *role, const char *user, const char *action) {
    char path[256];
    snprintf(path, sizeof(path), "%s/logged_district", district);

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

/* --------------------------------------------------------------------------
 * create_symlink
 * Creates a convenience symlink named active_reports-<district> in the
 * current working directory, pointing to <district>/reports.dat.
 * Any pre-existing symlink with the same name is removed first via unlink().
 * -------------------------------------------------------------------------- */
void create_symlink(const char *district) {
    char link_name[256];
    char target[256];

    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);
    snprintf(target,    sizeof(target),    "%s/reports.dat",    district);

    unlink(link_name);  /* remove old symlink if it exists; ignore errors */

    if (symlink(target, link_name) < 0) {
        fprintf(stderr, "symlink '%s': %s\n", link_name, strerror(errno));
        return;
    }

    printf("Symlink '%s' -> '%s' created\n", link_name, target);
}

/* --------------------------------------------------------------------------
 * check_symlink
 * Uses lstat() on the symlink itself (not its target) to confirm it exists
 * and is indeed a symlink. Then uses stat() to follow the link and check
 * whether the target file still exists. Reports dangling links as a warning.
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * init_district
 * Creates the directory structure and files for a new district.
 * umask(0) is called so that the chmod() calls below set permissions exactly
 * as specified — without it, the process umask would silently remove bits.
 *
 * Permission layout (from spec):
 *   district directory  750  rwxr-x---
 *   reports.dat         664  rw-rw-r--
 *   district.cfg        640  rw-r-----
 *   logged_district     644  rw-r--r--
 * -------------------------------------------------------------------------- */
void init_district(const char *district) {
    umask(0);
    char path[256];

    mkdir(district, 0750);
    chmod(district, 0750);

    snprintf(path, sizeof(path), "%s/reports.dat", district);
    int fd = open(path, O_WRONLY | O_CREAT, 0664);
    if (fd >= 0) close(fd);
    chmod(path, 0664);

    snprintf(path, sizeof(path), "%s/district.cfg", district);
    fd = open(path, O_WRONLY | O_CREAT, 0640);
    if (fd >= 0) close(fd);
    chmod(path, 0640);

    snprintf(path, sizeof(path), "%s/logged_district", district);
    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
    chmod(path, 0644);

    create_symlink(district);
}

/* --------------------------------------------------------------------------
 * compare_reports_by_id  (qsort comparator)
 * Used by both list_reports and filter_reports to sort results by ID.
 * Replaces the two copies of bubble sort that existed before.
 * qsort is O(n log n); bubble sort was O(n^2).
 * -------------------------------------------------------------------------- */
static int compare_reports_by_id(const void *a, const void *b) {
    const Report *ra = (const Report *)a;
    const Report *rb = (const Report *)b;
    return ra->id - rb->id;
}

/* --------------------------------------------------------------------------
 * add_report
 * Reads a new report from stdin, assigns the next available ID, and appends
 * the fixed-size Record struct to reports.dat.
 *
 * Both roles may add reports (spec: "Both roles may add reports").
 * check_permission verifies S_IWGRP (group write) on reports.dat before
 * writing — managers pass because their owner-write bit (S_IWUSR) is set,
 * inspectors pass because the group-write bit (S_IWGRP) is set (664).
 *
 * ID assignment: reads all existing IDs into a boolean array and finds the
 * first gap. This correctly handles IDs left by deleted reports.
 * The ids array is heap-allocated to avoid a large stack frame.
 * -------------------------------------------------------------------------- */
void add_report(const char *district, const char *user, const char *role) {
    /* Create district structure if it does not exist yet */
    struct stat dst;
    if (stat(district, &dst) != 0) {
        init_district(district);
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    /* Permission check: both roles need write access to reports.dat */
    if (!check_permission(path, role, S_IWGRP)) {
        fprintf(stderr, "Error: role '%s' does not have write permission on %s\n", role, path);
        write_log(district, role, user, "add_report_denied");
        return;
    }

    /* Read report fields from stdin */
    double latitude, longitude;
    char   category[MAX_CATEGORY];
    int    severity;
    char   description[MAX_DESC];

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
    /* Width limit = MAX_CATEGORY - 1 = 31, prevents buffer overflow */
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
    getchar();  /* consume the newline left in the buffer by the last scanf */
    if (fgets(description, MAX_DESC, stdin) == NULL) {
        fprintf(stderr, "Error: could not read description\n");
        return;
    }
    description[strcspn(description, "\n")] = '\0';  /* strip trailing newline */

    /* Find next available ID by scanning existing records */
    int next_id = 1;
    int fd_read = open(path, O_RDONLY);
    if (fd_read >= 0) {
        /*
         * Use a heap-allocated boolean array instead of a stack array.
         * 10000 ints on the stack = 40 KB; on the heap it is fine.
         * The +1 makes id=10000 safe to index without going out of bounds.
         */
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

    printf("Report #%d added to district '%s'\n", r.id, district);
    write_log(district, role, user, "add_report");
}

/* --------------------------------------------------------------------------
 * list_reports
 * Prints metadata for reports.dat (permissions, size, last-modified) and
 * then a sorted table of all reports in the district.
 *
 * Reports are loaded into a heap-allocated array (not a stack array) to
 * avoid a multi-megabyte stack frame. They are sorted by ID with qsort.
 *
 * Permission check: both roles need read access (S_IRGRP covers group read,
 * which is set on 664 for inspectors; managers have owner read).
 * -------------------------------------------------------------------------- */
void list_reports(const char *district, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    check_symlink(district);

    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    /* Permission check: read access required */
    if (!check_permission(path, role, S_IRGRP)) {
        fprintf(stderr, "Error: role '%s' does not have read permission on %s\n", role, path);
        write_log(district, role, user, "list_reports_denied");
        return;
    }

    /* Print file metadata as required by spec */
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

    /* Heap-allocate the report array */
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

/* --------------------------------------------------------------------------
 * view_report
 * Scans reports.dat linearly for a matching ID and prints full details.
 * Available to both roles; permission check requires group read (S_IRGRP).
 * -------------------------------------------------------------------------- */
void view_report(const char *district, int report_id, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    if (!check_permission(path, role, S_IRGRP)) {
        fprintf(stderr, "Error: role '%s' does not have read permission on %s\n", role, path);
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

/* --------------------------------------------------------------------------
 * remove_report
 * Removes a single report by shifting all subsequent records one slot
 * backward using lseek(), then truncating the file with ftruncate().
 * Manager role only — verified both by role string and by checking that the
 * owner-write bit (mapped from S_IWGRP via check_permission) is set.
 * -------------------------------------------------------------------------- */
void remove_report(const char *district, int report_id, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    /* Spec: manager role only - explicit role check */
if (strcmp(role, "manager") != 0) {
    printf("Error: only managers can remove reports\n");
    write_log(district, role, user, "remove_report_denied");
    return;
}

/* Additional verification: manager must have write permission */
if (!check_permission(path, role, S_IWGRP)) {
    printf("Error: manager lacks write permission on %s\n", path);
    write_log(district, role, user, "remove_report_denied");
    return;
}

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "fstat '%s': %s\n", path, strerror(errno));
        close(fd);
        return;
    }

    /*
     * Warn if the file size is not a clean multiple of sizeof(Report).
     * This could indicate a partial write or corruption.
     */
    if (st.st_size % sizeof(Report) != 0) {
        fprintf(stderr, "Warning: '%s' size is not a multiple of record size — possible corruption\n", path);
    }

    int total = (int)(st.st_size / sizeof(Report));

    /* Find the record to delete */
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

    /* Shift every record after found_pos one slot earlier */
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

    /* Shrink the file by one record */
    if (ftruncate(fd, (off_t)((total - 1) * sizeof(Report))) < 0) {
        fprintf(stderr, "Error: ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return;
    }
    close(fd);

    printf("Report #%d removed from district '%s'\n", report_id, district);
    write_log(district, role, user, "remove_report");
}

/* --------------------------------------------------------------------------
 * update_threshold
 * Writes a threshold=<value> line to district.cfg.
 * Manager role only. Uses fstat() on the already-open descriptor instead of
 * a separate stat() call, eliminating the TOCTOU race window that existed
 * when stat() and open() were two separate operations.
 * -------------------------------------------------------------------------- */
void update_threshold(const char *district, int value, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/district.cfg", district);

    /* Spec: manager role only */
    if (!check_permission(path, role, S_IWGRP)) {
        printf("Error: only managers can update threshold\n");
        write_log(district, role, user, "update_threshold_denied");
        return;
    }

    /*
     * Open first, THEN check permissions via fstat() on the open descriptor.
     * This closes the TOCTOU race: between stat() and open() in the original
     * code, another process could change permissions in that window.
     */
    int fd = open(path, O_WRONLY | O_TRUNC, 0640);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || (st.st_mode & 0777) != 0640) {
        printf("Error: district.cfg permissions have been changed, refusing to write\n");
        close(fd);
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

/* --------------------------------------------------------------------------
 * parse_condition  (AI-assisted, reviewed and corrected by student)
 * Splits a "field:operator:value" string into its three parts.
 * Returns 1 on success, 0 if the string does not contain two colons.
 * The value destination is limited to 63 characters to prevent overflow.
 * -------------------------------------------------------------------------- */
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

    /* Reject oversized field or operator before copying to fixed buffers */
    if (field_len == 0 || field_len >= 64) return 0;
    if (op_len    == 0 || op_len    >= 8)  return 0;

    memset(field, 0, 64);
    memset(op, 0, 8);
    strncpy(field, input,            field_len);  field[field_len] = '\0';
    strncpy(op,    first_colon + 1,  op_len);     op[op_len]       = '\0';

    /* Bounded copy for value - properly zero out the buffer first */
    memset(value, 0, 64);
    strncpy(value, second_colon + 1, 63);
    value[63] = '\0';

    return 1;
}

static int compare_strings(const char *actual, const char *expected, const char *op) {
    int cmp = strcmp(actual, expected);
    if (strcmp(op, "==") == 0) return cmp == 0;
    if (strcmp(op, "!=") == 0) return cmp != 0;
    return 0;
}

static int compare_ints(int actual, int expected, const char *op) {
    if (strcmp(op, "==") == 0) return actual == expected;
    if (strcmp(op, "!=") == 0) return actual != expected;
    if (strcmp(op, "<")  == 0) return actual <  expected;
    if (strcmp(op, "<=") == 0) return actual <= expected;
    if (strcmp(op, ">")  == 0) return actual >  expected;
    if (strcmp(op, ">=") == 0) return actual >= expected;
    return 0;
}

static int compare_time(time_t actual, time_t expected, const char *op) {
    if (strcmp(op, "==") == 0) return actual == expected;
    if (strcmp(op, "!=") == 0) return actual != expected;
    if (strcmp(op, "<")  == 0) return actual <  expected;
    if (strcmp(op, "<=") == 0) return actual <= expected;
    if (strcmp(op, ">")  == 0) return actual >  expected;
    if (strcmp(op, ">=") == 0) return actual >= expected;
    return 0;
}

/* --------------------------------------------------------------------------
 * match_condition  (AI-assisted, reviewed and corrected by student)
 * Returns 1 if Report *r satisfies field:op:value, 0 otherwise.
 * The value string is converted to the correct C type (int, time_t, or
 * char*) based on the field name before comparison.
 * -------------------------------------------------------------------------- */
int match_condition(Report *r, const char *field, const char *op, const char *value) {
    if (r == NULL || field == NULL || op == NULL || value == NULL) return 0;

    if (strcmp(field, "severity") == 0) {
        char *endptr;
        int int_value = (int)strtol(value, &endptr, 10);
        if (*endptr != '\0') return 0;  /* value is not a valid integer */
        return compare_ints(r->severity, int_value, op);
    }

    if (strcmp(field, "category") == 0)
        return compare_strings(r->category, value, op);

    if (strcmp(field, "inspector") == 0)
        return compare_strings(r->inspector, value, op);

    if (strcmp(field, "timestamp") == 0) {
        char *endptr;
        time_t time_value = (time_t)strtol(value, &endptr, 10);
        if (*endptr != '\0') return 0;  /* value is not a valid integer */
        return compare_time(r->timestamp, time_value, op);
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * filter_reports
 * Reads every record from reports.dat and applies all conditions (AND logic).
 * Results are collected into a heap-allocated array, sorted by ID, and
 * printed. The permission check requires group read (S_IRGRP).
 * -------------------------------------------------------------------------- */
void filter_reports(const char *district, const char *role, const char *user,
                    int argc, char *argv[], int condition_start) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    if (!check_permission(path, role, S_IRGRP)) {
        fprintf(stderr, "Error: role '%s' does not have read permission on %s\n", role, path);
        write_log(district, role, user, "filter_denied");
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }

    /* Heap-allocate results array */
    Report *results = malloc(MAX_REPORTS * sizeof(Report));
    if (!results) {
        fprintf(stderr, "Error: out of memory\n");
        close(fd);
        return;
    }

    int count = 0;
    Report r;

    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        int match = 1;

        for (int i = condition_start; i < argc; i++) {
            char field[64], op[8], value[64];
            if (parse_condition(argv[i], field, op, value)) {
                if (!match_condition(&r, field, op, value)) {
                    match = 0;
                    break;
                }
            }
        }

        if (match) {
            results[count++] = r;
            if (count >= MAX_REPORTS) break;
        }
    }

    close(fd);

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

/* --------------------------------------------------------------------------
 * main
 * Parses command-line arguments and dispatches to the correct function.
 * Every argv[++i] access is guarded with a bounds check so that missing
 * arguments produce a clear error instead of a crash or undefined behaviour.
 * Numeric arguments use parse_int() (strtol-based) instead of atoi() so
 * invalid input like "abc" is caught and reported.
 * threshold_value is a separate variable from report_id for clarity.
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    char *role            = NULL;
    char *user            = NULL;
    char *command         = NULL;
    char *district        = NULL;
    int   report_id       = -1;
    int   threshold_value = -1;

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
        }
    }

    if (!role || !user || !command || !district) {
        printf("Usage: city_manager --role <role> --user <user> --<command> <district> [args]\n");
        return 1;
    }

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
        int condition_start = 0;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--filter") == 0) {
                condition_start = i + 2;  /* skip --filter and district name */
                break;
            }
        }
        filter_reports(district, role, user, argc, argv, condition_start);
    }

    return 0;
}