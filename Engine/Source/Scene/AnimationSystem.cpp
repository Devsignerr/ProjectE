#include "Scene/AnimationSystem.h"

#include "Core/Log.h"
#include "Scene/AnimGraph.h"
#include "Scene/Components.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"

E_DEFINE_LOG_CATEGORY(LogAnimation, Log)

namespace
{
	const FAnimationChannel* FindTranslationChannel(const FAnimationClip& Clip, int32 Node)
	{
		for (const FAnimationChannel& Channel : Clip.Channels)
		{
			if (Channel.Node == Node && Channel.Path == EAnimationPath::Translation)
			{
				return &Channel;
			}
		}
		return nullptr;
	}

	// Clip 문자열 변경을 감지해 현재/이전 클립과 크로스페이드를 설정
	void ResolveClipChange(FAnimationComponent& Animation)
	{
		FAnimationRuntime&   Runtime = Animation.Runtime;
		const FAnimationSet& Set     = *Runtime.Set;
		if (Runtime.CurrentClip >= 0 && Animation.Clip == Runtime.ActiveClipName)
		{
			return;
		}

		int32 Desired = Animation.Clip.empty() ? 0 : Set.FindClip(Animation.Clip);
		if (Desired < 0)
		{
			if (!Runtime.bWarnedUnknownClip)
			{
				E_LOG(LogAnimation, Warning, "알 수 없는 애니메이션 클립 '{}' — 현재 클립을 유지합니다", Animation.Clip);
				Runtime.bWarnedUnknownClip = true;
			}
			Desired = Runtime.CurrentClip >= 0 ? Runtime.CurrentClip : 0;
		}
		else
		{
			Runtime.bWarnedUnknownClip = false;
		}
		Runtime.ActiveClipName = Animation.Clip;

		if (Runtime.CurrentClip < 0)
		{
			Runtime.CurrentClip = Desired;
			Runtime.CurrentTime = 0.0f;
		}
		else if (Desired != Runtime.CurrentClip)
		{
			Runtime.PreviousClip  = Runtime.CurrentClip;
			Runtime.PreviousTime  = Runtime.CurrentTime;
			Runtime.CurrentClip   = Desired;
			Runtime.CurrentTime   = 0.0f;
			Runtime.BlendElapsed  = 0.0f;
			Runtime.BlendDuration = Runtime.PendingBlendTime >= 0.0f ? Runtime.PendingBlendTime : Animation.BlendTime;
			if (Runtime.BlendDuration <= 0.0f)
			{
				Runtime.PreviousClip = -1;
			}
		}
		Runtime.PendingBlendTime = -1.0f;
	}


	// 판정 결과를 이벤트로 옮긴다
	void EmitNotifies(FAnimationRuntime& Runtime, FEntity Entity, const std::vector<FAnimNotify>& Notifies, const std::string& Clip, float DeltaSeconds)
	{
		for (const FAnimNotifyHit& Hit : Runtime.HitScratch)
		{
			if (Hit.Index >= 0 && Hit.Index < static_cast<int32>(Notifies.size()))
			{
				Runtime.PendingNotifies.push_back({ Entity, Notifies[Hit.Index].Name, Clip, Hit.Type,
				                                    Hit.Type == EAnimNotifyEventType::StateTick ? DeltaSeconds : 0.0f });
			}
		}
		Runtime.HitScratch.clear();
	}

	const std::vector<FAnimNotify>* FindClipNotifies(const FAnimationRuntime& Runtime, int32 Clip)
	{
		if (!Runtime.Metadata || Clip < 0 || Clip >= static_cast<int32>(Runtime.Set->Clips.size()))
		{
			return nullptr;
		}
		return Runtime.Metadata->FindNotifies(Runtime.Set->Clips[Clip].Name);
	}

	// 클립이 바뀌었으면 이전 클립의 진행 중 스테이트를 끝낸다 (사라지는 클립의 노티파이는 발생시키지 않는다)
	void EndStatesIfClipChanged(FAnimationRuntime& Runtime, FEntity Entity, bool bForceEnd)
	{
		if (Runtime.NotifyClip == Runtime.CurrentClip && !bForceEnd)
		{
			return;
		}
		if (const std::vector<FAnimNotify>* Old = FindClipNotifies(Runtime, Runtime.NotifyClip))
		{
			Runtime.ActiveStates.resize(Old->size(), 0);
			AnimNotifyMath::EndAll(Runtime.ActiveStates, Runtime.HitScratch);
			EmitNotifies(Runtime, Entity, *Old, Runtime.Set->Clips[Runtime.NotifyClip].Name, 0.0f);
		}
		Runtime.ActiveStates.clear();
		Runtime.NotifyClip = Runtime.CurrentClip;
	}

