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

/* Report structure*/
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

/* Tracks workload score for a single inspector */
typedef struct {
    char name[MAX_NAME];
    int  score;
    int  report_count;
} InspectorScore;

/* Main function: calculates workload scores for a district
   Output goes to stdout, redirected by city_hub via pipe */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <district>\n", argv[0]);
        return 1;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", argv[1]);

    // Verify district exists
    struct stat st;
    if (stat(argv[1], &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "ERROR: district '%s' does not exist\n", argv[1]);
        return 1;
    }

    // Open reports file
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open reports for district '%s'\n", argv[1]);
        return 1;
    }

    // Array to store inspector scores (max 100 unique inspectors)
    InspectorScore scores[100];
    int score_count = 0;
    memset(scores, 0, sizeof(scores));

    // Read all reports and accumulate scores per inspector
    Report r;
    ssize_t bytes_read;
    while ((bytes_read = read(fd, &r, sizeof(Report))) == sizeof(Report)) {
        // Ensure string fields are null-terminated
        r.inspector[MAX_NAME - 1] = '\0';
        r.category[MAX_CATEGORY - 1] = '\0';
        r.description[MAX_DESC - 1] = '\0';
        
        // Find existing inspector or create new entry
        int found = 0;
        for (int i = 0; i < score_count; i++) {
            if (strcmp(scores[i].name, r.inspector) == 0) {
                scores[i].score += r.severity;
                scores[i].report_count++;
                found = 1;
                break;
            }
        }
        
        if (!found && score_count < 100) {
            strncpy(scores[score_count].name, r.inspector, MAX_NAME - 1);
            scores[score_count].name[MAX_NAME - 1] = '\0';
            scores[score_count].score = r.severity;
            scores[score_count].report_count = 1;
            score_count++;
        } else if (!found) {
            fprintf(stderr, "Warning: Maximum inspector limit (100) reached\n");
            break;
        }
    }
    
    // Check for partial/corrupted record
    if (bytes_read > 0 && bytes_read != sizeof(Report)) {
        fprintf(stderr, "Warning: partial record found in '%s' (corrupted file?)\n", path);
    }

    close(fd);

    // Output results (goes through pipe to city_hub)
    printf("SCORE:%s:", argv[1]);
    if (score_count == 0) {
        printf("no reports found\n");
    } else {
        printf("%d inspectors\n", score_count);
        for (int i = 0; i < score_count; i++) {
            printf("  %s: %d (from %d reports)\n", 
                   scores[i].name, scores[i].score, scores[i].report_count);
        }
    }

    return 0;
}