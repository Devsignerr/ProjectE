#include "Scene/AnimationSystem.h"

#include "Core/Profiling.h"
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
		FAnimNotifyTrack& Track = Runtime.Notify;
		if (Track.Clip == Runtime.CurrentClip && !bForceEnd)
		{
			return;
		}
		if (const std::vector<FAnimNotify>* Old = FindClipNotifies(Runtime, Track.Clip))
		{
			Track.ActiveStates.resize(Old->size(), 0);
			AnimNotifyMath::EndAll(Track.ActiveStates, Runtime.HitScratch);
			EmitNotifies(Runtime, Entity, *Old, Runtime.Set->Clips[Track.Clip].Name, 0.0f);
		}
		Track.ActiveStates.clear();
		Track.Clip = Runtime.CurrentClip;
	}

	// 그래프 레이어 하나의 노티파이 판정 (기준 기여가 바뀌면 이전 기여의 진행 중 스테이트 End). bEnabled = false면 기준 없음
	void UpdateNotifyTrack(FAnimationRuntime& Runtime, FEntity Entity, FAnimNotifyTrack& Track, const FAnimNotifySource& Source, bool bEnabled,
	                       float DeltaSeconds)
	{
		const uint32 Key = bEnabled ? Source.Key : 0u;
		if (Key != Track.Key)
		{
			if (const std::vector<FAnimNotify>* Old = FindClipNotifies(Runtime, Track.Clip); Old != nullptr && Track.Key != 0)
			{
				Track.ActiveStates.resize(Old->size(), 0);
				AnimNotifyMath::EndAll(Track.ActiveStates, Runtime.HitScratch);
				EmitNotifies(Runtime, Entity, *Old, Runtime.Set->Clips[Track.Clip].Name, 0.0f);
			}
			Track.ActiveStates.clear();
			Track.Clip    = Key != 0 ? Source.Clip : -1;
			Track.bResync = true;
			Track.Key     = Key;
		}
		if (Key == 0)
		{
			return;
		}
		if (const std::vector<FAnimNotify>* Notifies = FindClipNotifies(Runtime, Source.Clip); Notifies != nullptr && Source.Delta != 0.0f)
		{
			AnimNotifyMath::Collect(*Notifies, Source.PreviousTime, Source.NewTime, Source.Delta, Source.Duration, Source.bLoop, Source.bWrapped,
			                        Track.bResync, Track.ActiveStates, Runtime.HitScratch);
			EmitNotifies(Runtime, Entity, *Notifies, Runtime.Set->Clips[Source.Clip].Name, DeltaSeconds);
			Track.bResync = false;
		}
	}

	// 기여 목록의 가중 포즈 → Out. 기여가 없으면 false (Out 그대로)
	bool BuildContributionPose(const FAnimationSet& Set, const std::vector<FAnimClipContribution>& Contributions, std::vector<FNodePose>& Sample,
	                           std::vector<FNodePose>& Out)
	{
		if (Contributions.empty())
		{
			return false;
		}
		bool bFirst = true;
		for (const FAnimClipContribution& Contribution : Contributions)
		{
			Sample = Set.RestPose;
			AnimationMath::SampleClip(Set.Clips[Contribution.Clip], Contribution.Time, Sample);
			AnimGraphMath::AddWeightedPose(Out, Sample, Contribution.Weight, bFirst);
			bFirst = false;
		}
		AnimGraphMath::FinishWeightedPose(Out);
		return true;
	}

	// 모델 노드 이름 (노드 엔티티 이름 — 본 마스크/IK 뼈 찾기)
	std::vector<std::string> GetNodeNames(const FScene& Scene, const FAnimationRuntime& Runtime)
	{
		std::vector<std::string> Names(Runtime.Set ? Runtime.Set->NodeParents.size() : 0);
		const FRegistry&         Registry = Scene.GetRegistry();
		for (size_t Node = 0; Node < Names.size() && Node < Runtime.NodeEntities.size(); ++Node)
		{
			const FEntity Entity = Runtime.NodeEntities[Node];
			if (const FNameComponent* Name = Registry.IsValid(Entity) ? Registry.TryGet<FNameComponent>(Entity) : nullptr)
			{
				Names[Node] = Name->Name;
			}
		}
		return Names;
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
	bool ResolveGraph(const FScene& Scene, FAnimGraphComponent& Graph, const FAnimationRuntime& Animation)
	{
		const FAnimationSet& Set = *Animation.Set;
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
			std::vector<std::string> MissingBones;
			Runtime.Rebind(Set, GetNodeNames(Scene, Animation), &Missing, &MissingBones);
			for (const std::string& Clip : Missing)
			{
				E_LOG(LogAnimation, Warning, "애니메이션 그래프 {}: 모델에 클립 '{}'이 없습니다 (그 샘플은 빼고 섞습니다)", Graph.Graph, Clip);
			}
			for (const std::string& Bone : MissingBones)
			{
				E_LOG(LogAnimation, Warning, "애니메이션 그래프 {}: 본 마스크의 뼈 '{}'를 모델에서 찾을 수 없습니다", Graph.Graph, Bone);
			}
		}
		return true;
	}

	// 진행 중 스테이트를 모두 End
	void EndNotifyTrack(FAnimationRuntime& Runtime, FEntity Entity, FAnimNotifyTrack& Track)
	{
		if (const std::vector<FAnimNotify>* Old = FindClipNotifies(Runtime, Track.Clip); Old != nullptr && !Track.ActiveStates.empty())
		{
			Track.ActiveStates.resize(Old->size(), 0);
			AnimNotifyMath::EndAll(Track.ActiveStates, Runtime.HitScratch);
			EmitNotifies(Runtime, Entity, *Old, Runtime.Set->Clips[Track.Clip].Name, 0.0f);
		}
		Track.ActiveStates.clear();
	}

	// ---- 몽타주 (규칙은 Scene/AnimMontage.h)

	std::string NormalizeSlot(std::string_view Slot)
	{
		return Slot.empty() ? std::string(FAnimationSystem::DefaultMontageSlot) : std::string(Slot);
	}

	// 슬롯 마스크 (nullptr = 몸 전체: 그래프 없음 / 슬롯 없음 / 빈 마스크)
	const std::vector<float>* FindSlotMask(const FAnimGraphComponent* Graph, const std::string& Slot)
	{
		if (Graph == nullptr || !Graph->Runtime.Asset)
		{
			return nullptr;
		}
		const int32 Index = Graph->Runtime.Asset->FindSlot(Slot);
		if (Index < 0 || Index >= static_cast<int32>(Graph->Runtime.SlotMasks.size()) || Graph->Runtime.Asset->Slots[static_cast<size_t>(Index)].Mask.IsFullBody())
		{
			return nullptr;
		}
		return &Graph->Runtime.SlotMasks[static_cast<size_t>(Index)];
	}

	// 몽타주 진행 구간의 노티파이. 부분 구간 반복이 감기면 (이전 → 구간 끝) + (구간 시작 → 새 시각)으로 나눠 판정한다
	void CollectMontageNotifies(FAnimationRuntime& Runtime, FEntity Entity, FAnimMontageInstance& Montage, const FMontageStep& Step, float DeltaSeconds)
	{
		const std::vector<FAnimNotify>* Notifies = FindClipNotifies(Runtime, Montage.ClipIndex);
		if (Notifies == nullptr || Step.Delta == 0.0f)
		{
			return;
		}
		FAnimNotifyTrack& Track = Montage.Notify;
		Track.Clip              = Montage.ClipIndex;
		const float Start       = AnimMontageMath::GetStartTime(Montage);
		const float End         = AnimMontageMath::GetEndTime(Montage);
		const bool  bSubRange   = Start > 0.0f || End < Montage.Duration;
		if (Step.bWrapped && bSubRange)
		{
			const bool  bForward = Step.Delta > 0.0f;
			const float Edge     = bForward ? End : Start;
			const float Restart  = bForward ? Start : End;
			AnimNotifyMath::Collect(*Notifies, Step.PreviousTime, Edge, Edge - Step.PreviousTime, Montage.Duration, false, false, Track.bResync,
			                        Track.ActiveStates, Runtime.HitScratch);
			Track.bResync = false;
			AnimNotifyMath::Collect(*Notifies, Restart, Step.NewTime, Step.NewTime - Restart, Montage.Duration, false, false, true, Track.ActiveStates,
			                        Runtime.HitScratch);
		}
		else
		{
			AnimNotifyMath::Collect(*Notifies, Step.PreviousTime, Step.NewTime, Step.Delta, Montage.Duration, Montage.Params.bLoop, Step.bWrapped,
			                        Track.bResync, Track.ActiveStates, Runtime.HitScratch);
		}
		EmitNotifies(Runtime, Entity, *Notifies, Montage.Clip, DeltaSeconds);
		Track.bResync = false;
	}

	// 몽타주 시각/가중치/노티파이/끝 이벤트. 반환 = 몸 전체 몽타주 가중치 >= 0.5 (아래 원천의 노티파이를 멈춘다)
	bool UpdateMontages(FAnimationRuntime& Runtime, FEntity Entity, const FAnimGraphComponent* Graph, float Delta, float DeltaSeconds)
	{
		const FAnimationSet& Set       = *Runtime.Set;
		bool                 bSuppress = false;
		for (FAnimMontageInstance& Montage : Runtime.Montages)
		{
			// 모델이 다시 인스턴스화되면 클립 번호가 바뀔 수 있다 — 이름으로 다시 찾고, 없으면 중단
			if (Montage.ClipIndex < 0 || Montage.ClipIndex >= static_cast<int32>(Set.Clips.size()) || Set.Clips[static_cast<size_t>(Montage.ClipIndex)].Name != Montage.Clip)
			{
				Montage.ClipIndex = Set.FindClip(Montage.Clip);
				if (Montage.ClipIndex < 0)
				{
					Montage.Notify.ActiveStates.clear();
					AnimMontageMath::BeginBlendOut(Montage, 0.0f, true);
				}
			}
			const FMontageStep Step = AnimMontageMath::Advance(Montage, Delta);
			if (Montage.bInterrupted)
			{
				EndNotifyTrack(Runtime, Entity, Montage.Notify); // 중단 뒤에는 노티파이 없음
			}
			else
			{
				CollectMontageNotifies(Runtime, Entity, Montage, Step, DeltaSeconds);
			}
			if (Montage.bFinished)
			{
				EndNotifyTrack(Runtime, Entity, Montage.Notify);
				Runtime.PendingMontageEvents.push_back({ Entity, Montage.Clip, Montage.Params.Slot, Montage.bInterrupted });
			}
			else if (Montage.Weight >= 0.5f && FindSlotMask(Graph, Montage.Params.Slot) == nullptr)
			{
				bSuppress = true;
			}
		}
		std::erase_if(Runtime.Montages, [](const FAnimMontageInstance& Montage) { return Montage.bFinished; });
		return bSuppress;
	}

	// 몽타주 포즈를 시작 순서대로 덮는다 (슬롯 마스크 × 가중치). 원천 포즈가 없었으면 기본 포즈에서 시작
	bool ApplyMontages(FAnimationRuntime& Runtime, const FAnimGraphComponent* Graph, bool bHavePose)
	{
		static const std::vector<float> FullBody;
		const FAnimationSet&            Set = *Runtime.Set;
		for (const FAnimMontageInstance& Montage : Runtime.Montages)
		{
			if (Montage.Weight <= 0.0f || Montage.ClipIndex < 0 || Montage.ClipIndex >= static_cast<int32>(Set.Clips.size()))
			{
				continue;
			}
			if (!bHavePose)
			{
				Runtime.PoseScratch = Set.RestPose;
				bHavePose           = true;
			}
			Runtime.MontageScratch = Set.RestPose;
			AnimationMath::SampleClip(Set.Clips[static_cast<size_t>(Montage.ClipIndex)], Montage.Time, Runtime.MontageScratch);
			const std::vector<float>* Mask = FindSlotMask(Graph, Montage.Params.Slot);
			AnimGraphMath::BlendMasked(Runtime.PoseScratch, Runtime.MontageScratch, Mask != nullptr ? *Mask : FullBody, Montage.Weight);
		}
		return bHavePose;
	}

	// 그래프 재생 → OutPose (규칙은 Scene/AnimGraph.h 머리 주석). 기본 레이어 기여가 없으면 false (포즈를 쓰지 않는다)
	//   bSuppressBaseNotifies: 몸 전체 몽타주가 덮는 중 — 기본 레이어 노티파이를 판정하지 않는다
	bool EvaluateGraph(FEntity Entity, FAnimationComponent& Animation, FAnimGraphComponent& Graph, float DeltaSeconds, bool bSuppressBaseNotifies,
	                   std::vector<FNodePose>& OutPose)
	{
		FAnimationRuntime&     Runtime      = Animation.Runtime;
		FAnimGraphRuntime&     GraphRuntime = Graph.Runtime;
		const FAnimationSet&   Set          = *Runtime.Set;
		const FAnimGraphAsset& Asset        = *GraphRuntime.Asset;
		const float            Delta        = Animation.bPlaying ? DeltaSeconds * Animation.Speed : 0.0f;

		// 기본 레이어: 노티파이는 가중치가 가장 큰 기여 하나
		GraphRuntime.Instance.Update(Asset, GraphRuntime.Binding, GraphRuntime.Parameters, Delta);
		const FAnimNotifySource& Source = GraphRuntime.Instance.GetNotifySource();
		UpdateNotifyTrack(Runtime, Entity, GraphRuntime.BaseNotify, Source, !bSuppressBaseNotifies, DeltaSeconds);
		// 인스펙터/GetCurrentClip용: 대표 클립
		if (Source.Clip >= 0)
		{
			Runtime.CurrentClip = Source.Clip;
			Runtime.CurrentTime = Source.NewTime;
		}
		const bool bHavePose = BuildContributionPose(Set, GraphRuntime.Instance.GetContributions(), GraphRuntime.SampleScratch, OutPose);

		// 추가 레이어: 순서대로 마스크 × 레이어 가중치로 덮어 섞는다. 노티파이는 레이어 가중치 >= 0.5일 때만
		const size_t LayerCount = Asset.Layers.size();
		if (GraphRuntime.LayerInstances.size() != LayerCount || GraphRuntime.Binding.Layers.size() != LayerCount)
		{
			return bHavePose;
		}
		for (size_t Layer = 0; Layer < LayerCount; ++Layer)
		{
			const FAnimGraphLayer& Data   = Asset.Layers[Layer];
			const float            Weight = AnimGraphMath::ComputeLayerWeight(Asset, Data, GraphRuntime.Parameters);
			FAnimGraphInstance&    Instance = GraphRuntime.LayerInstances[Layer];
			GraphRuntime.LayerWeights[Layer] = Weight;
			Instance.Update(Asset, Data, GraphRuntime.Binding.Layers[Layer], GraphRuntime.Parameters, Delta);
			UpdateNotifyTrack(Runtime, Entity, GraphRuntime.LayerNotify[Layer], Instance.GetNotifySource(), Weight >= 0.5f, DeltaSeconds);
			if (bHavePose && Weight > 0.0f &&
			    BuildContributionPose(Set, Instance.GetContributions(), GraphRuntime.SampleScratch, GraphRuntime.LayerPoseScratch))
			{
				AnimGraphMath::BlendMasked(OutPose, GraphRuntime.LayerPoseScratch, GraphRuntime.LayerMasks[Layer], Weight);
			}
		}
		return bHavePose;
	}

	// 클립 재생(크로스페이드, 루트 모션) → Runtime.PoseScratch. bSuppressNotifies: 몸 전체 몽타주가 덮는 중
	bool EvaluateClip(FScene& Scene, FEntity Entity, FAnimationComponent& Animation, float DeltaSeconds, bool bSuppressNotifies)
	{
		FAnimationRuntime&   Runtime = Animation.Runtime;
		const FAnimationSet& Set     = *Runtime.Set;
		ResolveClipChange(Animation);
		EndStatesIfClipChanged(Runtime, Entity, Runtime.Notify.bResync);

		const FAnimationClip& Clip  = Set.Clips[Runtime.CurrentClip];
		const float           Delta = Animation.bPlaying ? DeltaSeconds * Animation.Speed : 0.0f;

		const float PreviousCurrentTime = Runtime.CurrentTime;
		bool        bWrapped            = false;
		Runtime.CurrentTime = AnimationMath::AdvanceTime(Runtime.CurrentTime, Delta, Clip.Duration, Animation.bLoop, bWrapped);

		// 노티파이: 이번 진행 구간에서 지나간 시점/구간 (몽타주가 덮는 동안은 멈추고, 끝나면 그 시각부터 다시)
		if (bSuppressNotifies)
		{
			EndNotifyTrack(Runtime, Entity, Runtime.Notify);
			Runtime.Notify.bResync = true;
		}
		else if (const std::vector<FAnimNotify>* Notifies = FindClipNotifies(Runtime, Runtime.CurrentClip); Notifies != nullptr && Delta != 0.0f)
		{
			AnimNotifyMath::Collect(*Notifies, PreviousCurrentTime, Runtime.CurrentTime, Delta, Clip.Duration, Animation.bLoop, bWrapped,
			                        Runtime.Notify.bResync, Runtime.Notify.ActiveStates, Runtime.HitScratch);
			EmitNotifies(Runtime, Entity, *Notifies, Clip.Name, DeltaSeconds);
			Runtime.Notify.bResync = false;
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
		return true;
	}

	// 포즈 단계: 원천(그래프 | 클립) → (몽타주) → (IK) → 노드 엔티티에 기록
	void UpdateAnimation(FScene& Scene, FEntity Entity, FAnimationComponent& Animation, float DeltaSeconds)
	{
		FAnimationRuntime& Runtime = Animation.Runtime;
		if (!Runtime.Set || Runtime.Set->Clips.empty())
		{
			return;
		}
		Runtime.PendingNotifies.clear();
		Runtime.PendingMontageEvents.clear();
		if (Runtime.bPhysicsPose)
		{
			return; // 래그돌: 물리가 뼈 트랜스폼을 쓴다 (재생 시간·노티파이·몽타주·IK도 멈춘다)
		}
		FAnimGraphComponent* Graph = Scene.GetRegistry().TryGet<FAnimGraphComponent>(Entity);
		if (Graph != nullptr && !ResolveGraph(Scene, *Graph, Runtime))
		{
			Graph = nullptr;
		}
		// 몽타주 진행을 먼저 (몸 전체 몽타주가 덮는 중이면 아래 원천의 노티파이를 멈춘다)
		const float Delta     = Animation.bPlaying ? DeltaSeconds * Animation.Speed : 0.0f;
		const bool  bSuppress = !Runtime.Montages.empty() && UpdateMontages(Runtime, Entity, Graph, Delta, DeltaSeconds);

		bool bHavePose = Graph != nullptr ? EvaluateGraph(Entity, Animation, *Graph, DeltaSeconds, bSuppress, Runtime.PoseScratch)
		                                  : EvaluateClip(Scene, Entity, Animation, DeltaSeconds, bSuppress);
		bHavePose = ApplyMontages(Runtime, Graph, bHavePose);
		if (bHavePose)
		{
			WritePose(Scene, Runtime, Runtime.PoseScratch);
		}
	}
} // namespace

