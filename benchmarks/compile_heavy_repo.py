import generate_heavy_repo
import os

import subprocess
import time


def benchmark_catalyst_builds():
    """
    Benchmarks make, ninja, and cbe within the generate_heavy_repo.ROOT_DIR,
    presenting timings in a structured layout.
    """

    # Target directory for the -C flag
    root_dir = generate_heavy_repo.ROOT_DIR

    # Get paths dynamically to avoid hardcoded user home directories
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    cbe_bin = os.path.join(project_root, "build/common-ccache-release/cbe")
    cbe_system = os.path.expanduser("~/.local/bin/cbe")
    tasks = [
        ("make", ["make", "-C", root_dir], True),
        ("ninja -t clean", ["ninja", "-C", root_dir, "-t", "clean"], False),  # Initial setup clean
        ("ninja", ["ninja", "-C", root_dir], True),
        ("ninja -t clean", ["ninja", "-C", root_dir, "-t", "clean"], True),
        ("cbe (system)", [cbe_system, "-C", root_dir], True),
        ("cbe (system) -t clean", [cbe_system, "-C", root_dir, "-t", "clean"], True),
        ("cbe (built)", [cbe_bin, "-C", root_dir], True),
        ("cbe (built) -t clean", [cbe_bin, "-C", root_dir, "-t", "clean"], True),
    ]

    results = []

    print(f"Benchmarking in: {root_dir}...\n")

    for label, cmd, should_time in tasks:
        try:
            if should_time:
                start = time.perf_counter()
                subprocess.run(cmd, check=True, capture_output=True, text=True)
                end = time.perf_counter()
                results.append((label, f"{end - start:.4f}s"))
            else:
                subprocess.run(cmd, check=True, capture_output=True)

        except FileNotFoundError:
            if should_time:
                results.append((label, "Not Found"))
        except subprocess.CalledProcessError as e:
            if should_time:
                results.append((label, "Failed"))
            print(f"Error running {label}: {e.stderr}")

    # Layout Presentation
    left_shift = 0
    for label, _ in results:
        left_shift = max(left_shift, len(label) + 2)
    right_shift = 0
    for _, duration in results:
        right_shift = max(right_shift, len("Execution Time") + 2)
    header = f"{'Build Command':<{left_shift}} | {'Execution Time':>{right_shift}}"
    divider = "-" * len(header)

    print(divider)
    print(header)
    print(divider)

    for label, duration in results:
        print(f"{label:<{left_shift}} | {duration:>{right_shift}}")

    print(divider)
    print("Note: 'make clean' was not measured.")


def main():
    generate_heavy_repo.ensure_dirs()
    print("Generating headers...")
    generate_heavy_repo.generate_headers()
    print("Generating sources...")
    src_info = generate_heavy_repo.generate_sources()
    srcs = [name for name, _ in src_info]

    print("Generating catalyst manifest...")
    generate_heavy_repo.generate_catalyst_manifest(srcs)

    print("Generating catalyst estimates...")
    generate_heavy_repo.generate_estimates(src_info)

    print("Generating ninja manifest...")
    generate_heavy_repo.generate_ninja_manifest(srcs)

    print("Generating makefile...")
    generate_heavy_repo.generate_makefile(srcs)

    benchmark_catalyst_builds()


if __name__ == "__main__":
    main()
