#pragma once

#include "Core/CoreTypes.h"
#include "Network/LanDiscovery.h"
#include "Network/NetDriver.h"
#include "Network/NetPlayerSpawner.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"

#include <filesystem>
#include <string>
#include <vector>

class FGameWorld;
class FResourceManager;
class FScene;
struct FPlayOptions;

// 에디터 네트워크 플레이 설정 (네트워크 패널에서 고친다)
struct FPlayNetSettings
{
	enum class EMode : int32
	{
		Standalone      = 0, // 1인용 (기본)
		ListenServer    = 1, // 에디터가 호스트 + 런타임 클라이언트 N개
		DedicatedServer = 2, // 전용 서버 프로세스 + 에디터도 클라이언트 + 런타임 클라이언트 N개
	};
	EMode  Mode        = EMode::Standalone;
	int32  ClientCount = 1; // 추가로 띄울 런타임 클라이언트 창 수
	uint16 Port        = DefaultNetPort;
	int32  LatencyMs   = 0;    // 디버그 지연 (에디터와 띄운 프로세스 모두, 보내는 쪽)
	float  LossPercent = 0.0f; // 디버그 손실
};

// 에디터 플레이의 네트워크 부분: 드라이버/복제/플레이어/LAN + 자식 프로세스(전용 서버, 런타임 클라이언트).
//   씬: 편집 씬을 Saved/PlayInEditor/PIE.escene에 저장하고, 에디터와 자식 프로세스가 모두 이 파일(절대 경로)을
//       같은 방식으로 로드한다 → 정적 NetId가 같다 (에디터 플레이 씬도 복제가 아니라 같은 JSON에서 만든다)
//   정지: 네트워크 종료 + 자식 프로세스 종료
class FPlayInEditorNet
{
public:
	~FPlayInEditorNet();

	FPlayNetSettings PendingSettings; // 다음 플레이에 쓸 설정 (네트워크 패널/명령줄이 고친다)

	// World/Resources는 이 객체보다 오래 산다 (비소유)
	void Init(FGameWorld& InWorld, FResourceManager* InResources, std::filesystem::path InContentDirectory);

	// 플레이 시작 전: PIE 씬 저장, 전용 서버 실행, PlayMode 옵션 채우기. Standalone이면 false (1인용 플레이)
	bool Prepare(const FPlayNetSettings& Settings, FScene& EditScene, FPlayOptions& OutOptions);
	// PlayMode.Play 직후: 리슨 호스트 폰/LAN 알림, 런타임 클라이언트 실행
	void AfterPlay();
	void Stop();
	bool IsActive() const { return Mode != ENetMode::Standalone; }

	// 매 프레임: Update는 PlayMode.Tick 전(수신·클라이언트 보간), PostTick은 후(서버 복제 전송)
	void Update(float DeltaSeconds);
	void PostTick(float DeltaSeconds);

	// 패널
	ENetMode                 GetMode() const { return Mode; }
	const FNetDriver&        GetDriver() const { return Net; }
	int32                    GetChildProcessCount() const;
	FLanDiscovery&           GetLanSearch() { return LanSearch; }
	// 디버그 지연/손실을 지금 에디터 연결에 적용 (패널에서 바꿀 때). 띄울 프로세스에는 명령줄로 넘긴다
	void                     ApplySimulation();
	// 런타임 클라이언트 창 하나 실행 (Address 서버, SceneAsset = 서버가 연 씬)
	bool                     LaunchRuntimeClient(const std::string& Address, const std::string& SceneAsset);
	const std::string&       GetSceneAsset() const { return SceneAsset; }

private:
	bool LaunchProcess(const std::wstring& Executable, const std::wstring& Arguments);
	void TerminateChildren();

	FGameWorld*           World     = nullptr;
	FResourceManager*     Resources = nullptr;
	std::filesystem::path ContentDirectory;

	FPlayNetSettings   Settings;
	ENetMode           Mode = ENetMode::Standalone;
	std::string        SceneJson;  // PIE 씬 (PlayMode가 이 JSON으로 플레이 씬을 만든다)
	std::string        SceneAsset; // PIE 씬 절대 경로 (UTF-8) — 핸드셰이크 씬 비교에 그대로 쓰인다
	FScene*            PlayScene = nullptr;
	FNetDriver         Net;
	FReplicationServer ReplicationServer;
	FReplicationClient ReplicationClient;
	FNetPlayerSpawner  Players;
	FLanDiscovery      LanHost;
	FLanDiscovery      LanSearch;
	std::vector<void*> ChildProcesses; // HANDLE (공개 헤더에 Windows.h 금지)
	int32              LaunchedClientCount = 0; // 클라이언트 로그 파일 번호
};
