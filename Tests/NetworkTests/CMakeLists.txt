# 네트워크 단위 테스트 (Phase 17). 현재: GameNetworkingSockets localhost 에코 (전송 계층 도입 시험)
file(GLOB_RECURSE NETWORK_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(NetworkTests ${NETWORK_TEST_SOURCES})
target_link_libraries(NetworkTests PRIVATE ProjectE::Engine ThirdParty::gns)

e_set_target_defaults(NetworkTests "Tests")

add_test(NAME NetworkTests COMMAND NetworkTests)
