// Lua 절차적 건물 바인딩 (Scene/Building) — 런타임 생성 경로 (에디터 "생성"과 같은 코어 FBuildingSceneBuilder)
//   entity:GenerateBuilding([seed]) → 만든 프리팹 인스턴스 수. ProceduralBuildingComponent가 있는 엔티티에서 부른다.
//     seed를 주면 컴포넌트 시드를 바꾼 뒤 생성. 이전 생성물(층/호실 그룹)은 즉시 교체되고 BuildingPartComponent(유지) 엔티티와 고정 그룹만 남는다.
//     만든 엔티티의 메시/모델은 이번 프레임 스크립트 갱신 뒤 앱이 해석한다 (Scene.SpawnPrefab과 같음). 실패하면 Lua 오류
//   entity:ClearBuilding() → 지운 그룹 수, entity:GetBuildingSeed()
// 주의: 생성물 안에 스크립트를 두지 않는다 (다시 생성하면 즉시 파괴된다).
// 멀티플레이: 생성물은 복제하지 않는다. 같은 설정 + 같은 시드면 결과가 같으므로(결정적) 서버·클라이언트가 각자 생성한다 —
//   시드 동기화(예: 서버가 Multicast RPC로 시드를 보내 각자 GenerateBuilding)는 게임 코드 몫 (엔진 자동 동기화는 후속)
#include "Scene/Building/BuildingScene.h"
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#include <format>
#include <stdexcept>

void FLuaRuntime::RegisterBuildingBindings()
{
	const auto Require = [this](const FScriptEntity& Entity) -> FScene& {
		if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity.Entity))
		{
			throw std::runtime_error("유효하지 않은 엔티티입니다");
		}
		if (!Scene->GetRegistry().Has<FProceduralBuildingComponent>(Entity.Entity))
		{
			throw std::runtime_error("ProceduralBuildingComponent가 없는 엔티티입니다");
		}
		return *Scene;
	};

	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["GenerateBuilding"]          = [this, Require](const FScriptEntity& Entity, sol::optional<int32> Seed) {
		FScene& Target = Require(Entity);
		if (Seed.has_value())
		{
			Target.GetRegistry().Get<FProceduralBuildingComponent>(Entity.Entity).Seed = *Seed;
		}
		FBuildingApplyStats Stats;
		std::string         Error;
		if (!FBuildingSceneBuilder::Generate(Target, Entity.Entity, &Stats, &Error))
		{
			throw std::runtime_error(std::format("GenerateBuilding 실패: {}", Error));
		}
		bStructureChanged = true; // 앱이 메시/머티리얼/모델 참조를 해석한다
		return Stats.Instances;
	};
	EntityType["ClearBuilding"] = [this, Require](const FScriptEntity& Entity) {
		const int32 Removed = FBuildingSceneBuilder::Clear(Require(Entity), Entity.Entity);
		bStructureChanged   = bStructureChanged || Removed > 0;
		return Removed;
	};
	EntityType["GetBuildingSeed"] = [Require](const FScriptEntity& Entity) {
		return Require(Entity).GetRegistry().Get<FProceduralBuildingComponent>(Entity.Entity).Seed;
	};
}