	// 애니메이션 대상 노드 엔티티에 포즈 기록
	void WritePose(FScene& Scene, const FAnimationRuntime& Runtime, const std::vector<FNodePose>& Pose)
	{
		const FAnimationSet& Set       = *Runtime.Set;
		const size_t         NodeCount = FMath::Min(Runtime.NodeEntities.size(), Pose.size());
		const FRegistry&     Registry  = Scene.GetRegistry();
		for (size_t Node = 0; Node < NodeCount; ++Node)
		{
			const FEntity NodeEntity = Runtime.NodeEntities[Node];
			if (!Set.AnimatedNodes[Node] || !Registry.IsValid(NodeEntity))
			{
				continue;
			}
			const FNodePose&     NodePose  = Pose[Node];
			FTransformComponent& Transform = Scene.GetTransform(NodeEntity);
			Transform.Position = NodePose.Translation;
			Transform.Rotation = NodePose.Rotation;
			Transform.Scale    = NodePose.Scale;
		}
	}

	// 그래프 에셋 해석 + 모델 클립에 묶기. 그래프로 재생할 수 있으면 true
	bool ResolveGraph(FAnimGraphComponent& Graph, const FAnimationSet& Set)
	{
		FAnimGraphRuntime& Runtime    = Graph.Runtime;
		const uint32       Generation = FAnimGraphLibrary::Get().GetGeneration();
		if (!Runtime.bResolved || Runtime.ResolvedGraph != Graph.Graph || Runtime.ResolvedGeneration != Generation)
		{
			// 같은 경로를 다시 읽는 것(핫 리로드)이면 파일이 바뀌었을 때만 새 에셋 — 파라미터 유지, 같은 이름 상태에서 이어 간다
			const bool bReload         = Runtime.bResolved && Runtime.ResolvedGraph == Graph.Graph;
			Runtime.bResolved          = true;
			Runtime.ResolvedGraph      = Graph.Graph;
			Runtime.ResolvedGeneration = Generation;
			Runtime.SetAsset(Graph.Graph.empty() ? nullptr : FAnimGraphLibrary::Get().Load(Graph.Graph), bReload);
		}
		if (!Runtime.Asset)
		{
			return false;
		}
		if (Runtime.BoundSet != &Set)
		{
			std::vector<std::string> Missing;
			Runtime.Rebind(Set, &Missing);
			for (const std::string& Clip : Missing)
			{
				E_LOG(LogAnimation, Warning, "애니메이션 그래프 {}: 모델에 클립 '{}'이 없습니다 (그 샘플은 빼고 섞습니다)", Graph.Graph, Clip);
			}
		}
		return true;
	}

