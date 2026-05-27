import generate_heavy_repo

import subprocess
import time


def benchmark_catalyst_builds():
    """
    Benchmarks make, ninja, and cbe within the generate_heavy_repo.ROOT_DIR,
    presenting timings in a structured layout.
    """

    # Target directory for the -C flag
    root_dir = generate_heavy_repo.ROOT_DIR

    # Define the command sequence: (Label, Command List, Should Time)
    cbe_bin = "/home/som/Projects/catalyst/catalyst-build-executor/build/common-ccache/cbe"
    tasks = [
        ("make", ["make", "-C", root_dir], True),
        ("ninja -t clean", ["ninja", "-C", root_dir,
         "-t", "clean"], False),  # Initial setup clean
        ("ninja", ["ninja", "-C", root_dir], True),
        ("ninja -t clean", ["ninja", "-C", root_dir, "-t", "clean"], True),
        ("cbe", [cbe_bin, "-C", root_dir], True),
        ("cbe -t clean", [cbe_bin, "-C", root_dir, "-t", "clean"], True)
    ]

    results = []

    print(f"Benchmarking in: {root_dir}...\n")

    for label, cmd, should_time in tasks:
        try:
            if should_time:
                start = time.perf_counter()
                if "cbe" in label:
                    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                else:
                    subprocess.run(cmd, check=True, capture_output=True, text=True)
                end = time.perf_counter()
                results.append((label, f"{end - start:.4f}s"))
            else:
                # Run without timing (equivalent to the untimed ninja clean in your bash)
                subprocess.run(cmd, check=True, capture_output=True)

        except FileNotFoundError:
            if should_time:
                results.append((label, "Not Found"))
        except subprocess.CalledProcessError as e:
            if should_time:
                results.append((label, "Failed"))
            print(f"Error running {label}: {e.stderr}")

    # Layout Presentation
    header = f"{'Build Command':<20} | {'Execution Time':<15}"
    divider = "-" * len(header)

    print(divider)
    print(header)
    print(divider)

    for label, duration in results:
        print(f"{label:<20} | {duration:<15}")

    print(divider)
    print("Note: 'make clean' was not measured.")


def main():
    generate_heavy_repo.ensure_dirs()
    print("Generating headers...")
    generate_heavy_repo.generate_headers()
    print("Generating sources...")
    srcs = generate_heavy_repo.generate_sources()

    print("Generating catalyst manifest...")
    generate_heavy_repo.generate_catalyst_manifest(srcs)

    print("Generating ninja manifest...")
    generate_heavy_repo.generate_ninja_manifest(srcs)

    print("Generating makefile...")
    generate_heavy_repo.generate_makefile(srcs)

    benchmark_catalyst_builds()


if __name__ == "__main__":
    main()
