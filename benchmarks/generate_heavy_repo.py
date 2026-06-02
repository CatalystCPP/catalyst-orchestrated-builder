import os
import random

ROOT_DIR = "benchmarks/heavy_repo"
SRC_DIR = os.path.join(ROOT_DIR, "src")
INCLUDE_DIR = os.path.join(ROOT_DIR, "include")
BUILD_DIR = os.path.join(ROOT_DIR, "build")

NUM_HEADERS = 1000
NUM_SOURCES = 1000
HEADERS_PER_HEADER = 3
HEADERS_PER_SOURCE = 10


def ensure_dirs():
    os.makedirs(SRC_DIR, exist_ok=True)
    os.makedirs(INCLUDE_DIR, exist_ok=True)
    # Ninja usually requires the build directory to exist beforehand
    os.makedirs(BUILD_DIR, exist_ok=True)


def generate_headers():
    for i in range(NUM_HEADERS):
        filename = os.path.join(INCLUDE_DIR, f"header_{i}.hpp")
        with open(filename, "w") as f:
            f.write(f"#pragma once\n\n")
            f.write(f"// Header {i}\n")

            # Include previous headers to ensure DAG (no cycles)
            if i > 0:
                potential_deps = list(range(i))
                num_deps = min(len(potential_deps), HEADERS_PER_HEADER)
                deps = random.sample(potential_deps, num_deps)
                for dep in deps:
                    f.write(f"#include \"header_{dep}.hpp\"\n")

            f.write(f"\ninline int func_{i}() {{ return {i}; }}\n")


def generate_sources():
    source_info = []
    for i in range(NUM_SOURCES):
        name = f"source_{i}.cpp"
        filename = os.path.join(SRC_DIR, name)

        # Vary the number of included headers for different lengths
        num_deps = random.randint(5, 150)
        source_info.append((name, num_deps))

        with open(filename, "w") as f:
            f.write(f"// Source {i}\n")

            # Include random headers
            deps = random.sample(range(NUM_HEADERS), min(NUM_HEADERS, num_deps))
            for dep in deps:
                f.write(f"#include \"header_{dep}.hpp\"\n")

            f.write(f"\nint source_func_{i}() {{ \n")
            f.write(f"    int sum = 0;\n")
            for dep in deps:
                f.write(f"    sum += func_{dep}();\n")
            f.write(f"    return sum;\n")
            f.write(f"}}\n")

    # Main file
    with open(os.path.join(SRC_DIR, "main.cpp"), "w") as f:
        f.write("#include <iostream>\n")
        for i in range(NUM_SOURCES):
            f.write(f"int source_func_{i}();\n")

        f.write("\nint main() {\n")
        f.write("    int total = 0;\n")
        for i in range(NUM_SOURCES):
            f.write(f"    total += source_func_{i}();\n")
        f.write("    std::cout << \"Total: \" << total << std::endl;\n")
        f.write("    return 0;\n")
        f.write("}\n")

    return source_info


def generate_catalyst_manifest(source_files):
    manifest_path = os.path.join(ROOT_DIR, "catalyst.build")
    with open(manifest_path, "w") as f:
        f.write("DEF|cc|clang\n")
        f.write("DEF|cxx|clang++\n")
        f.write("DEF|cflags|\n")
        f.write("DEF|cxxflags|-std=c++20 -O0 -Iinclude\n")
        f.write("DEF|ldflags|\n")
        f.write("DEF|ldlibs|\n")

        obj_files = []

        # Sources
        for src in source_files:
            obj_name = f"build/{src}.o"
            obj_files.append(obj_name)
            f.write(f"cxx|src/{src}|{obj_name}\n")

        # Main
        obj_files.append("build/main.cpp.o")
        f.write(f"cxx|src/main.cpp|build/main.cpp.o\n")

        # Link
        all_objs = ",".join(obj_files)
        f.write(f"ld|{all_objs}|build/heavy_app\n")


