#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>      
#include <unistd.h>     
#include <sys/stat.h>   
#include <sys/types.h>
#include <time.h>       

#define MAX_NAME     64
#define MAX_CATEGORY 32
#define MAX_DESC     256

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

int check_permission(const char *path, const char *role, mode_t required_bit) {
    struct stat st;
    if (stat(path, &st) < 0) {
        perror("stat");
        return 0;
    }

    // Managers are owners, inspectors are group
    if (strcmp(role, "manager") == 0) {
        // shift required_bit to owner position if needed
        return (st.st_mode & required_bit) ? 1 : 0;
    } else if (strcmp(role, "inspector") == 0) {
        return (st.st_mode & required_bit) ? 1 : 0;
    }

    return 0;
}

void write_log(const char *district, const char *role, const char *user, const char *action) {
    char path[256];
    snprintf(path, sizeof(path), "%s/logged_district", district);

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        perror("open logged_district");
        return;
    }

    time_t now = time(NULL);
    char entry[512];
    snprintf(entry, sizeof(entry), "%ld\t%s\t%s\t%s\n",
             (long)now, user, role, action);

    write(fd, entry, strlen(entry));
    close(fd);
}

void create_symlink(const char *district) {
    char link_name[256];
    char target[256];

    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);
    snprintf(target, sizeof(target), "%s/reports.dat", district);

    // Remove old symlink if it exists
    unlink(link_name);

    if (symlink(target, link_name) < 0) {
        perror("symlink");
        return;
    }

    printf("Symlink '%s' -> '%s' created\n", link_name, target);
}

void check_symlink(const char *district) {
    char link_name[256];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);

    struct stat lst;
    // lstat checks the link itself, not what it points to
    if (lstat(link_name, &lst) < 0) {
        printf("No symlink found for district '%s'\n", district);
        return;
    }

    if (S_ISLNK(lst.st_mode)) {
        // Check if the target actually exists
        struct stat st;
        if (stat(link_name, &st) < 0) {
            printf("Warning: symlink '%s' is dangling (target does not exist)\n", link_name);
        } else {
            printf("Symlink '%s' is valid\n", link_name);
        }
    }
}

void init_district(const char *district) {
    umask(0);  // Ensure we can set permissions exactly
    char path[256];

    //district directory with 750
    mkdir(district, 0750);
    chmod(district, 0750);

    //reports.dat with 664
    snprintf(path, sizeof(path), "%s/reports.dat", district);
    int fd = open(path, O_WRONLY | O_CREAT, 0664);
    if (fd >= 0) close(fd);
    chmod(path, 0664);

    //district.cfg with 640
    snprintf(path, sizeof(path), "%s/district.cfg", district);
    fd = open(path, O_WRONLY | O_CREAT, 0640);
    if (fd >= 0) close(fd);
    chmod(path, 0640);

    //logged_district with 644
    snprintf(path, sizeof(path), "%s/logged_district", district);
    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
    chmod(path, 0644);

    create_symlink(district);
}

void add_report(const char *district, const char *user, const char *role) {
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    struct stat st;
    if (stat(district, &st) != 0) {
        init_district(district);
    }

    // For the input
    double latitude, longitude;
    char category[MAX_CATEGORY];
    int severity;
    char description[MAX_DESC];

    printf("X: ");
    scanf("%lf", &latitude);
    printf("Y: ");
    scanf("%lf", &longitude);
    printf("Category (road/lighting/flooding/other): ");
    scanf("%s", category);
    printf("Severity level (1/2/3): ");
    scanf("%d", &severity);
    printf("Description: ");
    getchar();  // consume newline left by scanf
    fgets(description, MAX_DESC, stdin);
    // Remove trailing newline from fgets
    description[strcspn(description, "\n")] = '\0';

    // Find next available ID
    int next_id = 1;
    int fd_read = open(path, O_RDONLY);
    if (fd_read >= 0) {
        Report temp;
        int ids[10000] = {0};
        while (read(fd_read, &temp, sizeof(Report)) == sizeof(Report)) {
            ids[temp.id] = 1;
        }
        close(fd_read);
        while (ids[next_id] == 1) next_id++;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0664);
    if (fd < 0) {
        perror("open reports.dat");
        return;
    }

    Report r;
    memset(&r, 0, sizeof(Report));
    r.id        = next_id;
    strncpy(r.inspector, user, MAX_NAME - 1);
    r.severity  = severity;
    r.timestamp = time(NULL);
    strncpy(r.category, category, MAX_CATEGORY - 1);
    strncpy(r.description, description, MAX_DESC - 1);
    r.latitude  = latitude;
    r.longitude = longitude;

    write(fd, &r, sizeof(Report));
    close(fd);

    printf("Report #%d added to district '%s'\n", r.id, district);
    write_log(district, role, user, "add_report");
}

