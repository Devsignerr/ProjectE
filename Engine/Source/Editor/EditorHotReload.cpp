// C++ 게임 모듈 핫 리로드 (에디터, Phase 48 사이드) — FEditorApplication 중 핫 리로드 부분.
//
// 다시 로드 절차 (ReloadGameModule — 메뉴/단축키는 다음 OnUpdate로 미뤄 ImGui 패널 도중에 씬을 바꾸지 않는다):
//   1. 플레이 중이면 정지, 시퀀서 미리보기 종료
//   2. 원본 DLL의 새 그림자 복사본을 만든다 (실패하면 아무것도 바꾸지 않고 끝)
//   3. 편집 씬을 JSON으로 (프리팹 오버라이드 기록 뒤 — 모듈 컴포넌트도 리플렉션이라 포함) + 열린 에셋 편집 창 상태(JSON)
//   4. FGameModuleHost::Reload: OnUnload → 모듈 소유 리플렉션 타입 제거 + ECS 타입 ID 폐기 + 소유자별 정리(BT 노드 등)
//      → 새 복사본 로드 + OnLoad. 이전 DLL은 FreeLibrary하지 않는다 (씬 풀의 가상 함수·남은 함수 포인터 안전)
//   5. 프리팹 캐시 비우기(옛 타입으로 읽은 원본) → 씬 JSON 복원(실행 취소 복원과 같은 경로: 선택은 엔티티 경로로, 모델 템플릿,
//      에셋 해석) → 에셋 편집 창 상태 복원. 편집 카메라는 건드리지 않는다
//   실행 취소 기록은 씬 JSON 스냅샷이므로 그대로 둔다 (모듈 컴포넌트는 이름으로 저장되어 새 타입으로 다시 읽힌다).
// ECS 타입 ID를 폐기하므로 새 DLL은 같은 이름의 컴포넌트에 새 풀을 쓴다 — 구조체 멤버를 바꿔도 옛 풀을 잘못 캐스팅하지 않는다.
// 옛 풀은 각 씬 레지스트리에 남아 엔티티 파괴 때 옛 DLL 코드로 정리된다 (편집 씬은 복원 시 비워진다).

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/Settings/SettingsRegistry.h"
#include "Core/StringConv.h"
#include "Editor/EditorApplication.h"
#include "Editor/EditorPreferences.h"
#include "Editor/EditorTheme.h"
#include "Editor/SceneEditOps.h"
#include "Scene/Prefab.h"
#include "Scene/SceneSerializer.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <share.h>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	double SteadySeconds()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// 검증용 씬 요약: 엔티티 수, 모듈 소유 컴포넌트 수, 그 Float 프로퍼티 값 합
	struct FModuleSceneSummary
	{
		uint32 Entities         = 0;
		uint32 ModuleComponents = 0;
		double FloatSum         = 0.0;
	};

	FModuleSceneSummary SummarizeScene(FScene& Scene, const std::string& Owner)
	{
		FModuleSceneSummary Summary;
		FRegistry&          Registry = Scene.GetRegistry();
		Summary.Entities             = Registry.GetAliveCount();
		FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
			if (Type.Owner != Owner)
			{
				return;
			}
			Registry.View<FTransformComponent>().Each([&](FEntity Entity, FTransformComponent&) {
				void* Component = Type.GetComponent(Registry, Entity);
				if (Component == nullptr)
				{
					return;
				}
				++Summary.ModuleComponents;
				for (const FPropertyInfo& Property : Type.Properties)
				{
					if (Property.Type == EPropertyType::Float)
					{
						Summary.FloatSum += Property.GetRef<float>(Component);
					}
				}
			});
		});
		return Summary;
	}

	// 이 프로세스(또는 다른 프로세스)가 파일을 열고 있지 않은지 — 읽기/쓰기 모두 거부 공유로 열어 본다 (쓰지 않음)
	bool IsFileUnlocked(const std::filesystem::path& Path)
	{
		FILE* File = _wfsopen(Path.c_str(), L"r+b", _SH_DENYRW);
		if (File == nullptr)
		{
			return false;
		}
		std::fclose(File);
		return true;
	}

	// 모듈 컴포넌트가 붙은 첫 엔티티 (이름, 회전) — 플레이 중 새 DLL의 OnUpdate가 도는지 확인
	FEntity FindModuleEntity(FScene& Scene, const std::string& Owner, const char* TypeName)
	{
		const FTypeInfo* Type = FTypeRegistry::Get().Find(TypeName);
		FEntity          Found;
		if (Type == nullptr || Type->Owner != Owner)
		{
			return Found;
		}
		Scene.GetRegistry().View<FTransformComponent>().Each([&](FEntity Entity, FTransformComponent&) {
			if (!Found.IsValid() && Type->HasComponent(Scene.GetRegistry(), Entity))
			{
				Found = Entity;
			}
		});
		return Found;
	}
} // namespace