def generate_ninja_manifest(source_files):
    manifest_path = os.path.join(ROOT_DIR, "build.ninja")
    with open(manifest_path, "w") as f:
        # Preamble: Variables
        f.write("cxx = clang++\n")
        f.write("cxxflags = -std=c++20 -O0 -Iinclude\n")
        f.write("ldflags = \n")
        f.write("\n")

        f.write("rule cxx\n")
        f.write("  command = $cxx $cxxflags -MMD -MT $out -MF $out.d -c $in -o $out\n")
        f.write("  description = CXX $out\n")
        f.write("  depfile = $out.d\n")
        f.write("  deps = gcc\n")
        f.write("\n")

        f.write("rule ld\n")
        f.write("  command = $cxx $ldflags $in -o $out\n")
        f.write("  description = LINK $out\n")
        f.write("\n")

        obj_files = []

        for src in source_files:
            obj_name = f"build/{src}.o"
            obj_files.append(obj_name)
            f.write(f"build {obj_name}: cxx src/{src}\n")

        # Build Statement: Main
        main_obj = "build/main.cpp.o"
        obj_files.append(main_obj)
        f.write(f"build {main_obj}: cxx src/main.cpp\n")

        # Build Statement: Link
        # Join objects with spaces for Ninja
        all_objs = " ".join(obj_files)
        f.write(f"\nbuild build/heavy_app: ld {all_objs}\n")



def generate_makefile(source_files):
    manifest_path = os.path.join(ROOT_DIR, "Makefile")
    with open(manifest_path, "w") as f:
        f.write("CXX = clang++\n")
        f.write("CXXFLAGS = -std=c++20 -O0 -Iinclude\n")
        f.write("LDFLAGS = \n")
        f.write("\n")

        obj_files = []
        for src in source_files:
            obj_name = f"build/{src}.o"
            obj_files.append(obj_name)

        main_obj = "build/main.cpp.o"
        obj_files.append(main_obj)

        all_objs = " ".join(obj_files)

        f.write(f"build/heavy_app: {all_objs}\n")
        f.write(f"\t$(CXX) $(LDFLAGS) {all_objs} -o $@\n\n")

        for src in source_files:
            obj_name = f"build/{src}.o"
            f.write(f"{obj_name}: src/{src}\n")
            f.write(f"\t$(CXX) $(CXXFLAGS) -MMD -MP -MF {obj_name}.d -c $< -o $@\n")

        f.write(f"{main_obj}: src/main.cpp\n")
        f.write(f"\t$(CXX) $(CXXFLAGS) -MMD -MP -MF {main_obj}.d -c $< -o $@\n")

        f.write("\n")
        deps = [o + ".d" for o in obj_files]
        f.write("-include " + " ".join(deps) + "\n")

        f.write("clean:\n")
        obj_files_clean_cmd = "\trm "
        for obj in obj_files:
            obj_files_clean_cmd += (obj + " ")
        f.write(obj_files_clean_cmd)


def generate_estimates(source_info):
    estimates_path = os.path.join(ROOT_DIR, "catalyst.estimates")
    with open(estimates_path, "w") as f:
        entries = []
        for src, num_deps in source_info:
            obj_name = f"build/{src}.o"
            entries.append((obj_name, num_deps))

        # Add main and link step estimates
        entries.append(("build/main.cpp.o", 80))
        entries.append(("build/heavy_app", 300))

        # Sort lexicographically by path
        entries.sort()

        for path, est in entries:
            f.write(f"{path}|{est}\n")


if __name__ == "__main__":
    ensure_dirs()
    print("Generating headers...")
    generate_headers()
    print("Generating sources...")
    src_info = generate_sources()
    srcs = [name for name, _ in src_info]

    print("Generating catalyst manifest...")
    generate_catalyst_manifest(srcs)

    print("Generating catalyst estimates...")
    generate_estimates(src_info)

    print("Generating ninja manifest...")
    generate_ninja_manifest(srcs)

    print("Generating makefile...")
    generate_makefile(srcs)

    print("Done.")
