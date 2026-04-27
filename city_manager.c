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

static int district_exists(const char *district) {
    struct stat st;
    return (stat(district, &st) == 0 && S_ISDIR(st.st_mode));
}

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

int check_permission(const char *path, const char *role, mode_t required_group_bit) {
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "stat '%s': %s\n", path, strerror(errno));
        return 0;
    }

    if (strcmp(role, "manager") == 0) {
        mode_t owner_bit = required_group_bit << 3;
        return (st.st_mode & owner_bit) ? 1 : 0;

    } else if (strcmp(role, "inspector") == 0) {
        return (st.st_mode & required_group_bit) ? 1 : 0;
    }

    return 0;
}

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

void create_symlink(const char *district) {
    char link_name[256];
    char target[256];

    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);
    snprintf(target,    sizeof(target),    "%s/reports.dat",    district);

    unlink(link_name);

    if (symlink(target, link_name) < 0) {
        fprintf(stderr, "symlink '%s': %s\n", link_name, strerror(errno));
        return;
    }

    printf("Symlink '%s' -> '%s' created\n", link_name, target);
}

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

static int compare_reports_by_id(const void *a, const void *b) {
    const Report *ra = (const Report *)a;
    const Report *rb = (const Report *)b;
    return ra->id - rb->id;
}

void add_report(const char *district, const char *user, const char *role) {
    struct stat dst;
    if (stat(district, &dst) != 0) {
        init_district(district);
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    if (!check_permission(path, role, S_IWGRP)) {
        fprintf(stderr, "Error: role '%s' does not have write permission on %s\n", role, path);
        write_log(district, role, user, "add_report_denied");
        return;
    }

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
    getchar();
    if (fgets(description, MAX_DESC, stdin) == NULL) {
        fprintf(stderr, "Error: could not read description\n");
        return;
    }
    description[strcspn(description, "\n")] = '\0';

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

void list_reports(const char *district, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    check_symlink(district);

    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    if (!check_permission(path, role, S_IRGRP)) {
        fprintf(stderr, "Error: role '%s' does not have read permission on %s\n", role, path);
        write_log(district, role, user, "list_reports_denied");
        return;
    }

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

void remove_report(const char *district, int report_id, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

if (strcmp(role, "manager") != 0) {
    printf("Error: only managers can remove reports\n");
    write_log(district, role, user, "remove_report_denied");
    return;
}

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

    if (st.st_size % sizeof(Report) != 0) {
        fprintf(stderr, "Warning: '%s' size is not a multiple of record size — possible corruption\n", path);
    }

    int total = (int)(st.st_size / sizeof(Report));

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

    if (ftruncate(fd, (off_t)((total - 1) * sizeof(Report))) < 0) {
        fprintf(stderr, "Error: ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return;
    }
    close(fd);

    printf("Report #%d removed from district '%s'\n", report_id, district);
    write_log(district, role, user, "remove_report");
}

void update_threshold(const char *district, int value, const char *role, const char *user) {
    if (!district_exists(district)) {
        fprintf(stderr, "Error: district '%s' does not exist\n", district);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/district.cfg", district);

    if (!check_permission(path, role, S_IWGRP)) {
        printf("Error: only managers can update threshold\n");
        write_log(district, role, user, "update_threshold_denied");
        return;
    }

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
                condition_start = i + 2;
                break;
            }
        }
        filter_reports(district, role, user, argc, argv, condition_start);
    }

    return 0;
}