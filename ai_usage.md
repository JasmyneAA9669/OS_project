AI Usage Log: Report Filtering Logic
Tooling

Model: DeepSeek
The Objective

I needed to implement a filtering system for a Report struct. Specifically, I needed a way to parse a raw condition string (formatted as field:op:value) and then check if a given report met those criteria.

The Prompt:

    I asked the AI to generate two specific C functions: parse_condition for splitting the input string and match_condition to handle the logic for fields like severity, category, and timestamp.

Technical Output

The AI provided a solid foundation, including the two requested functions and three internal helpers (compare_strings, compare_ints, and compare_time) to handle the different data types in the struct.
My Refinements (The "Human" Part)

While the logic was mostly there, I had to clean it up to make it production-ready:

    Warning Cleanup: The AI left a dangling value_len variable in parse_condition. It was just noise causing compiler warnings, so I stripped it out.

    Boilerplate Removal: It included a bunch of headers I already had in my project. I trimmed those down to keep the file clean.

    Logic Check: Verified that the time_t comparison was actually using the right logic for Unix timestamps.

Key Takeaways

    Type Handling: It was interesting to see how the AI separated the comparison logic. It realized immediately that severity (int) and inspector (string) couldn't use the same logic, which saved me a lot of boilerplate typing.

    The "Trust but Verify" Rule: This was a good reminder that AI loves to hallucinate small variables or "helper" code that doesn't actually do anything.

    Input Limitations: I realized during testing that because we're using time_t, the user has to input raw Unix timestamps. It’s not the most user-friendly, but for a backend tool, it gets the job done without needing a full date-parsing library.
