## AI Usage Documentation

## Tool Used
DeepSeek

## Prompt Given
I have a C struct called Report defined as:
```c
typedef struct {
    int     id;
    char    inspector[64];
    double  latitude;
    double  longitude;
    char    category[32];
    int     severity;
    time_t  timestamp;
    char    description[256];
} Report;
```
Please generate two functions:
1. `int parse_condition(const char *input, char *field, char *op, char *value);`
   This splits a string of the form "field:operator:value" into its three parts.
2. `int match_condition(Report *r, const char *field, const char *op, const char *value);`
   This returns 1 if the report matches the condition, 0 otherwise.
   Supported fields: severity, category, inspector, timestamp.
   Supported operators: ==, !=, <, <=, >, >=.

## What Was Generated
The AI generated five functions in total — `parse_condition` and `match_condition` as requested, plus three helper functions it added on its own: `compare_strings`, `compare_ints`, and `compare_time`.

## What I Changed
I removed an unused variable `value_len` in `parse_condition` that was declared but never actually used — it was throwing a compiler warning. I also stripped out some duplicate `#include` statements that were already in my code.

## What I Learned
The AI handled the type differences well — it knew to use integer comparison for `severity`, string comparison for `category` and `inspector`, and `time_t` for `timestamp`. That said, it still produced a dead variable, which is a good reminder to always read through generated code rather than just dropping it in. One thing worth noting: timestamp comparisons expect Unix timestamps as input values, not human-readable dates, so that's something to keep in mind when writing filter conditions.
