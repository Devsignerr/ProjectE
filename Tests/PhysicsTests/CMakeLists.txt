# Physics 모듈 단위 테스트 (변환 규약, 고정 스텝, 낙하/정지/레이캐스트/반발/마찰, 씬 동기화)
file(GLOB_RECURSE PHYSICS_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(PhysicsTests ${PHYSICS_TEST_SOURCES})
target_link_libraries(PhysicsTests PRIVATE ProjectE::Engine ThirdParty::jolt) # Jolt: 쿼터니언 규약 비교 테스트

e_set_target_defaults(PhysicsTests "Tests")

add_test(NAME PhysicsTests COMMAND PhysicsTests)
