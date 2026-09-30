# Network 모듈: 멀티플레이 전송 계층(GameNetworkingSockets / 테스트용 루프백), 연결 수립, 넷 모드, 복제 (Phase 17)
# 모듈 의존: Network → Scene → Core
# GNS 헤더는 이 모듈의 .cpp에서만 포함한다 (공개 헤더는 INetTransport 추상화만 노출)
file(GLOB_RECURSE NETWORK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ENetwork ${E_ENGINE_MODULE_TYPE} ${NETWORK_SOURCES})
add_library(ProjectE::Network ALIAS ENetwork)

target_include_directories(ENetwork PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ENetwork
    PUBLIC  ProjectE::Core ProjectE::Scene
    PRIVATE ThirdParty::gns)

e_set_engine_module_defaults(ENetwork)
