# AI Usage Documentation

# Phase 1

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
The AI handled the type differences well — it knew to use integer comparison for `severity`, string comparison for `category` and `inspector`, and `time_t` for `timestamp`. That said, it still produced a dead variable, which is a good reminder to always read through generated code rather than just dropping it in. One thing worth noting: timestamp comparisons expect Unix timestamps as input values, not human-readable dates, so that's something to keep in mind when writing filter conditions. I also used AI to help me better understand and work with some predefined functions from various libraries, as well as other functions throughout my code, which helped simplify certain parts of the implementation.

---

# Phases 2 & 3

## Tool Used
DeepSeek

## What I Used AI For
Across both phases I used DeepSeek mostly as a reference tool to understand how certain library functions and system calls work, rather than to generate full blocks of code. It also gave me some structural suggestions for how to approach specific parts of the implementation. Specifically:

- **`errno` and error handling** — I asked DeepSeek to explain how `errno` is set by system calls and how to properly check and print error messages using `perror()` and `strerror()`. I wasn't sure when `errno` gets overwritten, so I needed to understand the order of operations when checking it after a failed call.
- **`time.h` functions** — I used it to clarify how `time()`, `localtime()`, and `strftime()` work together for formatting timestamps when writing to the log file. The relationship between `time_t`, `struct tm`, and formatted strings wasn't immediately obvious to me.
- **`fcntl()` flags** — I asked for an explanation of `F_GETFL` and `F_SETFL`, specifically how to use them to retrieve and modify file descriptor flags. This came up when I needed to understand the state of a file descriptor before modifying it, as well as setting flags like `O_NONBLOCK` on pipe ends.
- **`sigaction()` vs `signal()`** — I asked DeepSeek to explain why `sigaction()` is preferred and how to properly set up handlers for `SIGUSR1` and `SIGINT`. It also clarified which functions are safe to call inside a signal handler (async-signal-safe functions).
- **`pipe()` and `dup2()`** — I asked for a breakdown of how these two work together to redirect a child process's stdout into a pipe the parent can read from. DeepSeek walked me through the general steps: create the pipe before forking, close the unused ends after forking, and use `dup2()` to replace the child's stdout with the write end before calling `exec()`.
- **`memset()`** — DeepSeek suggested using `memset()` to zero out buffers before reading into them, which helps avoid issues with leftover data in memory when reading messages from a pipe. I hadn't considered this before and it turned out to be a clean and simple improvement.
- **Scorer process structure** — I asked for suggestions on how to structure the scorer: what steps to follow, how to open and read the binary report file, how to accumulate severity scores per inspector, and how to format the output. The AI gave me a general outline which I then implemented myself using the appropriate system calls.

## What I Changed
Since I was mostly using AI to understand concepts rather than generate code, the changes were about how I applied the knowledge. After understanding how `errno` works, I made sure to save its value immediately after a failed system call before making any other calls that could overwrite it. For `sigaction()`, I adapted the general structure shown to me to fit my specific handler logic. The `memset()` suggestion I adopted directly. For the scorer outline, I filled in all the actual implementation myself.

## What I Learned
Using AI as a documentation assistant was genuinely useful across these phases. Instead of spending a lot of time digging through man pages for unfamiliar functions, I could get a plain-language explanation with a small example and then verify it myself. That said, I still double-checked everything against the actual man pages, since AI explanations can sometimes oversimplify and leave out important edge cases — for example, the subtleties around which functions are truly async-signal-safe, or the exact behavior of pipe reads when the write end isn't properly closed.
