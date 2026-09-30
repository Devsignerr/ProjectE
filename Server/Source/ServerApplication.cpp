#include "ServerApplication.h"

#include "Audio/AudioReflection.h"
#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "Network/NetTransport.h"
#include "Physics/PhysicsReflection.h"
#include "Scene/SceneSerializer.h"
#include "UI/UIReflection.h"

E_DEFINE_LOG_CATEGORY(LogServer, Log)

namespace
{
	FApplicationDesc MakeServerDesc()
	{
		FApplicationDesc Desc;
		Desc.bHeadless           = true;
		Desc.HeadlessTickSeconds = 1.0f / 60.0f; // 서버 시뮬레이션 60Hz
		return Desc;
	}
} // namespace

FServerApplication::FServerApplication()
	: FApplication(MakeServerDesc())
{
}

bool FServerApplication::OnInit()
{
	if (!FPaths::HasProject())
	{
		E_LOG(LogServer, Error, "프로젝트가 없습니다 (--project <경로>)");
		return false;
	}
	E_LOG(LogServer, Display, "전용 서버 — 프로젝트: {}", FPaths::GetProjectName());

	// 씬 로드 전에 타입 등록 (오디오/UI는 컴포넌트 타입만 — 서버는 소리를 내거나 UI를 그리지 않는다)
	RegisterAudioTypes();
	RegisterPhysicsTypes();
	RegisterUITypes();
	if (!FPaths::GetProjectDescriptor().GameModule.empty())
	{
		GameModule.Load(FGameModuleHost::GetDefaultModulePath(FPaths::GetProjectDescriptor().GameModule));
	}

	std::string SceneAsset = FPaths::GetProjectDescriptor().DefaultScene;
	if (const std::wstring SceneArg = FCommandLine::FromProcess().GetValue(L"--scene"); !SceneArg.empty())
	{
		SceneAsset = FStringConv::ToUtf8(SceneArg);
	}
	if (SceneAsset.empty() || !FSceneSerializer::LoadFromFile(Scene, FPaths::GetProjectContentDirectory() / FStringConv::ToWide(SceneAsset)))
	{
		E_LOG(LogServer, Error, "씬을 열지 못했습니다: {}", SceneAsset);
		return false;
	}
	Scene.UpdateTransforms();
	E_LOG(LogServer, Display, "씬 로드: {} (엔티티 {}개)", SceneAsset, Scene.GetRegistry().GetAliveCount());

	// GPU 리소스 없음 → Resources = nullptr (에셋 해석 생략). 복제할 값에 렌더 보간이 섞이지 않도록 물리 보간을 끈다
	World.Init({ &Scripts, &Physics, &GameModule, nullptr, FPaths::GetProjectContentDirectory() });
	Physics.SetInterpolation(false);
	World.BeginPlay(Scene);

	const FNetLaunchOptions NetOptions = FNetLaunchOptions::FromCommandLine(FCommandLine::FromProcess());
	if (!Net.StartServer(CreateGnsTransport(), NetOptions.Port, FNetSessionInfo::FromProject(SceneAsset), true))
	{
		E_LOG(LogServer, Error, "포트 {}에서 서버를 열지 못했습니다", NetOptions.Port);
		return false;
	}
	E_LOG(LogServer, Display, "서버 시작 (Ctrl+C 종료)");
	return true;
}

void FServerApplication::OnUpdate(float DeltaSeconds)
{
	Net.Update(DeltaSeconds);
	World.TickGameplay(DeltaSeconds, nullptr);
	World.TickPresentation(Scene, DeltaSeconds); // 애니메이션(노티파이/소켓)은 게임 로직에 쓰이므로 서버도 돌린다
}

void FServerApplication::OnShutdown()
{
	Net.Shutdown();
	World.EndPlay();
	E_LOG(LogServer, Display, "서버 종료 (틱 {}회)", GetFrameIndex());
	GameModule.Unload();
}