void FEditorApplication::InitGameModule()
{
	if (!FPaths::HasProject() || FPaths::GetProjectDescriptor().GameModule.empty())
	{
		return;
	}
	const std::string           Name   = FPaths::GetProjectDescriptor().GameModule;
	const std::filesystem::path Source = FGameModuleHost::GetDefaultModulePath(Name);
	HotReload.Init(Name, Source, FPaths::GetSavedDirectory() / L"HotReload");

	std::error_code       FsError;
	std::filesystem::path Copy;
	if (std::filesystem::exists(Source, FsError))
	{
		std::string Error;
		Copy = HotReload.MakeShadowCopy(Error);
		if (Copy.empty())
		{
			E_LOG(LogEditor, Warning, "게임 모듈 복사본을 만들지 못해 원본을 직접 로드합니다 (핫 리로드 시 원본이 잠김): {}", Error);
		}
	}
	GameModule.Load(Copy.empty() ? Source : Copy, Name);
}

bool FEditorApplication::ReloadGameModule(const std::string& Reason)
{
	if (!HotReload.IsConfigured())
	{
		ShowNotification("이 프로젝트에는 C++ 게임 모듈이 없습니다 (.eproject \"GameModule\")", true);
		return false;
	}
	const double StartTime = SteadySeconds();
	E_LOG(LogEditor, Display, "C++ 게임 모듈 다시 로드 시작 ({}): {}", Reason, HotReload.GetModuleName());
	if (PlayMode.IsActive())
	{
		E_LOG(LogEditor, Display, "플레이 중이므로 먼저 정지합니다");
		StopPlay();
	}
	AssetEditors.EndScenePreviews(Context);

	// 새 복사본을 먼저 만든다 (실패하면 지금 상태 그대로)
	std::string                 Error;
	const std::filesystem::path Copy = HotReload.MakeShadowCopy(Error);
	if (Copy.empty())
	{
		E_LOG(LogEditor, Error, "C++ 다시 로드 실패: {}", Error);
		ShowNotification("C++ 다시 로드 실패: " + Error, true);
		return false;
	}

	// 편집 상태 직렬화 (모듈 컴포넌트 포함 — 리플렉션)
	CommitPendingEdit();
	FPrefabLibrary::Get().RecordAllOverrides(Scene);
	const std::string              SceneJson    = FSceneSerializer::ToJsonString(Scene);
	const std::vector<std::string> EditorStates = AssetEditors.CaptureEditorStates();
	const uint32                   EntitiesBefore = Scene.GetRegistry().GetAliveCount();

	// 모듈 교체 (로드된 적 없으면 처음 로드)
	const bool bReloaded = GameModule.IsLoaded() ? GameModule.Reload(Copy) : GameModule.Load(Copy, HotReload.GetModuleName());
	HotReload.ResetSourceBaseline();

	// 옛 타입으로 읽은 캐시를 비우고 새 타입으로 복원
	FPrefabLibrary::Get().Invalidate();
	RestoreSnapshot(SceneJson);
	AssetEditors.RestoreEditorStates(Context, EditorStates);
	Resources.RequestGarbageCollection("게임 모듈 다시 로드");

	const double ElapsedMs = (SteadySeconds() - StartTime) * 1000.0;
	if (!bReloaded)
	{
		ShowNotification("C++ 다시 로드 실패 — 이전 모듈을 유지합니다 (로그 확인)", true);
		return false;
	}
	E_LOG(LogEditor, Display, "C++ 게임 모듈 다시 로드 완료: {} ({:.0f}ms, 엔티티 {} → {}, 실행 취소 기록 유지, 누적 DLL 버전 {}개)",
	      FStringConv::ToUtf8(Copy.filename().wstring()), ElapsedMs, EntitiesBefore, Scene.GetRegistry().GetAliveCount(), GameModule.GetReloadCount() + 1);
	ShowNotification(std::format("C++ 다시 로드됨: {} ({:.0f}ms)", HotReload.GetModuleName(), ElapsedMs), false);
	return true;
}