void list_reports(const char *district, const char *role, const char *user) {
    check_symlink(district);  // Check symlink before listing reports
    
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    // Print file metadata
    struct stat st;
    if (stat(path, &st) == 0) {
        char perms[10];
        mode_to_string(st.st_mode, perms);

        char timebuf[64];
        struct tm *t = localtime(&st.st_mtime);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", t);

        printf("File: %s | Permissions: %s | Size: %ld bytes | Last modified: %s\n\n",
            path, perms, st.st_size, timebuf);
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open reports.dat");
        return;
    }

    Report reports[10000];
    int count = 0;
    while (read(fd, &reports[count], sizeof(Report)) == sizeof(Report)) {
        count++;
    }
    close(fd);

    // Bubble sort to sort the IDS :)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (reports[j].id > reports[j+1].id) {
                Report tmp = reports[j];
                reports[j] = reports[j+1];
                reports[j+1] = tmp;
            }
        }
    }

    printf("%-5s %-20s %-12s %-10s %s\n", "ID", "Inspector", "Category", "Severity", "Timestamp");
    printf("\n");

    if (count == 0) {
        printf("No reports found in district '%s'\n", district);
        write_log(district, role, user, "list_reports");  // log before returning!
        return;
    }

    for (int i = 0; i < count; i++) {
        char timebuf[64];
        struct tm *t = localtime(&reports[i].timestamp);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", t);
        printf("%-5d %-20s %-12s %-10d %s\n",
            reports[i].id, reports[i].inspector, reports[i].category, reports[i].severity, timebuf);
    }

    printf("\nTotal: %d report(s)\n", count);
    write_log(district, role, user, "list_reports");
}

void view_report(const char *district, int report_id, const char *role, const char *user) {
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open reports.dat");
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
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    // Only managers can remove reports
    if (strcmp(role, "manager") != 0) {
        printf("Error: only managers can remove reports\n");
        write_log(district, role, user, "remove_report_denied");
        return;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("open reports.dat");
        return;
    }

    // Count total records
    struct stat st;
    stat(path, &st);
    int total = st.st_size / sizeof(Report);

    // Find the record to remove
    int found_pos = -1;
    Report r;
    for (int i = 0; i < total; i++) {
        lseek(fd, i * sizeof(Report), SEEK_SET);
        read(fd, &r, sizeof(Report));
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

    // Shift every record after found_pos one slot backwards
    for (int i = found_pos + 1; i < total; i++) {
        lseek(fd, i * sizeof(Report), SEEK_SET);
        read(fd, &r, sizeof(Report));

        lseek(fd, (i - 1) * sizeof(Report), SEEK_SET);
        write(fd, &r, sizeof(Report));
    }

    // Shrink the file by one record
    ftruncate(fd, (total - 1) * sizeof(Report));
    close(fd);

    printf("Report #%d removed from district '%s'\n", report_id, district);
    write_log(district, role, user, "remove_report");
}

void update_threshold(const char *district, int value, const char *role, const char *user) {
    // Only managers can update threshold
    if (strcmp(role, "manager") != 0) {
        printf("Error: only managers can update threshold\n");
        write_log(district, role, user, "update_threshold_denied");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/district.cfg", district);

    // Check permission bits are still 640
    struct stat st;
    if (stat(path, &st) < 0) {
        perror("stat district.cfg");
        return;
    }
    if ((st.st_mode & 0777) != 0640) {
        printf("Error: district.cfg permissions have been changed, refusing to write\n");
        return;
    }

    int fd = open(path, O_WRONLY | O_TRUNC, 0640);
    if (fd < 0) {
        perror("open district.cfg");
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "threshold=%d\n", value);
    write(fd, buf, strlen(buf));
    close(fd);

    printf("Threshold updated to %d in district '%s'\n", value, district);
    write_log(district, role, user, "update_threshold");
}

int parse_condition(const char *input, char *field, char *op, char *value) {
    if (input == NULL || field == NULL || op == NULL || value == NULL) {
        return 0;
    }
    
    const char *first_colon = strchr(input, ':');
    if (first_colon == NULL) {
        return 0;
    }
    
    const char *second_colon = strchr(first_colon + 1, ':');
    if (second_colon == NULL) {
        return 0;
    }
    
    // Calculate lengths
    size_t field_len = first_colon - input;
    size_t op_len = second_colon - (first_colon + 1);
    
    strncpy(field, input, field_len);
    field[field_len] = '\0';
    
    strncpy(op, first_colon + 1, op_len);
    op[op_len] = '\0';
    
    // Copy value
    strcpy(value, second_colon + 1);
    
    return 1;
}

static int compare_strings(const char *actual, const char *expected, const char *op) {
    int cmp = strcmp(actual, expected);
    
    if (strcmp(op, "==") == 0) {
        return cmp == 0;
    } else if (strcmp(op, "!=") == 0) {
        return cmp != 0;
    }
    
    return 0;
}

static int compare_ints(int actual, int expected, const char *op) {
    if (strcmp(op, "==") == 0) {
        return actual == expected;
    } else if (strcmp(op, "!=") == 0) {
        return actual != expected;
    } else if (strcmp(op, "<") == 0) {
        return actual < expected;
    } else if (strcmp(op, "<=") == 0) {
        return actual <= expected;
    } else if (strcmp(op, ">") == 0) {
        return actual > expected;
    } else if (strcmp(op, ">=") == 0) {
        return actual >= expected;
    }
    return 0;
}

static int compare_time(time_t actual, time_t expected, const char *op) {
    if (strcmp(op, "==") == 0) {
        return actual == expected;
    } else if (strcmp(op, "!=") == 0) {
        return actual != expected;
    } else if (strcmp(op, "<") == 0) {
        return actual < expected;
    } else if (strcmp(op, "<=") == 0) {
        return actual <= expected;
    } else if (strcmp(op, ">") == 0) {
        return actual > expected;
    } else if (strcmp(op, ">=") == 0) {
        return actual >= expected;
    }
    return 0;
}

int match_condition(Report *r, const char *field, const char *op, const char *value) {
    if (r == NULL || field == NULL || op == NULL || value == NULL) {
        return 0;
    }
    
    if (strcmp(field, "severity") == 0) {
        int int_value = atoi(value);
        return compare_ints(r->severity, int_value, op);
    }
    
    else if (strcmp(field, "category") == 0) {
        return compare_strings(r->category, value, op);
    }
    
    else if (strcmp(field, "inspector") == 0) {
        return compare_strings(r->inspector, value, op);
    }
    
    else if (strcmp(field, "timestamp") == 0) {
        time_t time_value = (time_t)atol(value);
        return compare_time(r->timestamp, time_value, op);
    }
    
    return 0;
}

void filter_reports(const char *district, const char *role, const char *user, int argc, char *argv[], int condition_start) {
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open reports.dat");
        return;
    }

    Report results[10000];
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
        }
    }

    close(fd);

    // Bubble sort for IDS :)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (results[j].id > results[j+1].id) {
                Report tmp = results[j];
                results[j] = results[j+1];
                results[j+1] = tmp;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        char timebuf[64];
        struct tm *t = localtime(&results[i].timestamp);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", t);
        printf("%-5d %-20s %-12s %-10d %s\n",
            results[i].id, results[i].inspector, results[i].category, results[i].severity, timebuf);
    }

    printf("\nTotal matching: %d report(s)\n", count);
    write_log(district, role, user, "filter");
}

