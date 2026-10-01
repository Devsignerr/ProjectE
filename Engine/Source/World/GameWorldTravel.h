#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

class FGameWorld;
class FNetDriver;
class FNetPlayerSpawner;
class FReplicationClient;
class FReplicationServer;
class FResourceManager;
class FScene;

// 맵 전환이 다루는 앱 객체. 모두 비소유(앱이 소유), 없으면 nullptr
struct FSceneTravelTargets
{
	FGameWorld*         World             = nullptr; // 필수
	FScene*             Scene             = nullptr; // 필수: 내용만 비우고 새 씬을 연다 (객체는 그대로 — 앱/에디터가 든 씬 포인터 유지)
	FNetDriver*         Net               = nullptr; // 없으면 Standalone
	FReplicationServer* ReplicationServer = nullptr; // 서버/Standalone: 새 씬 정적 NetId
	FReplicationClient* ReplicationClient = nullptr; // 클라이언트
	FNetPlayerSpawner*  Players           = nullptr; // 멀티플레이 서버: 새 씬에서 플레이어 폰 (원격 플레이어는 이동 완료 때 앱의 입장 처리가 만든다)
	FResourceManager*   Resources         = nullptr; // 없으면 에셋 해석 생략 (GPU 없는 전용 서버)
	std::filesystem::path ContentDirectory;          // SceneAsset 기준
	std::string           PlayerPrefab;              // Players와 함께 (프로젝트 설정 "플레이어 프리팹")
	std::function<void()> OnEndPlay;                 // 이전 씬 EndPlay 직후, 씬을 비우기 전 (오디오 정리 등)
};

// 게임 중 맵 전환 (런타임, 에디터 플레이, 전용 서버, 테스트가 같은 순서를 쓴다).
//   요청: 서버/Standalone은 스크립트·게임 모듈(FGameWorld::RequestOpenScene), 클라이언트는 서버의 Travel 메시지(FNetDriver::ConsumeServerTravel).
//         앱은 프레임 끝(게임플레이·복제 틱 뒤)에 ConsumePending으로 꺼내 Travel을 부른다 — 갱신 도중 씬을 부수지 않는다
//   Travel 순서: (서버) 클라이언트에 이동 지시(BeginServerTravel, 이후 그 플레이어들은 TravelAck까지 복제/메시지에서 빠짐)
//         → World.EndPlay(스크립트 OnDestroy, 게임 모듈 OnEndPlay, 물리/AI 정리) → OnEndPlay → 복제/플레이어 정리
//         → Scene.Clear → 씬 파일 로드 → 에셋 해석 → (서버) 정적 NetId → BeginPlay → (리슨) 호스트 폰
//         / (클라이언트) 정적 NetId → BeginPlay → TravelAck. 서버는 TravelAck가 온 플레이어마다 FNetDriver::OnPlayerJoined를 다시 부른다
//   GPU 리소스: 씬 엔티티가 사라지며 핸들 참조만 끊긴다. FResourceManager 경로 캐시는 유지 (같은 에셋은 다음 맵에서 재사용, 해제는 후속)
struct FGameWorldTravel
{
	static std::optional<std::string> ConsumePending(FGameWorld& World, FNetDriver* Net);
	// 반환: 새 씬을 열었는가. 실패해도(파일 손상 등) 빈 씬으로 게임은 계속된다 (Error 로그)
	static bool Travel(const FSceneTravelTargets& Targets, const std::string& SceneAsset);
};
