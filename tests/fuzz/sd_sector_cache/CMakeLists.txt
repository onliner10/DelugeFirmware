# Fuzzer for the SD sector cache (src/RZA1/sd_sector_cache.c), with AddressSanitizer and UndefinedBehaviorSanitizer.
# Needs clang with libFuzzer:
#   CC=clang cmake -B build/fuzz-sd-sector-cache -S tests/fuzz/sd_sector_cache -G Ninja
#   cmake --build build/fuzz-sd-sector-cache
#   build/fuzz-sd-sector-cache/sd_sector_cache_fuzz -max_len=512 -runs=1000000
cmake_minimum_required(VERSION 3.24)
project(SdSectorCacheFuzz C)

add_executable(sd_sector_cache_fuzz sd_sector_cache_fuzz.c ../../../src/RZA1/sd_sector_cache.c)
target_include_directories(sd_sector_cache_fuzz PRIVATE ../../../src)
target_compile_definitions(sd_sector_cache_fuzz PRIVATE IN_UNIT_TESTS=1)
target_compile_features(sd_sector_cache_fuzz PRIVATE c_std_11)

set(options
  -fsanitize=fuzzer,address,undefined
  -fno-sanitize-recover=all
  -fno-omit-frame-pointer
  -g
  -O1
)
target_compile_options(sd_sector_cache_fuzz PRIVATE ${options})
target_link_options(sd_sector_cache_fuzz PRIVATE ${options})