	// 그래프 재생 (규칙은 Scene/AnimGraph.h 머리 주석)
	void UpdateGraphAnimation(FScene& Scene, FEntity Entity, FAnimationComponent& Animation, FAnimGraphComponent& Graph, float DeltaSeconds)
	{
		FAnimationRuntime&   Runtime      = Animation.Runtime;
		FAnimGraphRuntime&   GraphRuntime = Graph.Runtime;
		const FAnimationSet& Set          = *Runtime.Set;
		const float          Delta        = Animation.bPlaying ? DeltaSeconds * Animation.Speed : 0.0f;

		GraphRuntime.Instance.Update(*GraphRuntime.Asset, GraphRuntime.Binding, GraphRuntime.Parameters, Delta);
		const std::vector<FAnimClipContribution>& Contributions = GraphRuntime.Instance.GetContributions();
		const FAnimNotifySource&                  Source        = GraphRuntime.Instance.GetNotifySource();

		// 노티파이: 가중치가 가장 큰 기여 하나 (바뀌면 이전 기여의 스테이트를 끝낸다)
		if (Source.Key != GraphRuntime.NotifyKey)
		{
			if (const std::vector<FAnimNotify>* Old = FindClipNotifies(Runtime, Runtime.NotifyClip); Old != nullptr && GraphRuntime.NotifyKey != 0)
			{
				Runtime.ActiveStates.resize(Old->size(), 0);
				AnimNotifyMath::EndAll(Runtime.ActiveStates, Runtime.HitScratch);
				EmitNotifies(Runtime, Entity, *Old, Set.Clips[Runtime.NotifyClip].Name, 0.0f);
			}
			Runtime.ActiveStates.clear();
			Runtime.NotifyClip    = Source.Clip;
			Runtime.bResyncStates = true;
			GraphRuntime.NotifyKey = Source.Key;
		}
		if (const std::vector<FAnimNotify>* Notifies = FindClipNotifies(Runtime, Source.Clip); Notifies != nullptr && Source.Delta != 0.0f)
		{
			AnimNotifyMath::Collect(*Notifies, Source.PreviousTime, Source.NewTime, Source.Delta, Source.Duration, Source.bLoop, Source.bWrapped,
			                        Runtime.bResyncStates, Runtime.ActiveStates, Runtime.HitScratch);
			EmitNotifies(Runtime, Entity, *Notifies, Set.Clips[Source.Clip].Name, DeltaSeconds);
			Runtime.bResyncStates = false;
		}
		// 인스펙터/GetCurrentClip용: 대표 클립
		if (Source.Clip >= 0)
		{
			Runtime.CurrentClip = Source.Clip;
			Runtime.CurrentTime = Source.NewTime;
		}

		// 포즈 = 기여의 가중 합
		if (Contributions.empty())
		{
			return;
		}
		bool bFirst = true;
		for (const FAnimClipContribution& Contribution : Contributions)
		{
			GraphRuntime.SampleScratch = Set.RestPose;
			AnimationMath::SampleClip(Set.Clips[Contribution.Clip], Contribution.Time, GraphRuntime.SampleScratch);
			AnimGraphMath::AddWeightedPose(GraphRuntime.PoseScratch, GraphRuntime.SampleScratch, Contribution.Weight, bFirst);
			bFirst = false;
		}
		AnimGraphMath::FinishWeightedPose(GraphRuntime.PoseScratch);
		WritePose(Scene, Runtime, GraphRuntime.PoseScratch);
	}

	void UpdateAnimation(FScene& Scene, FEntity Entity, FAnimationComponent& Animation, float DeltaSeconds)
	{
		FAnimationRuntime& Runtime = Animation.Runtime;
		if (!Runtime.Set || Runtime.Set->Clips.empty())
		{
			return;
		}
		Runtime.PendingNotifies.clear();
		if (FAnimGraphComponent* Graph = Scene.GetRegistry().TryGet<FAnimGraphComponent>(Entity); Graph != nullptr && ResolveGraph(*Graph, *Runtime.Set))
		{
			UpdateGraphAnimation(Scene, Entity, Animation, *Graph, DeltaSeconds);
			return;
		}
		const FAnimationSet& Set = *Runtime.Set;
		ResolveClipChange(Animation);
		EndStatesIfClipChanged(Runtime, Entity, Runtime.bResyncStates);

		const FAnimationClip& Clip  = Set.Clips[Runtime.CurrentClip];
		const float           Delta = Animation.bPlaying ? DeltaSeconds * Animation.Speed : 0.0f;

		const float PreviousCurrentTime = Runtime.CurrentTime;
		bool        bWrapped            = false;
		Runtime.CurrentTime = AnimationMath::AdvanceTime(Runtime.CurrentTime, Delta, Clip.Duration, Animation.bLoop, bWrapped);

		// 노티파이: 이번 진행 구간에서 지나간 시점/구간
		if (const std::vector<FAnimNotify>* Notifies = FindClipNotifies(Runtime, Runtime.CurrentClip); Notifies != nullptr && Delta != 0.0f)
		{
			AnimNotifyMath::Collect(*Notifies, PreviousCurrentTime, Runtime.CurrentTime, Delta, Clip.Duration, Animation.bLoop, bWrapped,
			                        Runtime.bResyncStates, Runtime.ActiveStates, Runtime.HitScratch);
			EmitNotifies(Runtime, Entity, *Notifies, Clip.Name, DeltaSeconds);
			Runtime.bResyncStates = false;
		}

		// 현재 클립 포즈
		Runtime.PoseScratch = Set.RestPose;
		AnimationMath::SampleClip(Clip, Runtime.CurrentTime, Runtime.PoseScratch);

		// 크로스페이드: 이전 클립도 계속 진행시키며 가중 보간
		if (Runtime.PreviousClip >= 0)
		{
			const FAnimationClip& PreviousClip = Set.Clips[Runtime.PreviousClip];
			bool                  bIgnored     = false;
			Runtime.PreviousTime = AnimationMath::AdvanceTime(Runtime.PreviousTime, Delta, PreviousClip.Duration, Animation.bLoop, bIgnored);
			Runtime.BlendElapsed += Animation.bPlaying ? DeltaSeconds : 0.0f;

			const float Weight = AnimationMath::ComputeCrossfadeWeight(Runtime.BlendElapsed, Runtime.BlendDuration);
			Runtime.BlendScratch = Set.RestPose;
			AnimationMath::SampleClip(PreviousClip, Runtime.PreviousTime, Runtime.BlendScratch);
			AnimationMath::BlendPoses(Runtime.BlendScratch, Runtime.PoseScratch, Weight, Runtime.PoseScratch);
			if (Weight >= 1.0f)
			{
				Runtime.PreviousClip = -1;
			}
		}

		// 루트 모션: 루트 본의 수평 이동량을 엔티티로 옮기고 본은 첫 프레임 수평 위치에 고정
		if (Animation.bRootMotion && Set.RootMotionNode >= 0)
		{
			if (const FAnimationChannel* Channel = FindTranslationChannel(Clip, Set.RootMotionNode))
			{
				const FMatrix4x4& ParentToModel = Set.RootMotionParentToModel;
				if (Delta != 0.0f)
				{
					FVector3 ModelDelta = ParentToModel.TransformVector(
						AnimationMath::ComputeRootMotionDelta(*Channel, PreviousCurrentTime, Runtime.CurrentTime, bWrapped, Clip.Duration));
					ModelDelta.Z = 0.0f;

					FTransformComponent& RootTransform = Scene.GetTransform(Entity);
					RootTransform.Position += RootTransform.Rotation.RotateVector(ModelDelta * RootTransform.Scale);
				}

				const FVector4 StartValue = AnimationMath::SampleChannel(*Channel, 0.0f);
				const FVector3 Start      = ParentToModel.TransformPosition(FVector3(StartValue.X, StartValue.Y, StartValue.Z));
				FNodePose&     RootPose   = Runtime.PoseScratch[Set.RootMotionNode];
				FVector3       Current    = ParentToModel.TransformPosition(RootPose.Translation);
				Current.X                 = Start.X;
				Current.Y                 = Start.Y;
				RootPose.Translation      = ParentToModel.GetInverse().TransformPosition(Current);
			}
		}

		WritePose(Scene, Runtime, Runtime.PoseScratch);
	}
} // namespace