int main(int argc, char *argv[]) {
    char *role     = NULL;
    char *user     = NULL;
    char *command  = NULL;
    char *district = NULL;
    int  report_id = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--role") == 0) {
            role = argv[++i];
        } else if (strcmp(argv[i], "--user") == 0) {
            user = argv[++i];
        } else if (strcmp(argv[i], "--add") == 0) {
            command  = "add";
            district = argv[++i];
        } else if (strcmp(argv[i], "--list") == 0) {
            command  = "list";
            district = argv[++i];
        } else if (strcmp(argv[i], "--view") == 0) {
            command   = "view";
            district  = argv[++i];
            report_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--remove_report") == 0) {
            command   = "remove_report";
            district  = argv[++i];
            report_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--update_threshold") == 0) {
            command  = "update_threshold";
            district = argv[++i];
            report_id = atoi(argv[++i]);  // reusing report_id for threshold value
        } else if (strcmp(argv[i], "--filter") == 0) {
            command  = "filter";
            district = argv[++i];
            // conditions start at the next argument
        }
    }

    // Validate required arguments
    if (!role || !user || !command || !district) {
        printf("Usage: city_manager --role <role> --user <user> --<command> <district>\n");
        return 1;
    }

    // Dispatch to the right function
    if (strcmp(command, "add") == 0) {
        add_report(district, user, role);
    } else if (strcmp(command, "list") == 0) {
        list_reports(district, role, user);
    } else if (strcmp(command, "view") == 0) {
        view_report(district, report_id, role, user);
    } else if (strcmp(command, "remove_report") == 0) {
        remove_report(district, report_id, role, user);
    } else if (strcmp(command, "update_threshold") == 0) {
        update_threshold(district, report_id, role, user);
    } else if (strcmp(command, "filter") == 0) {
        // Find where conditions start in argv
        int condition_start = 0;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--filter") == 0) {
                condition_start = i + 2;  // skip --filter and district
                break;
            }
        }
        filter_reports(district, role, user, argc, argv, condition_start);
    } 

    return 0;
}