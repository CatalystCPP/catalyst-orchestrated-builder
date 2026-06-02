# Work Estimate

The `WorkEstimate` feature allows the build executor to prioritize tasks based on their estimated duration or cost. This can improve build times by scheduling heavier tasks earlier (e.g., Longest Processing Time first).

## Class: `catalyst::WorkEstimate`

Defined in `include/cob/work_estimate.hpp`.

### Responsibilities
- Loads estimates from a specified file.
- Provides a lookup mechanism to retrieve the work estimate for a given file path.

## File Format

The estimates file is a text file where each line represents an estimate for a specific file.
The format for each line is:

```
<file_path>|<estimate>
```

- `<file_path>`: The path to the file (string).
- `<estimate>`: The estimated work unit (integer).
- The separator is a pipe character `|`.

Example:
```
src/main.cpp|150
src/utils.cpp|45
src/heavy_compilation.cpp|5000
```

## Implementation Details

- The file is memory-mapped for performance using `catalyst::MappedFile`.
- Estimates are stored in a `std::unordered_map<std::string_view, std::string_view>` for fast lookups.
- The values are parsed on demand using `std::from_chars`.

### Usage

```cpp
#include "cob/work_estimate.hpp"

// ...

catalyst::WorkEstimate estimates("path/to/estimates.txt");
size_t work = estimates.get_work_estimate("src/main.cpp");
```

If a path is not found in the estimates file or if parsing fails, `get_work_estimate` returns `0`.