void FEditorApplication::StartGameModuleBuild()
{
	if (!HotReload.IsConfigured())
	{
		ShowNotification("이 프로젝트에는 C++ 게임 모듈이 없습니다", true);
		return;
	}
	std::string Error;
	if (!HotReload.GetBuild().Start(HotReload.GetModuleName(), Error))
	{
		E_LOG(LogEditor, Error, "게임 모듈 빌드를 시작하지 못했습니다: {}", Error);
		ShowNotification("게임 모듈 빌드 시작 실패: " + Error, true);
		return;
	}
	bReloadAfterBuild = true;
	LastBuildErrorLine.clear();
	E_LOG(LogEditor, Display, "게임 모듈 빌드 시작: {}", HotReload.GetModuleName());
}

void FEditorApplication::UpdateGameModuleHotReload()
{
	if (!HotReload.IsConfigured())
	{
		return;
	}

	// 빌드 출력/완료
	FGameModuleBuild&        Build = HotReload.GetBuild();
	std::vector<std::string> Lines;
	std::optional<bool>      Finished;
	Build.Poll(Lines, Finished);
	for (const std::string& Line : Lines)
	{
		const bool bError = Line.find(" error ") != std::string::npos || Line.find("error C") != std::string::npos ||
		                    Line.find("error LNK") != std::string::npos || Line.find("FAILED:") != std::string::npos;
		if (bError)
		{
			E_LOG(LogEditor, Error, "[빌드] {}", Line);
			if (LastBuildErrorLine.empty())
			{
				LastBuildErrorLine = Line; // 알림에는 첫 오류
			}
		}
		else
		{
			E_LOG(LogEditor, Log, "[빌드] {}", Line);
		}
	}
	if (Finished.has_value())
	{
		const bool bReload = bReloadAfterBuild;
		bReloadAfterBuild  = false;
		if (*Finished)
		{
			E_LOG(LogEditor, Display, "게임 모듈 빌드 성공: {}", HotReload.GetModuleName());
			if (bReload)
			{
				ReloadGameModule("빌드 완료");
			}
		}
		else
		{
			E_LOG(LogEditor, Error, "게임 모듈 빌드 실패: {}", HotReload.GetModuleName());
			ShowNotification("게임 모듈 빌드 실패" + (LastBuildErrorLine.empty() ? std::string(" (출력 로그 확인)") : ": " + LastBuildErrorLine), true);
		}
	}

	// 메뉴/단축키 요청
	if (!PendingGameModuleReload.empty())
	{
		const std::string Reason = std::move(PendingGameModuleReload);
		PendingGameModuleReload.clear();
		ReloadGameModule(Reason);
	}

	// 원본 변경 감지 (외부 빌드 — 우리 빌드 중에는 끝난 뒤 위에서 다시 로드). 자동 검증은 다른 빌드에 흔들리지 않도록 끈다
	if (!Build.IsRunning() && !IsAutomationRun() && FEditorPreferences::Get().General.bAutoReloadGameModule &&
	    HotReload.PollSourceChange(SteadySeconds()))
	{
		ReloadGameModule("DLL 변경 감지");
	}

	UpdateVerifyHotReload();
}

