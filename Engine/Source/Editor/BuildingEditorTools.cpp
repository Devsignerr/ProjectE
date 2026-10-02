#include "Editor/BuildingEditorTools.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/Building/BuildingConfig.h"
#include "Scene/Building/BuildingScene.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <chrono>
#include <format>
#include <string>
#include <vector>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	bool        GBakeNavMeshAfterGenerate = false;
	std::string GLastSummary;

	int32 CountGroups(const FScene& Scene, FEntity Building)
	{
		int32 Count = 0;
		for (const FEntity Child : Scene.GetChildren(Building))
		{
			const FBuildingPartComponent* Part = Scene.GetRegistry().TryGet<FBuildingPartComponent>(Child);
			Count += (Part != nullptr && Part->Floor >= 0) ? 1 : 0;
		}
		return Count;
	}
} // namespace

void FBuildingEditorTools::DrawInspector(FEditorContext& Context, FEntity Entity)
{
	FScene& Scene = *Context.Scene;
	if (!Scene.GetRegistry().Has<FProceduralBuildingComponent>(Entity))
	{
		return;
	}
	ImGui::SeparatorText("생성");
	ImGui::BeginDisabled(Context.bPlaying);
	const bool bHasGenerated = CountGroups(Scene, Entity) > 0;
	if (ImGui::Button(bHasGenerated ? ICON_FA_HAMMER " 다시 생성" : ICON_FA_HAMMER " 생성"))
	{
		Generate(Context, Entity);
	}
	ImGui::SetItemTooltip("하위에 층/호실 그룹과 프리팹 인스턴스를 만든다 (실행 취소 한 단계).\n다시 생성하면 그룹 아래는 교체되고, '건물 생성물 (유지)' 컴포넌트를 붙인 엔티티만 남는다.\n층/호실 그룹에서 유지를 켜면 그 자리는 고정된다");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_DICE " 새 시드"))
	{
		const uint64 Ticks = static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count());
		Generate(Context, Entity, static_cast<int32>((Ticks ^ (Ticks >> 29)) % 100000u) + 1);
	}
	ImGui::SetItemTooltip("무작위 시드로 바꾸고 다시 생성");
	ImGui::SameLine();
	ImGui::BeginDisabled(!bHasGenerated);
	if (ImGui::Button(ICON_FA_ERASER " 지우기"))
	{
		Clear(Context, Entity);
	}
	ImGui::EndDisabled();
	ImGui::Checkbox("생성 후 내비메시 굽기", &GBakeNavMeshAfterGenerate);
	ImGui::EndDisabled();
	if (!GLastSummary.empty())
	{
		ImGui::TextDisabled("%s", GLastSummary.c_str());
	}
}

bool FBuildingEditorTools::Generate(FEditorContext& Context, FEntity Entity, std::optional<int32> SeedOverride)
{
	FScene& Scene = *Context.Scene;
	if (!Scene.GetRegistry().Has<FProceduralBuildingComponent>(Entity))
	{
		return false;
	}
	if (SeedOverride.has_value())
	{
		Scene.GetRegistry().Get<FProceduralBuildingComponent>(Entity).Seed = *SeedOverride;
	}
	// 편집기에서는 매번 파일을 다시 읽는다 (설정/소품 테이블을 고친 뒤 바로 반영)
	FBuildingLibrary::Get().Invalidate();
	FBuildingApplyStats Stats;
	std::string         Error;
	if (!FBuildingSceneBuilder::Generate(Scene, Entity, &Stats, &Error))
	{
		if (Context.Notify)
		{
			Context.Notify("건물 생성 실패: " + Error, true);
		}
		return false;
	}
	const auto Start = std::chrono::steady_clock::now();
	if (Context.Resources != nullptr)
	{
		FSceneAssetResolver::Resolve(Scene, *Context.Resources, Context.ContentDirectory);
	}
	const double ResolveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
	Context.DeselectIf([&Scene](FEntity Selected) { return !Scene.GetRegistry().IsValid(Selected); });
	Context.MarkEdited("건물 생성");
	GLastSummary = std::format("시드 {}: 인스턴스 {}개, 생성 {:.0f}ms + 배치 {:.0f}ms + 에셋 {:.0f}ms{}{}", Scene.GetRegistry().Get<FProceduralBuildingComponent>(Entity).Seed,
	                           Stats.Instances, Stats.GenerateMs, Stats.ApplyMs, ResolveMs, Stats.Kept > 0 ? std::format(", 유지 {}", Stats.Kept) : std::string(),
	                           Stats.Warnings.empty() ? std::string() : std::format(", 경고 {}건 (출력 로그)", Stats.Warnings.size()));
	E_LOG(LogEditor, Display, "[건물] {}", GLastSummary);
	if (Context.Notify)
	{
		Context.Notify("건물 생성: " + GLastSummary, !Stats.Warnings.empty() || Stats.Failed > 0);
	}
	if (GBakeNavMeshAfterGenerate && Context.BakeNavMeshRequest)
	{
		Context.BakeNavMeshRequest();
	}
	return true;
}

void FBuildingEditorTools::Clear(FEditorContext& Context, FEntity Entity)
{
	FScene& Scene = *Context.Scene;
	if (FBuildingSceneBuilder::Clear(Scene, Entity) > 0)
	{
		Context.DeselectIf([&Scene](FEntity Selected) { return !Scene.GetRegistry().IsValid(Selected); });
		Context.MarkEdited("건물 생성물 지우기");
	}
}

int32 FBuildingEditorTools::RunCommandLine(FEditorContext& Context)
{
	const FCommandLine CommandLine = FCommandLine::FromProcess();
	const std::wstring Names       = CommandLine.GetValue(L"--generate-building");
	if (Names.empty())
	{
		return 0;
	}
	const std::string    Targets = "," + FStringConv::ToUtf8(Names) + ",";
	std::vector<FEntity> Buildings;
	FScene&              Scene = *Context.Scene;
	Scene.GetRegistry().View<FProceduralBuildingComponent>().Each([&](FEntity Entity, FProceduralBuildingComponent&) {
		const FNameComponent* Name = Scene.GetRegistry().TryGet<FNameComponent>(Entity);
		if (Name != nullptr && Targets.find("," + Name->Name + ",") != std::string::npos)
		{
			Buildings.push_back(Entity);
		}
	});
	std::optional<int32> Seed;
	if (const std::wstring SeedArg = CommandLine.GetValue(L"--building-seed"); !SeedArg.empty())
	{
		Seed = std::stoi(SeedArg);
	}
	const std::wstring SectionArg = CommandLine.GetValue(L"--building-section");
	GBakeNavMeshAfterGenerate     = CommandLine.HasFlag(L"--building-bake-navmesh");
	int32 Generated               = 0;
	for (const FEntity Building : Buildings)
	{
		if (!SectionArg.empty())
		{
			Scene.GetRegistry().Get<FProceduralBuildingComponent>(Building).SectionFloor = std::stoi(SectionArg);
		}
		Generated += Generate(Context, Building, Seed) ? 1 : 0;
	}
	if (Buildings.empty())
	{
		E_LOG(LogEditor, Error, "자동 검증: --generate-building 대상 건물이 없습니다 ({})", FStringConv::ToUtf8(Names));
	}
	return Generated;
}
