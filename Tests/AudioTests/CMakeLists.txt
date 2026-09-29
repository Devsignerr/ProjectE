# Audio 모듈 단위 테스트 (장치 없는 miniaudio 엔진으로 믹스 결과를 직접 검증)
file(GLOB_RECURSE AUDIO_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(AudioTests ${AUDIO_TEST_SOURCES})
target_link_libraries(AudioTests PRIVATE ProjectE::Audio ProjectE::Scene)

e_set_target_defaults(AudioTests "Tests")

add_test(NAME AudioTests COMMAND AudioTests)
