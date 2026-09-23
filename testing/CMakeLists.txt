# testing/ - development-only targets. Pulled in by GNP_BUILD_TESTING=ON.
#
# Everything here links against gnp_core, the same library the production
# `gnp` binary uses. Nothing in gnp_core or gnp links anything from here.

add_executable(gnp_sim sim/main_sim.cpp sim/sim_inject.cpp)
target_include_directories(gnp_sim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gnp_sim PRIVATE gnp_core)
gnp_set_warnings(gnp_sim)

add_executable(test_ring unit/test_ring.cpp)
target_link_libraries(test_ring PRIVATE gnp_core)
gnp_set_warnings(test_ring)
add_test(NAME ring_protocol COMMAND test_ring)
