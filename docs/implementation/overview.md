# Implementation Details

## Core Architecture

COB operates in three phases:

1.  Parsing: The `catalyst.build` manifest is parsed into a linear sequence of definitions and steps.
2.  Graph Construction: A buildgraph is built. This is where implicit dependencies are discovered and cycles are found.
3.  Execution: The graph is traversed, and tasks are scheduled onto a thread pool.

## Dependency Tracking (`.d` Files)

COB tracks implicit dependencies, such as files included by ``#include`` ``/#include_next``.

### How it works

1.  Injection: When executing a `cc` or `cxx` step, COB passes in the `-MMD -MF <output>.d` flags to the compiler.
This instructs the compiler (GCC/Clang) to emit a Make-compatible dependency file alongside the object file for the
next build.
2.  Discovery: On the next pass, during graph construction, COB checks for the existence of these `.d` files from the previous build.
3.  Parsing: COB reads the `.d` file and extracts every input into the build graph.
4.  Graph augmentation: These discovered prerequisites are added as nodes in the build graph, with edges pointing to the object file.

### Staleness Check

During execution, COB checks the modification time of:
-  Source input files.
-  The output object file.
-  Dependencies discovered from the `.d` file.

If *any* dependency is newer than the object file, the step is re-executed.

## Automatic Response Files (`.rsp`)

To overcome operating system limits on command-line length (which can be exceeded when linking large projects), COB implements automatic response file generation.

### The Mechanism

For the `ld` (Link) step:

1.  Threshold Check: COB checks the number of input files. The current internal threshold is **50 inputs**.
2.  Generation: If the number of inputs exceeds this threshold, COB generates a temporary file named `<output>.rsp`.
3.  Content: This file contains the list of all input files, separated by newlines.
4.  Command Mutation: The command line is altered. Instead of passing `obj1.o obj2.o ...`, COB
passes `@<output>.rsp`. The linker (``ld/link.exe/etc.``) reads the arguments from this file.

This process ensures builds don't fail due to command line limits.

## Performance Optimizations

### Memory Mapped I/O
COB extensively uses `mmap` (on Linux/macOS) or `CreateFileMapping` (on Windows) for reading source files and
dependency files. This reduces the number of system calls and allows the OS to manage page caching efficiently.

### Thread Pool
A thread pool consumes tasks from a ready-queue. This queue is populated by a topological sort of the build graph.

### Stat Caching
COB populates a stat cache to ensure that "popular" dependencies don't invoke unnecesary stat syscalls.

## Work Estimation

COB supports prioritizing tasks based on estimated costs. See [Work Estimates](work_estimate.md) for details.
