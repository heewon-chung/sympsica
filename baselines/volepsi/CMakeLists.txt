cmake_minimum_required(VERSION 3.18)
project(sympsica_volepsi_driver CXX)
set(CMAKE_CXX_STANDARD 20)
# FINDING (Task 33, first real build of this substrate): volePSI's own installed
# CMake package is named "volePSI" (mixed case) with exported target
# visa::volePSI (verified against the built /usr/local/lib/cmake/volePSI/{volePSIConfig,
# volePSITargets}.cmake) -- find_package is case-sensitive on Linux, so the
# lowercase "volepsi"/"visa::volepsi" spelling never resolves. Corrected below;
# this is a mechanical namespace-qualifier fix, not a change to any protocol logic.
find_package(volePSI REQUIRED HINTS ${VOLEPSI_HINT})
add_executable(rr22_driver rr22_driver.cpp)
target_link_libraries(rr22_driver visa::volePSI)
