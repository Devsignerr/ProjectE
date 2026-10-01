// Lua 컷신 시퀀스 바인딩 (Scene/SequencePlayer.h)
//   entity:PlaySequence([asset], [startTime]) → 시작했는지. asset을 주면 그 시퀀스로 바꾼다 (SequencePlayerComponent가 없으면 붙인다).
//     startTime 생략 = 처음부터, 음수 = 멈춘 자리에서 이어서
//   entity:StopSequence() (처음으로 + 카메라 컷 해제), entity:PauseSequence(), entity:IsSequencePlaying(),
//   entity:GetSequenceTime(), entity:SetSequenceTime(t), entity:GetSequenceDuration()
//   이벤트: 시퀀스 이벤트 트랙 → 재생 엔티티(없으면 가장 가까운 조상)의 스크립트 OnSequenceEvent_<이름>(), 끝나면 OnSequenceFinished()
//   멀티플레이: 로컬 연출 — 모든 프로세스에서 맞추려면 서버가 Multicast RPC로 PlaySequence를 부른다
#include "Scene/Scene.h"
#include "Scene/SequencePlayer.h"
#include "Scripting/LuaRuntime.h"

#include <stdexcept>

void FLuaRuntime::RegisterSequenceBindings()
{
	const auto Require = [this](const FScriptEntity& Entity) -> FScene& {
		if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity.Entity))
		{
			throw std::runtime_error("유효하지 않은 엔티티입니다");
		}
		return *Scene;
	};

	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["PlaySequence"]              = [Require](const FScriptEntity& Entity, sol::optional<std::string> Asset, sol::optional<float> StartTime) {
        FScene& Target = Require(Entity);
        return FSequenceSystem::Play(Target, Entity.Entity, Asset.value_or(std::string()), StartTime.value_or(0.0f));
	};
	EntityType["StopSequence"]        = [Require](const FScriptEntity& Entity) { FSequenceSystem::Stop(Require(Entity), Entity.Entity); };
	EntityType["PauseSequence"]       = [Require](const FScriptEntity& Entity) { FSequenceSystem::Pause(Require(Entity), Entity.Entity); };
	EntityType["IsSequencePlaying"]   = [Require](const FScriptEntity& Entity) { return FSequenceSystem::IsPlaying(Require(Entity), Entity.Entity); };
	EntityType["GetSequenceTime"]     = [Require](const FScriptEntity& Entity) { return FSequenceSystem::GetTime(Require(Entity), Entity.Entity); };
	EntityType["SetSequenceTime"]     = [Require](const FScriptEntity& Entity, float Seconds) { FSequenceSystem::SetTime(Require(Entity), Entity.Entity, Seconds); };
	EntityType["GetSequenceDuration"] = [Require](const FScriptEntity& Entity) { return FSequenceSystem::GetDuration(Require(Entity), Entity.Entity); };
}

void FLuaRuntime::DispatchSequenceEvents()
{
	FRegistry& Registry = Scene->GetRegistry();
	struct FPending
	{
		FEntity     Entity;
		std::string Method;
	};
	std::vector<FPending> Calls;
	Registry.View<FSequencePlayerComponent>().Each([&](FEntity Entity, FSequencePlayerComponent& Component) {
		for (const std::string& Event : Component.Runtime.Events)
		{
			Calls.push_back({ Entity, "OnSequenceEvent_" + Event });
		}
		if (Component.Runtime.bFinishedThisUpdate)
		{
			Calls.push_back({ Entity, "OnSequenceFinished" });
		}
	});
	for (const FPending& Call : Calls)
	{
		// 받는 쪽: 재생 엔티티의 스크립트, 없으면 가장 가까운 조상의 스크립트
		for (FEntity Current = Call.Entity; Current.IsValid() && Registry.IsValid(Current); Current = Scene->GetParent(Current))
		{
			const auto Found = Instances.find(Current.ToId());
			if (Found != Instances.end())
			{
				if (!Found->second.bFaulted && Found->second.bStarted)
				{
					CallMethod(Found->second, Call.Method.c_str());
				}
				break;
			}
		}
	}
}