void FEditorApplication::DrawGameModuleMenuItems()
{
	if (!HotReload.IsConfigured())
	{
		return;
	}
	ImGui::Separator();
	const bool bBuilding = HotReload.GetBuild().IsRunning();
	if (ImGui::MenuItem(ICON_FA_ARROWS_ROTATE " C++ 다시 로드", "Ctrl+Alt+F11", false, !bBuilding))
	{
		PendingGameModuleReload = "메뉴";
	}
	ImGui::SetItemTooltip("게임 모듈 DLL(%s)의 새 복사본을 로드합니다. 플레이 중이면 정지하고, 편집 씬은 JSON으로 보존해 새 타입으로 다시 만듭니다",
	                      HotReload.GetModuleName().c_str());
	if (!bBuilding)
	{
		if (ImGui::MenuItem(ICON_FA_HAMMER " 게임 모듈 빌드 후 다시 로드"))
		{
			StartGameModuleBuild();
		}
		ImGui::SetItemTooltip("에디터가 놓인 빌드 폴더에서 cmake --build --target %s 를 백그라운드로 실행하고, 성공하면 다시 로드합니다",
		                      HotReload.GetModuleName().c_str());
	}
	else if (ImGui::MenuItem(ICON_FA_HAMMER " 게임 모듈 빌드 취소"))
	{
		HotReload.GetBuild().Cancel();
		bReloadAfterBuild = false;
	}
	bool& bAutoReload = FEditorPreferences::Get().General.bAutoReloadGameModule;
	if (ImGui::MenuItem("DLL 변경 시 자동 다시 로드", nullptr, &bAutoReload))
	{
		if (FSettingsSection* Section = FSettingsRegistry::Get().Find("EditorGeneral"); Section != nullptr && !IsAutomationRun())
		{
			Section->Save();
		}
		HotReload.ResetSourceBaseline();
	}
	ImGui::TextDisabled("게임 모듈: %s (다시 로드 %u회)", HotReload.GetModuleName().c_str(), GameModule.GetReloadCount());
}

void FEditorApplication::DrawGameModuleBuildStatus()
{
	const FGameModuleBuild& Build = HotReload.GetBuild();
	if (!Build.IsRunning())
	{
		return;
	}
	// 메인 창 오른쪽 아래, 알림 토스트 위
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float          Margin   = 16.0f * ImGuiLayer.GetDpiScale();
	ImGui::SetNextWindowPos(ImVec2(Viewport->WorkPos.x + Viewport->WorkSize.x - Margin, Viewport->WorkPos.y + Viewport->WorkSize.y - Margin * 4.0f),
	                        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	ImGui::SetNextWindowViewport(Viewport->ID);
	ImGui::SetNextWindowBgAlpha(0.85f);
	constexpr ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
	                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
	                                   ImGuiWindowFlags_NoSavedSettings;
	if (ImGui::Begin("##GameModuleBuild", nullptr, Flags))
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_HAMMER " 게임 모듈 빌드 중: %s (%.0f초)", HotReload.GetModuleName().c_str(), Build.GetElapsedSeconds());
	}
	ImGui::End();
}

