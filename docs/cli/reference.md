# CLI Reference

The `cob` executable provides a set of cli flags to modulate behavior.

## Synopsis

```bash
cob [options]
```

## Options

| Option | Description | Defaults |
|--------|-------------|----------|
| `-h, --help` | Show the help message and exit. | N/A |
| `-v, --version` | Show the version information and exit. | N/A|
| `-d <dir>` | Change the working directory before performing any operations. | Current Working Directory |
| `-f <file>` | Use `<file>` as the build manifest. | `catalyst.build`. |
| `-e <estimate>` | Use `<estimate>` as the estimates file. | `catalyst.estimates`. |
| `--build-log <file>` | Log combined build stdout/stderr to `<file>` when the `logging` feature is enabled. | Disabled |
| `-j, --jobs <N>` | Set the number of parallel jobs. | Maximum number of available hardware threads (``nproc``). |
| `--dry-run` | Print the commands that would be executed without actually running them. | N/A |
| `--clean` | Remove all generated build artifacts defined in the manifest (including sidecar `.d` files). | N/A|
| `--compdb` | Generate a `compile_commands.json` file for integration with clangd and other IDEs. | N/A |
| `--graph` | Print a `.dot` file to stdout for build graph inspection. Files that need rebuild are colored green. | N/A |
