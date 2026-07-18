# Command-line tools (DESIGN §8). Logic lives in lob_replay_lib.h so the test
# suite can drive run_cli() directly; the executable is a thin main().
add_executable(lob_replay lob_replay.cc)
target_link_libraries(lob_replay PRIVATE lob lob_warnings)

# One smoke test of the real binary — the gtest suite covers run_cli()'s
# logic; this proves the executable links and runs.
if(LOB_BUILD_TESTS)
  add_test(NAME lob_replay_help_smoke COMMAND lob_replay --help)
endif()
