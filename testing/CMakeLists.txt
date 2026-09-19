# testing/ - development-only targets. Pulled in by GNP_BUILD_TESTING=ON.
#
# Everything here links against gnp_core, the same library the production
# `gnp` binary uses. Nothing in gnp_core or gnp links anything from here.

add_library(gnp_testing_sim STATIC sim/sim_inject.cpp)
target_include_directories(gnp_testing_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gnp_testing_sim PUBLIC gnp_core)

add_executable(gnp_sim sim/main_sim.cpp)
target_link_libraries(gnp_sim PRIVATE gnp_testing_sim)

add_executable(test_ring unit/test_ring.cpp)
target_link_libraries(test_ring PRIVATE gnp_core)
add_test(NAME ring_protocol COMMAND test_ring)
