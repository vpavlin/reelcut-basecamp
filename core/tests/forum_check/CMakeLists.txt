# Reelcut's forum posts checked by each forum's own library: their decoders and verifiers must take
# what Reelcut writes. Needs the two repos checked out (and libsodium, sqlite, nlohmann_json):
#   cmake -S . -B build -DFORUMER_DIR=~/forumer -DLOGOS_FORUM_DIR=~/lp-0026-forum && cmake --build build && ./build/forumtest
# Last run against forumer 7533c2a (v0.2.5) and lp-0026-forum 8f08f52 (0.4.0): 29/29.
cmake_minimum_required(VERSION 3.16)
project(forumtest CXX)
set(CMAKE_CXX_STANDARD 20)
add_subdirectory(${FORUMER_DIR}/lib/forumer_core forumer_core)
add_subdirectory(${LOGOS_FORUM_DIR}/core forum_core)
add_executable(forumtest test.cpp ../../src/forum_post.cpp)
target_include_directories(forumtest PRIVATE ../../src)
target_link_libraries(forumtest PRIVATE forumer_core forum_core)
