# Network 모듈 단위 테스트 (Phase 17): 메시지 형식, 루프백/GNS 전송 계층, 연결 수립(FNetDriver), 명령줄
file(GLOB_RECURSE NETWORK_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(NetworkTests ${NETWORK_TEST_SOURCES})
target_link_libraries(NetworkTests PRIVATE ProjectE::Engine)

e_set_target_defaults(NetworkTests "Tests")

add_test(NAME NetworkTests COMMAND NetworkTests)
