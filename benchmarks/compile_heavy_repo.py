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
    cbe_bin = os.path.join(project_root, "build/common-ccache/cbe")
    cbe_system = os.path.expanduser("~/.local/bin/cbe")
    tasks = [
        ("make", ["make", "-C", root_dir], True),
        ("ninja -t clean", ["ninja", "-C", root_dir,
         "-t", "clean"], False),  # Initial setup clean
        ("ninja", ["ninja", "-C", root_dir], True),
        ("ninja -t clean", ["ninja", "-C", root_dir, "-t", "clean"], True),
        ("cbe (system)", [cbe_system, "-C", root_dir], True),
        ("cbe (system) -t clean", [cbe_system, "-C", root_dir, "-t", "clean"], True),
        ("cbe (system, mimalloc)", [cbe_system, "-C", root_dir], True),
        ("cbe (system, mimalloc) -t clean", [cbe_system, "-C", root_dir, "-t", "clean"], True),
        ("cbe (built, affinity)", [cbe_bin, "-C", root_dir], True),
        ("cbe (built, affinity) -t clean", [cbe_bin, "-C", root_dir, "-t", "clean"], True),
        ("cbe (built, affinity, mimalloc)", [cbe_bin, "-C", root_dir], True),
        ("cbe (built, affinity, mimalloc) -t clean", [cbe_bin, "-C", root_dir, "-t", "clean"], True)
    ]

    results = []

    print(f"Benchmarking in: {root_dir}...\n")

    for label, cmd, should_time in tasks:
        try:
            env = os.environ.copy()
            if "mimalloc" in label:
                env["LD_PRELOAD"] = "/usr/lib/x86_64-linux-gnu/libmimalloc.so"

            if should_time:
                start = time.perf_counter()
                if "cbe" in label:
                    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
                else:
                    subprocess.run(cmd, check=True, capture_output=True, text=True, env=env)
                end = time.perf_counter()
                results.append((label, f"{end - start:.4f}s"))
            else:
                # Run without timing (equivalent to the untimed ninja clean in your bash)
                subprocess.run(cmd, check=True, capture_output=True, env=env)

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
