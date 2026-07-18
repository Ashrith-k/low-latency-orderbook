#include <iostream>
#include <string>
#include <vector>

#include "lob_replay_lib.h"

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  return lob::tools::run_cli(args, std::cout, std::cerr);
}
