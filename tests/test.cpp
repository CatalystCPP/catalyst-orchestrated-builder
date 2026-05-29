#include "tests/test_suite.hpp"

#include <cassert>

int main(int argc, char **argv) {
    return !(stat_cache_test() && integration_test());
}