void FAnimationSystem::Update(FScene& Scene, float DeltaSeconds)
{
	Scene.GetRegistry().View<FAnimationComponent>().Each([&](FEntity Entity, FAnimationComponent& Animation) {
		UpdateAnimation(Scene, Entity, Animation, DeltaSeconds);
	});
}

bool FAnimationSystem::Play(FScene& Scene, FEntity Entity, std::string_view ClipName, float BlendTime)
{
	FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity);
	if (Animation == nullptr || !Animation->Runtime.Set || Animation->Runtime.Set->FindClip(ClipName) < 0)
	{
		return false;
	}
	Animation->Clip                     = std::string(ClipName);
	Animation->bPlaying                 = true;
	Animation->Runtime.PendingBlendTime = BlendTime;
	return true;
}

void FAnimationSystem::Stop(FScene& Scene, FEntity Entity)
{
	if (FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity))
	{
		Animation->bPlaying = false;
	}
}

void FAnimationSystem::Resume(FScene& Scene, FEntity Entity)
{
	if (FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity))
	{
		Animation->bPlaying = true;
	}
}

void FAnimationSystem::SetSpeed(FScene& Scene, FEntity Entity, float Speed)
{
	if (FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity))
	{
		Animation->Speed = Speed;
	}
}

void FAnimationSystem::SetTime(FScene& Scene, FEntity Entity, float Seconds)
{
	FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity);
	if (Animation == nullptr || !Animation->Runtime.Set || Animation->Runtime.Set->Clips.empty())
	{
		return;
	}
	ResolveClipChange(*Animation);
	FAnimationRuntime& Runtime = Animation->Runtime;
	Runtime.CurrentTime        = FMath::Clamp(Seconds, 0.0f, Runtime.Set->Clips[Runtime.CurrentClip].Duration);
	Runtime.PreviousClip       = -1;
	// 스크럽: 점 노티파이는 건너뛰고, 진행 중 스테이트는 다음 갱신에서 End 후 그 시각 기준으로 다시 Begin
	Runtime.bResyncStates = true;
}

float FAnimationSystem::GetTime(FScene& Scene, FEntity Entity)
{
	const FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity);
	return Animation != nullptr && Animation->Runtime.CurrentClip >= 0 ? Animation->Runtime.CurrentTime : 0.0f;
}

