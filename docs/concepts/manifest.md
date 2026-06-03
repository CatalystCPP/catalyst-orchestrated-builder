# Build File Schema

The `catalyst.build` manifest defines the build graphs and arguments for build
rules. It uses a strict, pipe-delimited, 3-column format designed for efficient
parsing.

## Structure

The file can have either of: Definitions or Build Steps.

### Definitions

Definitions configure the toolchain and environment variables.

Format: `DEF|<var>|<val>`

- `DEF`: The introducer for a definition line. Must be capitalized.
- `<var>`: The variable name. While keys in `DEF` can be anything, only specific ones are respected by the build system.
- `<val>`: The value assigned to the variable. Empty values use `DEF|<var>|`.

#### Supported Variables

| Variable | Description | Example |
| ---- | ---- | ---- |
| `cc` | The C compiler executable. | `/usr/bin/clang` |
| `cxx` | The C++ compiler executable. | `/usr/bin/clang++` |
| `linker` | The linker executable (defaults to `cxx` if empty/omitted). | `/usr/bin/clang++` or `/usr/bin/ld` |
| `archiver` | The static archiver executable (defaults to `ar`, `libtool`, or `lib` based on OS if empty/omitted). | `/usr/bin/ar` |
| `cflags` | Flags passed to `cc` steps. | `-O2 -Wall` |
| `cxxflags` | Flags passed to `cxx` steps. | `-std=c++20 -O3` |
| `ldflags` | Linker search paths and general flags. | `-L/usr/local/lib` |
| `ldlibs` | Libraries to link against. | `-lpthread -lm` |

### Build Steps

Build steps define the actions to transform input files into output files.

Format: `<step_type>|<input_list>|<output_file>`

-   `<step_type>`: Mnemonic for the tool to use (see Toolchain Mapping).
-   `<input_list>`: Comma-separated list of input files.
-   `<output_file>`: The path to the generated file.

#### Toolchain Mapping

COB maps specific step types to command templates. It strictly enforces certain
behaviors (like dependency generation) by injecting flags.

| Type | Description | Command Template (Approximation) |
| ---- | ---- | ---- |
| `cc` | C Compile | `$cc $cflags -MMD -MT $out -MF $out.d -c $in -o $out` |
| `cxx` | C++ Compile | `$cxx $cxxflags -MMD -MT $out -MF $out.d -c $in -o $out` |
| `ld` | Binary Link | `$cxx $in -o $out $ldflags $ldlibs` |
| `ar` | Static Link | `ar rcs $out $in` |
| `sld` | Shared Link | `$cxx -shared $in -o $out` |


#### Opaque Dependencies

Inputs prefixed with `!` are treated as "opaque dependencies".
These inputs are tracked for changes to trigger rebuilds but are not passed as
arguments to the build tool.

Example: `cxx|src/main.cpp,!resource.png|build/main.o`

*   `src/main.cpp` is passed to the compiler.
*   `resource.png` is not passed to the compiler, but modifying it will trigger
a rebuild of `build/main.o`.

This is useful for implicit dependencies (like configuration files or binary
content) that affect the build output but aren't direct inputs to the command
line.

Important Notes(See [Implementation Details](../implementation/overview.md) for more info):

1.  Dependency Tracking (`.d`): For `cc` and `cxx`, __do not__ manually add `-MMD`, `-MT`, or `-MF` to your `$cflags`/`$cxxflags`. COB automatically injects these to manage incremental builds correctly.
2.  Response Files (`.rsp`): For `ld` steps with many inputs (currently >50), COB will automatically generate a response file and pass it via `@<out>.rsp` to avoid command-line length limits.

## Example Manifest

```text
DEF|cc|clang
DEF|cxx|clang++
DEF|cflags|
DEF|cxxflags|-std=c++20 -O3
DEF|ldflags|
DEF|ldlibs|
cxx|src/main.cpp,!resources/helpmessage.txt|build/main.o
cxx|src/net.cpp|build/net.o
ld|build/main.o,build/net.o|build/app
```
