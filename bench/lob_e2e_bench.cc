#include <iostream>
#include <string>
#include <vector>

#include "e2e_bench_lib.h"

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  return lob::bench::run_e2e_cli(args, std::cout, std::cerr);
}