float FAnimationSystem::GetCurrentClipDuration(FScene& Scene, FEntity Entity)
{
	const FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity);
	if (Animation == nullptr || !Animation->Runtime.Set || Animation->Runtime.CurrentClip < 0 ||
	    Animation->Runtime.CurrentClip >= static_cast<int32>(Animation->Runtime.Set->Clips.size()))
	{
		return 0.0f;
	}
	return Animation->Runtime.Set->Clips[Animation->Runtime.CurrentClip].Duration;
}

std::vector<std::string> FAnimationSystem::GetClipNames(FScene& Scene, FEntity Entity)
{
	std::vector<std::string> Names;
	if (const FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity); Animation && Animation->Runtime.Set)
	{
		for (const FAnimationClip& Clip : Animation->Runtime.Set->Clips)
		{
			Names.push_back(Clip.Name);
		}
	}
	return Names;
}

std::string FAnimationSystem::GetCurrentClip(FScene& Scene, FEntity Entity)
{
	const FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity);
	if (Animation == nullptr || !Animation->Runtime.Set || Animation->Runtime.CurrentClip < 0)
	{
		return {};
	}
	return Animation->Runtime.Set->Clips[Animation->Runtime.CurrentClip].Name;
}

// ---------------------------------------------------------------- 애니메이션 그래프

FEntity FAnimationSystem::FindAnimGraph(const FScene& Scene, FEntity Entity)
{
	const FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Entity))
	{
		return NullEntity;
	}
	if (Registry.Has<FAnimGraphComponent>(Entity))
	{
		return Entity;
	}
	for (const FEntity Child : Scene.GetChildren(Entity))
	{
		if (const FEntity Found = FindAnimGraph(Scene, Child); Found.IsValid())
		{
			return Found;
		}
	}
	return NullEntity;
}

bool FAnimationSystem::SetAnimParam(FScene& Scene, FEntity Entity, std::string_view Name, float Value)
{
	const FEntity Target = FindAnimGraph(Scene, Entity);
	if (!Target.IsValid())
	{
		return false;
	}
	Scene.GetRegistry().Get<FAnimGraphComponent>(Target).Runtime.Parameters.Set(Name, Value);
	return true;
}

bool FAnimationSystem::SetAnimParam(FScene& Scene, FEntity Entity, std::string_view Name, bool bValue)
{
	return SetAnimParam(Scene, Entity, Name, bValue ? 1.0f : 0.0f);
}

std::optional<float> FAnimationSystem::GetAnimParam(FScene& Scene, FEntity Entity, std::string_view Name)
{
	const FEntity Target = FindAnimGraph(Scene, Entity);
	if (!Target.IsValid())
	{
		return std::nullopt;
	}
	const FAnimGraphRuntime& Runtime = Scene.GetRegistry().Get<FAnimGraphComponent>(Target).Runtime;
	float                    Value   = 0.0f;
	if (Runtime.Parameters.TryGet(Name, Value))
	{
		return Value;
	}
	if (Runtime.Asset)
	{
		if (const FAnimGraphParameter* Parameter = Runtime.Asset->FindParameter(Name))
		{
			return Parameter->Default;
		}
	}
	return std::nullopt;
}

bool FAnimationSystem::IsAnimParamBool(FScene& Scene, FEntity Entity, std::string_view Name)
{
	const FEntity Target = FindAnimGraph(Scene, Entity);
	if (!Target.IsValid())
	{
		return false;
	}
	const FAnimGraphRuntime&   Runtime   = Scene.GetRegistry().Get<FAnimGraphComponent>(Target).Runtime;
	const FAnimGraphParameter* Parameter = Runtime.Asset ? Runtime.Asset->FindParameter(Name) : nullptr;
	return Parameter != nullptr && Parameter->Type == EAnimParamType::Bool;
}

std::string FAnimationSystem::GetAnimState(FScene& Scene, FEntity Entity)
{
	const FEntity Target = FindAnimGraph(Scene, Entity);
	if (!Target.IsValid())
	{
		return {};
	}
	const FAnimGraphRuntime& Runtime = Scene.GetRegistry().Get<FAnimGraphComponent>(Target).Runtime;
	const int32              State   = Runtime.Instance.GetCurrentState();
	if (!Runtime.Asset || State < 0 || State >= static_cast<int32>(Runtime.Asset->States.size()))
	{
		return {};
	}
	return Runtime.Asset->States[State].Name;
}