void FAnimationSystem::Update(FScene& Scene, float DeltaSeconds)
{
	E_PROFILE_SCOPE("애니메이션");
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
	Runtime.Notify.bResync = true;
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

// ---------------------------------------------------------------- 몽타주

FEntity FAnimationSystem::FindAnimation(const FScene& Scene, FEntity Entity)
{
	const FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Entity))
	{
		return NullEntity;
	}
	if (Registry.Has<FAnimationComponent>(Entity))
	{
		return Entity;
	}
	for (const FEntity Child : Scene.GetChildren(Entity))
	{
		if (const FEntity Found = FindAnimation(Scene, Child); Found.IsValid())
		{
			return Found;
		}
	}
	return NullEntity;
}

bool FAnimationSystem::PlayMontage(FScene& Scene, FEntity Entity, std::string_view ClipName, const FMontagePlayParams& Params)
{
	const FEntity Target = FindAnimation(Scene, Entity);
	if (!Target.IsValid())
	{
		return false;
	}
	FAnimationRuntime& Runtime = Scene.GetRegistry().Get<FAnimationComponent>(Target).Runtime;
	const int32        Clip    = Runtime.Set ? Runtime.Set->FindClip(ClipName) : -1;
	if (Clip < 0)
	{
		E_LOG(LogAnimation, Warning, "몽타주: 모델에 클립 '{}'이 없습니다", ClipName);
		return false;
	}
	FAnimMontageInstance Montage;
	Montage.Clip        = std::string(ClipName);
	Montage.ClipIndex   = Clip;
	Montage.Params      = Params;
	Montage.Params.Slot = NormalizeSlot(Params.Slot);
	Montage.Params.BlendIn  = FMath::Max(Montage.Params.BlendIn, 0.0f);
	Montage.Params.BlendOut = FMath::Max(Montage.Params.BlendOut, 0.0f);
	Montage.Duration    = Runtime.Set->Clips[static_cast<size_t>(Clip)].Duration;
	AnimMontageMath::Start(Montage);
	// 같은 슬롯의 재생 중 몽타주는 새 BlendIn 시간으로 빠진다 (중단)
	for (FAnimMontageInstance& Other : Runtime.Montages)
	{
		if (Other.Params.Slot == Montage.Params.Slot)
		{
			AnimMontageMath::BeginBlendOut(Other, Montage.Params.BlendIn, true);
		}
	}
	Runtime.Montages.push_back(std::move(Montage));
	return true;
}

bool FAnimationSystem::StopMontage(FScene& Scene, FEntity Entity, std::string_view Slot, float BlendOut)
{
	const FEntity Target = FindAnimation(Scene, Entity);
	if (!Target.IsValid())
	{
		return false;
	}
	bool bStopped = false;
	for (FAnimMontageInstance& Montage : Scene.GetRegistry().Get<FAnimationComponent>(Target).Runtime.Montages)
	{
		if ((Slot.empty() || Montage.Params.Slot == Slot) && !Montage.bInterrupted)
		{
			AnimMontageMath::BeginBlendOut(Montage, BlendOut, true);
			bStopped = true;
		}
	}
	return bStopped;
}

bool FAnimationSystem::IsMontagePlaying(FScene& Scene, FEntity Entity, std::string_view Slot)
{
	const FEntity Target = FindAnimation(Scene, Entity);
	if (!Target.IsValid())
	{
		return false;
	}
	for (const FAnimMontageInstance& Montage : Scene.GetRegistry().Get<FAnimationComponent>(Target).Runtime.Montages)
	{
		if ((Slot.empty() || Montage.Params.Slot == Slot) && !Montage.bInterrupted && !Montage.bFinished)
		{
			return true;
		}
	}
	return false;
}