void FEditorApplication::UpdateVerifyHotReload()
{
	// 자동 검증 --verify-hot-reload: 같은 DLL을 두 번 다시 로드 → 씬 엔티티 수·모듈 컴포넌트 값 보존, 원본 DLL/PDB 잠금 없음,
	// 복사본 PDB 경로 → 플레이 시작(새 DLL의 OnUpdate가 모듈 컴포넌트를 돌리는지) → 정지 후 씬 보존. 로그 PASS/FAIL
	// --verify-hot-reload-build: 게임 모듈 빌드 후 다시 로드 경로 (빌드 완료 → 다시 로드 → 종료 요청, 로그 PASS/FAIL).
	// 빌드가 몇 초 걸리므로 Verify.ps1 -Frames를 크게 준다
	static const bool bVerifyBuild = FCommandLine::FromProcess().HasFlag(L"--verify-hot-reload-build");
	if (bVerifyBuild && GetFrameIndex() >= 30)
	{
		static FModuleSceneSummary BuildBefore;
		if (VerifyHotReloadStage == 0)
		{
			BuildBefore          = SummarizeScene(Scene, HotReload.GetModuleName());
			VerifyHotReloadFrame = GameModule.GetReloadCount();
			StartGameModuleBuild();
			VerifyHotReloadStage = HotReload.GetBuild().IsRunning() ? 1 : 2;
		}
		else if (VerifyHotReloadStage == 1 && !HotReload.GetBuild().IsRunning() && !bReloadAfterBuild)
		{
			const FModuleSceneSummary After = SummarizeScene(Scene, HotReload.GetModuleName());
			const bool bSceneKept = After.Entities == BuildBefore.Entities && After.ModuleComponents == BuildBefore.ModuleComponents &&
			                        std::abs(After.FloatSum - BuildBefore.FloatSum) < 1.0e-3;
			if (GameModule.GetReloadCount() == VerifyHotReloadFrame + 1 && bSceneKept)
			{
				E_LOG(LogEditor, Display, "핫 리로드 빌드 검증: 빌드 → 다시 로드 ({}회), 모듈 컴포넌트 {}개 보존 — 플레이 확인", GameModule.GetReloadCount(),
				      After.ModuleComponents);
				StartPlay(); // 새 DLL 코드로 새 풀의 컴포넌트를 돌린다 (구조체 배치가 바뀌었어도)
				VerifyHotReloadStage = 4;
				VerifyHotReloadFrame = GetFrameIndex();
			}
			else
			{
				E_LOG(LogEditor, Error, "핫 리로드 빌드 검증 FAIL: 다시 로드 {}회 → {}회, 모듈 컴포넌트 {} → {}", VerifyHotReloadFrame, GameModule.GetReloadCount(),
				      BuildBefore.ModuleComponents, After.ModuleComponents);
				VerifyHotReloadStage = 3;
				RequestExit();
			}
		}
		else if (VerifyHotReloadStage == 4 && GetFrameIndex() >= VerifyHotReloadFrame + 30)
		{
			const bool bPlaying = PlayMode.IsActive();
			StopPlay();
			if (bPlaying)
			{
				E_LOG(LogEditor, Display, "핫 리로드 빌드 검증 PASS: 빌드 → 다시 로드 → 플레이 30프레임 → 정지");
			}
			else
			{
				E_LOG(LogEditor, Error, "핫 리로드 빌드 검증 FAIL: 플레이가 시작되지 않았습니다");
			}
			VerifyHotReloadStage = 3;
			RequestExit();
		}
		else if (VerifyHotReloadStage == 2)
		{
			E_LOG(LogEditor, Error, "핫 리로드 빌드 검증 FAIL: 빌드를 시작하지 못했습니다");
			VerifyHotReloadStage = 3;
			RequestExit();
		}
		return;
	}

	static const bool bVerify = FCommandLine::FromProcess().HasFlag(L"--verify-hot-reload");
	if (!bVerify || VerifyHotReloadStage >= 3 || GetFrameIndex() < 30)
	{
		return;
	}

	static FModuleSceneSummary Before;
	static std::vector<std::string> Failures;
	static FEntityPath             ProbePath;
	static FQuat                   ProbeRotation;
	const std::string&             Owner = HotReload.GetModuleName();
	auto Check = [](bool bOk, const std::string& What) {
		if (!bOk)
		{
			Failures.push_back(What);
		}
		E_LOG(LogEditor, Display, "핫 리로드 검증 {}: {}", bOk ? "통과" : "실패", What);
	};
	auto CheckSummary = [&](const char* Label) {
		const FModuleSceneSummary After = SummarizeScene(Scene, Owner);
		Check(After.Entities == Before.Entities && After.ModuleComponents == Before.ModuleComponents && std::abs(After.FloatSum - Before.FloatSum) < 1.0e-3,
		      std::format("{}: 엔티티 {}→{}, 모듈 컴포넌트 {}→{}, 값 합 {:.3f}→{:.3f}", Label, Before.Entities, After.Entities, Before.ModuleComponents,
		                  After.ModuleComponents, Before.FloatSum, After.FloatSum));
	};

	if (VerifyHotReloadStage == 0)
	{
		Before = SummarizeScene(Scene, Owner);
		Check(GameModule.IsLoaded() && Before.ModuleComponents > 0,
		      std::format("시작 상태: 모듈 {} 로드, 모듈 컴포넌트 {}개 (--scene Scenes/Tests/GameModule.escene 권장)", Owner, Before.ModuleComponents));
		for (uint32 Round = 1; Round <= 2; ++Round)
		{
			Check(ReloadGameModule(std::format("자동 검증 {}", Round)), std::format("다시 로드 {}회차", Round));
			CheckSummary(std::format("다시 로드 {}회차 뒤", Round).c_str());
			const FTypeInfo* Spinner = FTypeRegistry::Get().Find("SpinnerComponent");
			Check(Spinner != nullptr && Spinner->Owner == Owner, "모듈 타입 다시 등록 (소유자 = 모듈 이름)");
		}
		Check(GameModule.GetReloadCount() == 2, std::format("다시 로드 횟수 {}", GameModule.GetReloadCount()));

		// 원본 DLL/PDB가 잠기지 않았는지 + 복사본 PDB 경로
		const std::filesystem::path Source    = HotReload.GetSourceDll();
		const std::filesystem::path SourcePdb = std::filesystem::path(Source).replace_extension(L".pdb");
		Check(IsFileUnlocked(Source), "원본 DLL 잠금 없음: " + FStringConv::ToUtf8(Source.filename().wstring()));
		std::error_code FsError;
		if (std::filesystem::exists(SourcePdb, FsError))
		{
			Check(IsFileUnlocked(SourcePdb), "원본 PDB 잠금 없음: " + FStringConv::ToUtf8(SourcePdb.filename().wstring()));
		}
		const std::filesystem::path Loaded = GameModule.GetLoadedPath();
		std::ifstream               Stream(Loaded, std::ios::binary);
		const std::vector<uint8>    Image{ std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>() };
		std::string                 PdbPath;
		const std::string           Expected = FStringConv::ToUtf8(std::filesystem::path(Loaded).replace_extension(L".pdb").filename().wstring());
		const bool                  bPdbRead = GameModuleHotReload::ReadPdbPath(Image, PdbPath);
		Check(bPdbRead && PdbPath == Expected, std::format("복사본 PDB 경로 '{}' (기대 '{}')", PdbPath, Expected));

		// 플레이: 새 DLL의 OnUpdate가 돌면 회전 컴포넌트가 붙은 엔티티가 돈다
		StartPlay();
		const FEntity Probe = FindModuleEntity(*Context.Scene, Owner, "SpinnerComponent");
		Check(Probe.IsValid() && PlayMode.IsActive(), "플레이 시작 + 회전 엔티티");
		if (Probe.IsValid())
		{
			ProbePath     = FEntityPath::Build(*Context.Scene, Probe);
			ProbeRotation = Context.Scene->GetTransform(Probe).Rotation;
		}
		VerifyHotReloadStage = 1;
		VerifyHotReloadFrame = GetFrameIndex();
		return;
	}

	if (VerifyHotReloadStage == 1 && GetFrameIndex() >= VerifyHotReloadFrame + 30)
	{
		const FEntity Probe = ProbePath.Resolve(*Context.Scene);
		if (Probe.IsValid())
		{
			const FQuat Now = Context.Scene->GetTransform(Probe).Rotation;
			const float Dot = std::abs(Now.X * ProbeRotation.X + Now.Y * ProbeRotation.Y + Now.Z * ProbeRotation.Z + Now.W * ProbeRotation.W);
			Check(Dot < 0.99999f, std::format("플레이 중 새 DLL OnUpdate 실행 (회전 변화 cos {:.5f})", Dot));
		}
		StopPlay();
		Check(!PlayMode.IsActive(), "플레이 정지");
		CheckSummary("플레이 정지 뒤");
		// 플레이 뒤 한 번 더 (플레이 씬 레지스트리에 옛 풀이 남은 상태에서 다음 플레이)
		Check(ReloadGameModule("자동 검증 3"), "플레이 뒤 다시 로드");
		CheckSummary("플레이 뒤 다시 로드");
		StartPlay();
		VerifyHotReloadStage = 2;
		VerifyHotReloadFrame = GetFrameIndex();
		return;
	}

	if (VerifyHotReloadStage == 2 && GetFrameIndex() >= VerifyHotReloadFrame + 10)
	{
		Check(PlayMode.IsActive(), "다시 플레이");
		StopPlay();
		CheckSummary("두 번째 플레이 정지 뒤");
		if (Failures.empty())
		{
			E_LOG(LogEditor, Display, "핫 리로드 검증 PASS (다시 로드 {}회)", GameModule.GetReloadCount());
		}
		else
		{
			E_LOG(LogEditor, Error, "핫 리로드 검증 FAIL: {}개 실패 — 첫 실패: {}", Failures.size(), Failures.front());
		}
		VerifyHotReloadStage = 3;
	}
}
