#include "Scene/AnimationSystem.h"

#include "Core/Profiling.h"
#include "Core/Console/Console.h"
#include "Core/Jobs/ParallelFor.h"
#include "Core/Log.h"
#include "Scene/AnimGraph.h"
#include "Scene/AnimIK.h"
#include "Scene/AnimRetarget.h"
#include "Scene/AnimUpdateRate.h"
#include "Scene/CameraProjection.h"
#include "Scene/Components.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <format>

E_DEFINE_LOG_CATEGORY(LogAnimation, Log)

namespace
{
	FRootMotionReceiver GRootMotionReceiver = nullptr;

	// ---- 콘솔 변수 (병렬 평가·갱신 빈도 LOD, 규칙은 Scene/AnimUpdateRate.h와 FAnimationSystem::Update 주석)
	TAutoConsoleVariable<bool> CVarParallel("a.ParallelEvaluate", true, "애니메이션 포즈 평가를 엔티티별 병렬로 (0 = 메인 스레드 순차, 결과는 같다)");
	TAutoConsoleVariable<bool> CVarUro("a.URO", true,
	                                   "애니메이션 갱신 빈도 LOD: 화면에 작게 보이는 모델은 몇 프레임마다 포즈를 평가 (건너뛴 시간은 다음 평가가 진행, 노티파이 유지)");
	TAutoConsoleVariable<float> CVarUroFull("a.URO.FullRateScreenSize", 0.2f, "이 화면 크기(경계 반경 / 화면 세로 절반) 이상이면 매 프레임 평가",
	                                        EConsoleFlags::None, { .Range = std::pair(0.0f, 10.0f) });
	TAutoConsoleVariable<float> CVarUroHalf("a.URO.HalfRateScreenSize", 0.08f, "이 화면 크기 이상이면 2프레임마다, 미만이면 a.URO.MaxInterval프레임마다",
	                                        EConsoleFlags::None, { .Range = std::pair(0.0f, 10.0f) });
	TAutoConsoleVariable<int32> CVarUroMaxInterval("a.URO.MaxInterval", 4, "가장 작게 보이는 모델의 평가 간격 (프레임)", EConsoleFlags::None,
	                                               { .Range = std::pair(1.0f, 16.0f) });
	TAutoConsoleVariable<float> CVarUroRadius("a.URO.Radius", 100.0f, "화면 크기 계산에 쓰는 모델 경계 반경 (cm, 모델 루트 월드 스케일을 곱함)",
	                                          EConsoleFlags::None, { .Range = std::pair(1.0f, 100000.0f) });

	// ---- 루트 모션 (규칙은 Scene/AnimRootMotion.h)

	struct FRootMotionContext
	{
		ERootMotionMode Mode      = ERootMotionMode::None;
		int32           Node      = -1; // -1 = 클립마다 자동
		bool            bRotation = false;

		bool ExtractBase() const { return Mode == ERootMotionMode::All; }
		bool ExtractMontages() const { return Mode != ERootMotionMode::None; }
	};

	FRootMotionContext MakeRootMotionContext(const FAnimationComponent& Animation, const FAnimationSet& Set)
	{
		FRootMotionContext Context;
		Context.Mode      = Animation.RootMotionMode != ERootMotionMode::None ? Animation.RootMotionMode
		                    : Animation.bRootMotion                         ? ERootMotionMode::All
		                                                                    : ERootMotionMode::None;
		Context.Node      = Animation.RootMotionBone.empty() ? -1 : Set.FindNode(Animation.RootMotionBone);
		Context.bRotation = Animation.bRootMotionRotation;
		return Context;
	}

	// 루트 모션 노드 (마스크 가중치 확인용): 지정 뼈 → 클립 자동 → 세트 전체
	int32 GetRootMotionNode(const FRootMotionContext& Context, const FAnimationSet& Set, int32 Clip)
	{
		if (Context.Node >= 0)
		{
			return Context.Node;
		}
		if (Clip >= 0 && Clip < static_cast<int32>(Set.ClipRootMotionNodes.size()) && Set.ClipRootMotionNodes[static_cast<size_t>(Clip)] >= 0)
		{
			return Set.ClipRootMotionNodes[static_cast<size_t>(Clip)];
		}
		return Set.RootMotionNode;
	}

	// 클립 포즈 샘플 (Strip이 있으면 루트 모션을 제자리화)
	void SampleClipPose(const FAnimationSet& Set, int32 Clip, float Time, std::vector<FNodePose>& Out, const FRootMotionContext* Strip)
	{
		Out = Set.RestPose;
		AnimationMath::SampleClip(Set.Clips[static_cast<size_t>(Clip)], Time, Out);
		if (Strip != nullptr)
		{
			const FRootMotionTrack Track = RootMotionMath::MakeTrack(Set, Clip, Strip->Node, Strip->bRotation);
			if (Track.IsValid() && Track.Node < static_cast<int32>(Out.size()))
			{
				RootMotionMath::RemoveFromPose(Track, Time, Out[static_cast<size_t>(Track.Node)]);
			}
		}
	}

	FRootMotionDelta ComputeClipRootMotion(const FAnimationSet& Set, int32 Clip, float PreviousTime, float NewTime, float Delta, bool bWrapped,
	                                       const FRootMotionContext& Context)
	{
		const FRootMotionTrack Track = RootMotionMath::MakeTrack(Set, Clip, Context.Node, Context.bRotation);
		return RootMotionMath::ComputeDelta(Track, PreviousTime, NewTime, Delta, bWrapped, Set.Clips[static_cast<size_t>(Clip)].Duration);
	}

	// 추출한 이동량 적용: 수신자(캐릭터 이동) → 없으면 엔티티 로컬 트랜스폼
	void ApplyRootMotion(FScene& Scene, FEntity Entity, const FRootMotionDelta& Delta, float DeltaSeconds)
	{
		FTransformComponent& Transform = Scene.GetTransform(Entity);
		const FVector3       World     = Transform.WorldMatrix.TransformVector(Delta.Translation);
		if (GRootMotionReceiver != nullptr && GRootMotionReceiver(Scene, Entity, World, Delta.Yaw, DeltaSeconds))
		{
			return;
		}
		RootMotionMath::ApplyToTransform(Delta, Transform.Position, Transform.Rotation, Transform.Scale);
	}

	// ---- 리타기팅 (규칙은 Scene/AnimRetarget.h)

	void AddUniqueSource(std::vector<std::string>& Sources, std::string Source)
	{
		const auto Trim = [](std::string& Text) {
			const size_t First = Text.find_first_not_of(" \t");
			const size_t Last  = Text.find_last_not_of(" \t");
			Text               = First == std::string::npos ? std::string() : Text.substr(First, Last - First + 1);
		};
		Trim(Source);
		if (Source.empty())
		{
			return;
		}
		const std::string Key = FAnimRetargetLibrary::NormalizePath(Source);
		for (const std::string& Existing : Sources)
		{
			if (FAnimRetargetLibrary::NormalizePath(Existing) == Key)
			{
				return;
			}
		}
		Sources.push_back(std::move(Source));
	}

	void AddQualifiedSource(std::vector<std::string>& Sources, std::string_view ClipName)
	{
		std::string Model;
		std::string Clip;
		if (FAnimRetargetLibrary::SplitQualifiedName(ClipName, Model, Clip))
		{
			AddUniqueSource(Sources, std::move(Model));
		}
	}

	// 컴포넌트 소스 목록 + "<모델>:<클립>" 이름이 가리키는 모델 (컴포넌트 Clip, 그래프 샘플, Play/PlayMontage 요청)
	std::vector<std::string> CollectRetargetSources(const FAnimationComponent& Animation, const FAnimGraphComponent* Graph)
	{
		std::vector<std::string> Sources;
		size_t                   Start = 0;
		const std::string&       List  = Animation.RetargetSources;
		while (Start <= List.size())
		{
			const size_t End = List.find_first_of(";,", Start);
			AddUniqueSource(Sources, List.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
			if (End == std::string::npos)
			{
				break;
			}
			Start = End + 1;
		}
		AddQualifiedSource(Sources, Animation.Clip);
		for (const std::string& Requested : Animation.Runtime.RequestedSources)
		{
			AddUniqueSource(Sources, Requested);
		}
		// 그래프 샘플 이름 (아직 묶기 전이면 라이브러리 캐시에서 읽는다 — 같은 경로는 한 번만 읽음)
		std::shared_ptr<const FAnimGraphAsset> GraphAsset;
		if (Graph != nullptr)
		{
			GraphAsset = Graph->Runtime.Asset && Graph->Runtime.ResolvedGraph == Graph->Graph ? Graph->Runtime.Asset
			             : Graph->Graph.empty()                                             ? nullptr
			                                                                                : FAnimGraphLibrary::Get().Load(Graph->Graph);
		}
		if (GraphAsset)
		{
			const FAnimGraphAsset& Asset = *GraphAsset;
			for (int32 Layer = -1; Layer < static_cast<int32>(Asset.Layers.size()); ++Layer)
			{
				for (const FAnimGraphState& State : Asset.GetMachine(Layer).States)
				{
					for (const FAnimBlendSample& Sample : State.Samples)
					{
						AddQualifiedSource(Sources, Sample.Clip);
					}
				}
			}
		}
		return Sources;
	}

	std::string GetRetargetTargetKey(const FScene& Scene, FEntity Entity, const FAnimationRuntime& Runtime)
	{
		if (const FModelComponent* Model = Scene.GetRegistry().TryGet<FModelComponent>(Entity); Model != nullptr && !Model->AssetPath.empty())
		{
			return FAnimRetargetLibrary::NormalizePath(Model->AssetPath);
		}
		return std::format("set:{}", static_cast<const void*>(Runtime.BaseSet.get()));
	}

	// 소스 목록이 바뀌었거나 라이브러리 세대가 바뀌었으면 Set을 다시 만든다. 바뀌었으면 true
	bool ResolveRetargeting(const FScene& Scene, FEntity Entity, FAnimationComponent& Animation, const FAnimGraphComponent* Graph)
	{
		FAnimationRuntime& Runtime = Animation.Runtime;
		if (!Runtime.Set)
		{
			return false;
		}
		if (Runtime.Set.get() != Runtime.BuiltSet || !Runtime.BaseSet)
		{
			// 처음이거나 누군가(모델 다시 인스턴스화) 세트를 바꿈 → 그것이 모델 자신의 세트
			Runtime.BaseSet  = Runtime.Set;
			Runtime.BuiltSet = Runtime.Set.get();
			Runtime.RetargetKey.clear();
		}
		FAnimRetargetLibrary&          Library = FAnimRetargetLibrary::Get();
		const std::vector<std::string> Sources = CollectRetargetSources(Animation, Graph);
		std::string                    Key;
		for (const std::string& Source : Sources)
		{
			Key += FAnimRetargetLibrary::NormalizePath(Source) + ";";
		}
		if (!Key.empty())
		{
			Key += std::format("#{}", Library.GetGeneration());
		}
		if (Key == Runtime.RetargetKey)
		{
			return false;
		}
		Runtime.RetargetKey = Key;

		const FAnimationSet&                  Old  = *Runtime.Set;
		const FAnimationSet&                  Base = *Runtime.BaseSet;
		std::shared_ptr<const FAnimationSet> NewSet;
		if (Sources.empty())
		{
			NewSet = Runtime.BaseSet;
		}
		else
		{
			const std::string                                   TargetKey = GetRetargetTargetKey(Scene, Entity, Runtime);
			std::vector<FAnimationClip>                         Clips     = Base.Clips;
			std::vector<std::shared_ptr<const FModelMetadata>> Notify    = Base.ClipNotifySources;
			std::vector<std::pair<std::string, int32>>          Aliases   = Base.ClipAliases;
			Notify.resize(Clips.size());
			for (const std::string& Source : Sources)
			{
				if (FAnimRetargetLibrary::NormalizePath(Source) == TargetKey)
				{
					for (size_t Index = 0; Index < Base.Clips.size(); ++Index)
					{
						Aliases.emplace_back(Source + ":" + Base.Clips[Index].Name, static_cast<int32>(Index)); // 자기 자신: 별칭만
					}
					continue;
				}
				const auto Retargeted = Library.GetRetargetedClips(Source, TargetKey, Base, Runtime.Metadata.get());
				const auto Model      = Library.LoadSource(Source);
				if (!Retargeted || !Model)
				{
					continue;
				}
				for (const FAnimationClip& Clip : *Retargeted)
				{
					Aliases.emplace_back(Source + ":" + Clip.Name, static_cast<int32>(Clips.size()));
					Clips.push_back(Clip);
					Notify.push_back(Model->Metadata);
				}
			}
			std::shared_ptr<FAnimationSet> Built = MakeAnimationSet(std::move(Clips), Base.NodeParents, Base.RestPose, Base.NodeNames);
			Built->OwnClipCount                  = static_cast<int32>(Base.Clips.size());
			Built->ClipNotifySources             = std::move(Notify);
			Built->ClipAliases                   = std::move(Aliases);
			NewSet                               = std::move(Built);
		}
		if (NewSet.get() == Runtime.Set.get())
		{
			return false;
		}

		// 클립 번호를 이름으로 다시 찾는다 (자기 클립은 앞쪽 그대로, 덧붙인 클립은 순서가 바뀔 수 있다)
		const auto Remap = [&](int32 Clip) {
			if (Clip < 0 || Clip >= static_cast<int32>(Old.Clips.size()))
			{
				return -1;
			}
			if (Clip < static_cast<int32>(Base.Clips.size()))
			{
				return Clip;
			}
			for (const auto& [Alias, Index] : Old.ClipAliases)
			{
				if (Index == Clip)
				{
					return NewSet->FindClip(Alias);
				}
			}
			return NewSet->FindClip(Old.Clips[static_cast<size_t>(Clip)].Name);
		};
		const int32 OldCurrent = Runtime.CurrentClip;
		Runtime.CurrentClip    = Remap(Runtime.CurrentClip);
		Runtime.PreviousClip   = Remap(Runtime.PreviousClip);
		if (Runtime.CurrentClip != OldCurrent)
		{
			// 진행 중 스테이트는 이전 세트 번호 기준 — 버린다 (End 없이)
			Runtime.Notify.ActiveStates.clear();
			Runtime.Notify.Clip = Runtime.CurrentClip;
		}
		Runtime.Set            = std::move(NewSet);
		Runtime.BuiltSet       = Runtime.Set.get();
		return true;
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

	// 클립 노티파이: 리타기팅 클립은 소스 모델 .emeta, 나머지는 모델 자신
	const std::vector<FAnimNotify>* FindClipNotifies(const FAnimationRuntime& Runtime, int32 Clip)
	{
		if (!Runtime.Set || Clip < 0 || Clip >= static_cast<int32>(Runtime.Set->Clips.size()))
		{
			return nullptr;
		}
		const std::vector<std::shared_ptr<const FModelMetadata>>& Sources = Runtime.Set->ClipNotifySources;
		const FModelMetadata* Metadata = Clip < static_cast<int32>(Sources.size()) && Sources[static_cast<size_t>(Clip)] ? Sources[static_cast<size_t>(Clip)].get()
		                                                                                                              : Runtime.Metadata.get();
		return Metadata != nullptr ? Metadata->FindNotifies(Runtime.Set->Clips[static_cast<size_t>(Clip)].Name) : nullptr;
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
	//   Strip이 있으면 루트 모션을 제자리화하고 OutRootMotion에 Σ 가중치 × 이동량
	bool BuildContributionPose(const FAnimationSet& Set, const std::vector<FAnimClipContribution>& Contributions, std::vector<FNodePose>& Sample,
	                           std::vector<FNodePose>& Out, const FRootMotionContext* Strip = nullptr, FRootMotionDelta* OutRootMotion = nullptr)
	{
		if (Contributions.empty())
		{
			return false;
		}
		bool bFirst = true;
		for (const FAnimClipContribution& Contribution : Contributions)
		{
			SampleClipPose(Set, Contribution.Clip, Contribution.Time, Sample, Strip);
			AnimGraphMath::AddWeightedPose(Out, Sample, Contribution.Weight, bFirst);
			if (Strip != nullptr && OutRootMotion != nullptr)
			{
				RootMotionMath::AddWeighted(*OutRootMotion,
				                            ComputeClipRootMotion(Set, Contribution.Clip, Contribution.PreviousTime, Contribution.Time, Contribution.Delta,
				                                                  Contribution.bWrapped, *Strip),
				                            Contribution.Weight);
			}
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

	// 애니메이션 대상 노드 엔티티에 포즈 기록 (Touched = IK가 바꾼 노드 — 채널이 없어도 기록)
	void WritePose(FScene& Scene, const FAnimationRuntime& Runtime, const std::vector<FNodePose>& Pose, const std::vector<uint8>& Touched)
	{
		const FAnimationSet& Set       = *Runtime.Set;
		const size_t         NodeCount = FMath::Min(Runtime.NodeEntities.size(), Pose.size());
		const FRegistry&     Registry  = Scene.GetRegistry();
		for (size_t Node = 0; Node < NodeCount; ++Node)
		{
			const FEntity NodeEntity = Runtime.NodeEntities[Node];
			const bool    bTouched   = Node < Touched.size() && Touched[Node] != 0;
			if ((!Set.AnimatedNodes[Node] && !bTouched) || !Registry.IsValid(NodeEntity))
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
			Montage.LastStep        = Step;
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

	// 몽타주 한 단계의 루트 모션 (부분 구간 반복이 감기면 (이전 → 구간 끝) 다음 (구간 시작 → 새 시각))
	FRootMotionDelta ComputeMontageRootMotion(const FAnimationSet& Set, const FAnimMontageInstance& Montage, const FRootMotionContext& Context)
	{
		const FMontageStep& Step = Montage.LastStep;
		if (Step.Delta == 0.0f)
		{
			return {};
		}
		const FRootMotionTrack Track = RootMotionMath::MakeTrack(Set, Montage.ClipIndex, Context.Node, Context.bRotation);
		if (!Step.bWrapped)
		{
			return RootMotionMath::ComputeSegment(Track, Step.PreviousTime, Step.NewTime);
		}
		const float Start    = AnimMontageMath::GetStartTime(Montage);
		const float End      = AnimMontageMath::GetEndTime(Montage);
		const bool  bForward = Step.Delta > 0.0f;
		return RootMotionMath::Compose(RootMotionMath::ComputeSegment(Track, Step.PreviousTime, bForward ? End : Start),
		                               RootMotionMath::ComputeSegment(Track, bForward ? Start : End, Step.NewTime));
	}

	// 몽타주 포즈를 시작 순서대로 덮는다 (슬롯 마스크 × 가중치). 원천 포즈가 없었으면 기본 포즈에서 시작
	//   루트 모션: 몽타주 추출이 켜져 있으면 몽타주 포즈를 제자리화하고 Runtime.RootMotion = Lerp(아래, 몽타주, 가중치 × 루트 노드 마스크)
	bool ApplyMontages(FAnimationRuntime& Runtime, const FAnimGraphComponent* Graph, bool bHavePose, const FRootMotionContext& Context)
	{
		static const std::vector<float> FullBody;
		const FAnimationSet&            Set      = *Runtime.Set;
		const bool                      bExtract = Context.ExtractMontages();
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
			SampleClipPose(Set, Montage.ClipIndex, Montage.Time, Runtime.MontageScratch, bExtract ? &Context : nullptr);
			const std::vector<float>* Mask = FindSlotMask(Graph, Montage.Params.Slot);
			AnimGraphMath::BlendMasked(Runtime.PoseScratch, Runtime.MontageScratch, Mask != nullptr ? *Mask : FullBody, Montage.Weight);
			if (bExtract)
			{
				const int32 Node       = GetRootMotionNode(Context, Set, Montage.ClipIndex);
				const float MaskWeight = Mask != nullptr && Node >= 0 && Node < static_cast<int32>(Mask->size()) ? (*Mask)[static_cast<size_t>(Node)] : 1.0f;
				Runtime.RootMotion     = RootMotionMath::Lerp(Runtime.RootMotion, ComputeMontageRootMotion(Set, Montage, Context), Montage.Weight * MaskWeight);
			}
		}
		return bHavePose;
	}

	// 그래프 재생 → OutPose (규칙은 Scene/AnimGraph.h 머리 주석). 기본 레이어 기여가 없으면 false (포즈를 쓰지 않는다)
	//   bSuppressBaseNotifies: 몸 전체 몽타주가 덮는 중 — 기본 레이어 노티파이를 판정하지 않는다
	bool EvaluateGraph(FEntity Entity, FAnimationComponent& Animation, FAnimGraphComponent& Graph, float DeltaSeconds, bool bSuppressBaseNotifies,
	                   const FRootMotionContext& RootMotion, std::vector<FNodePose>& OutPose)
	{
		const FRootMotionContext* Strip = RootMotion.ExtractBase() ? &RootMotion : nullptr;
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
		const bool bHavePose =
			BuildContributionPose(Set, GraphRuntime.Instance.GetContributions(), GraphRuntime.SampleScratch, OutPose, Strip, &Runtime.RootMotion);

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
			FRootMotionDelta LayerRootMotion;
			if (bHavePose && Weight > 0.0f &&
			    BuildContributionPose(Set, Instance.GetContributions(), GraphRuntime.SampleScratch, GraphRuntime.LayerPoseScratch, Strip, &LayerRootMotion))
			{
				AnimGraphMath::BlendMasked(OutPose, GraphRuntime.LayerPoseScratch, GraphRuntime.LayerMasks[Layer], Weight);
				if (Strip != nullptr && !Instance.GetContributions().empty())
				{
					const std::vector<float>& Mask = GraphRuntime.LayerMasks[Layer];
					const int32 Node       = GetRootMotionNode(RootMotion, Set, Instance.GetContributions().front().Clip);
					const float MaskWeight = Node >= 0 && Node < static_cast<int32>(Mask.size()) ? Mask[static_cast<size_t>(Node)] : (Mask.empty() ? 1.0f : 0.0f);
					Runtime.RootMotion     = RootMotionMath::Lerp(Runtime.RootMotion, LayerRootMotion, Weight * MaskWeight);
				}
			}
		}
		return bHavePose;
	}

	// 클립 재생(크로스페이드, 루트 모션) → Runtime.PoseScratch. bSuppressNotifies: 몸 전체 몽타주가 덮는 중
	bool EvaluateClip(FEntity Entity, FAnimationComponent& Animation, float DeltaSeconds, bool bSuppressNotifies, const FRootMotionContext& RootMotion)
	{
		const FRootMotionContext* Strip = RootMotion.ExtractBase() ? &RootMotion : nullptr;
		FAnimationRuntime&   Runtime = Animation.Runtime;
		const FAnimationSet& Set     = *Runtime.Set;
		// ResolveClipChange(로그 가능)는 준비 단계(메인 스레드)에서 했다
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

		// 현재 클립 포즈 (루트 모션 추출이면 제자리화 + 이번 구간 이동량)
		SampleClipPose(Set, Runtime.CurrentClip, Runtime.CurrentTime, Runtime.PoseScratch, Strip);
		if (Strip != nullptr)
		{
			Runtime.RootMotion = ComputeClipRootMotion(Set, Runtime.CurrentClip, PreviousCurrentTime, Runtime.CurrentTime, Delta, bWrapped, RootMotion);
		}

		// 크로스페이드: 이전 클립도 계속 진행시키며 가중 보간 (루트 모션도 같은 가중치)
		if (Runtime.PreviousClip >= 0)
		{
			const FAnimationClip& PreviousClip   = Set.Clips[Runtime.PreviousClip];
			const float           PreviousBefore = Runtime.PreviousTime;
			bool                  bPreviousWrapped = false;
			Runtime.PreviousTime = AnimationMath::AdvanceTime(Runtime.PreviousTime, Delta, PreviousClip.Duration, Animation.bLoop, bPreviousWrapped);
			Runtime.BlendElapsed += Animation.bPlaying ? DeltaSeconds : 0.0f;

			const float Weight = AnimationMath::ComputeCrossfadeWeight(Runtime.BlendElapsed, Runtime.BlendDuration);
			SampleClipPose(Set, Runtime.PreviousClip, Runtime.PreviousTime, Runtime.BlendScratch, Strip);
			AnimationMath::BlendPoses(Runtime.BlendScratch, Runtime.PoseScratch, Weight, Runtime.PoseScratch);
			if (Strip != nullptr)
			{
				const FRootMotionDelta Previous =
					ComputeClipRootMotion(Set, Runtime.PreviousClip, PreviousBefore, Runtime.PreviousTime, Delta, bPreviousWrapped, RootMotion);
				Runtime.RootMotion = RootMotionMath::Lerp(Previous, Runtime.RootMotion, Weight);
			}
			if (Weight >= 1.0f)
			{
				Runtime.PreviousClip = -1;
			}
		}
		return true;
	}

	// ---- IK (규칙은 Scene/AnimIK.h)

	int32 FindNodeIndex(const std::vector<std::string>& Names, const std::string& Name)
	{
		for (size_t Node = 0; Node < Names.size(); ++Node)
		{
			if (Names[Node] == Name)
			{
				return static_cast<int32>(Node);
			}
		}
		return -1;
	}

	using AnimationMath::ComputeModelRotations;

	void ResolveFootIk(const FScene& Scene, const FAnimationRuntime& Animation, FFootIkComponent& FootIk)
	{
		FFootIkRuntime& Runtime = FootIk.Runtime;
		if (Runtime.ResolvedSet == Animation.Set.get() && Runtime.ResolvedFeet == FootIk.FootBones && Runtime.ResolvedPelvis == FootIk.PelvisBone)
		{
			return;
		}
		Runtime.ResolvedSet    = Animation.Set.get();
		Runtime.ResolvedFeet   = FootIk.FootBones;
		Runtime.ResolvedPelvis = FootIk.PelvisBone;
		Runtime.Feet.clear();
		const std::vector<std::string> Names   = GetNodeNames(Scene, Animation);
		const std::vector<int32>&      Parents = Animation.Set->NodeParents;
		for (const std::string& Bone : AnimIKMath::SplitBoneList(FootIk.FootBones))
		{
			FFootIkFoot Foot;
			Foot.End  = FindNodeIndex(Names, Bone);
			Foot.Mid  = Foot.End >= 0 ? Parents[static_cast<size_t>(Foot.End)] : -1;
			Foot.Root = Foot.Mid >= 0 ? Parents[static_cast<size_t>(Foot.Mid)] : -1;
			if (Foot.Root < 0)
			{
				E_LOG(LogAnimation, Warning, "발 IK: 뼈 '{}'를 찾을 수 없거나 부모 두 단계가 없습니다", Bone);
				continue;
			}
			Runtime.Feet.push_back(Foot);
		}
		Runtime.Pelvis = FootIk.PelvisBone.empty() ? -1 : FindNodeIndex(Names, FootIk.PelvisBone);
		if (!FootIk.PelvisBone.empty() && Runtime.Pelvis < 0)
		{
			E_LOG(LogAnimation, Warning, "발 IK: 골반 뼈 '{}'를 찾을 수 없습니다", FootIk.PelvisBone);
		}
	}

	void ApplyFootIk(FScene& Scene, FEntity Entity, FAnimationRuntime& Animation, FFootIkComponent& FootIk, float DeltaSeconds)
	{
		ResolveFootIk(Scene, Animation, FootIk);
		FFootIkRuntime& Runtime = FootIk.Runtime;
		if (Runtime.Feet.empty())
		{
			return;
		}
		const std::vector<int32>& Parents  = Animation.Set->NodeParents;
		std::vector<FNodePose>&   Pose     = Animation.PoseScratch;
		std::vector<FMatrix4x4>&  Matrices = Animation.IkMatrices;
		const FMatrix4x4&         RootWorld = Scene.GetTransform(Entity).WorldMatrix;
		const FMatrix4x4          WorldToModel = RootWorld.GetInverse();
		AnimationMath::ComputeModelMatrices(Pose, Parents, Matrices);

		// 바닥 탐색 위치 (IK 전 발) → World가 다음 게임플레이 틱에 쓴다
		for (FFootIkFoot& Foot : Runtime.Feet)
		{
			Foot.ProbeModelPosition = Matrices[static_cast<size_t>(Foot.End)].GetOrigin();
			Foot.bHasProbePosition  = true;
		}
		Runtime.ProbeAge += DeltaSeconds;
		const bool  bActive = FootIk.bEnabled && FootIk.Weight > 0.0f && Runtime.ProbeAge <= FFootIkRuntime::ProbeMaxAge;
		const float BaseZ   = RootWorld.GetOrigin().Z;
		float       Lowest  = 0.0f;
		bool        bAny    = false;
		for (FFootIkFoot& Foot : Runtime.Feet)
		{
			const bool  bUse   = bActive && Foot.bHit;
			const float Target = bUse ? FMath::Clamp(Foot.HitPoint.Z - BaseZ, -FootIk.MaxAdjust, FootIk.MaxAdjust) * FMath::Clamp(FootIk.Weight, 0.0f, 1.0f) : 0.0f;
			Foot.Offset        = AnimIKMath::SmoothTowards(Foot.Offset, Target, FootIk.InterpSpeed, DeltaSeconds);
			const FVector3 TargetNormal = bUse && FootIk.bAlignToGround ? Foot.HitNormal : FVector3::UpVector;
			const float    Alpha        = AnimIKMath::SmoothTowards(0.0f, 1.0f, FootIk.InterpSpeed, DeltaSeconds);
			Foot.Normal                 = (Foot.Normal + (TargetNormal - Foot.Normal) * Alpha).GetNormalized();
			if (Foot.Normal.IsNearlyZero())
			{
				Foot.Normal = FVector3::UpVector;
			}
			Lowest = FMath::Min(Lowest, Foot.Offset);
			bAny   = bAny || FMath::Abs(Foot.Offset) > 0.01f || FVector3::Dot(Foot.Normal, FVector3::UpVector) < 0.9999f;
		}
		Runtime.PelvisOffset = Runtime.Pelvis >= 0 ? Lowest : 0.0f;
		if (!bAny)
		{
			return; // 평지: 애니메이션 그대로
		}

		// 골반 내리기 (모델 공간 위 방향 × cm → 골반 부모 공간)
		const FVector3 UpModel = WorldToModel.TransformVector(FVector3::UpVector); // 월드 1cm 위 (모델 단위)
		if (Runtime.Pelvis >= 0 && Runtime.PelvisOffset < -0.01f)
		{
			const int32      Parent      = Parents[static_cast<size_t>(Runtime.Pelvis)];
			const FMatrix4x4 ParentInverse = Parent >= 0 ? Matrices[static_cast<size_t>(Parent)].GetInverse() : FMatrix4x4::Identity;
			Pose[static_cast<size_t>(Runtime.Pelvis)].Translation += ParentInverse.TransformVector(UpModel * Runtime.PelvisOffset);
			Animation.IkTouched[static_cast<size_t>(Runtime.Pelvis)] = 1;
			AnimationMath::ComputeModelMatrices(Pose, Parents, Matrices);
		}

		// 발마다 2본 IK → 허벅지/무릎/발 로컬 회전
		std::vector<FQuat>& Rotations = Animation.IkRotations;
		ComputeModelRotations(Pose, Parents, Rotations);
		const FVector3 UpDirection   = UpModel.GetNormalized();
		const float    MaxAlignAngle = FMath::DegreesToRadians(FootIk.MaxAlignAngle);
		for (const FFootIkFoot& Foot : Runtime.Feet)
		{
			const size_t   End  = static_cast<size_t>(Foot.End);
			const size_t   Mid  = static_cast<size_t>(Foot.Mid);
			const size_t   Root = static_cast<size_t>(Foot.Root);
			const FVector3 Target = Foot.ProbeModelPosition + UpModel * Foot.Offset;
			const AnimIKMath::FTwoBoneResult Result =
				AnimIKMath::SolveTwoBone(Matrices[Root].GetOrigin(), Matrices[Mid].GetOrigin(), Matrices[End].GetOrigin(), Target, FootIk.KneeDirection);
			const int32    RootParent = Parents[Root];
			const FQuat    ParentRotation = RootParent >= 0 ? Rotations[static_cast<size_t>(RootParent)] : FQuat::Identity;
			const FQuat    NewRoot = Result.RootDelta * Rotations[Root];
			const FQuat    NewMid  = Result.MidDelta * Result.RootDelta * Rotations[Mid];
			const FVector3 NormalModel = WorldToModel.TransformVector(Foot.Normal).GetNormalized();
			const FQuat    Align = AnimIKMath::ComputeLookAtDelta(UpDirection, NormalModel, MaxAlignAngle, FMath::Clamp(FootIk.Weight, 0.0f, 1.0f));
			const FQuat    NewEnd = Align * Rotations[End]; // 발은 애니메이션 방향 유지 + 바닥 기울기
			Pose[Root].Rotation = (ParentRotation.Inverse() * NewRoot).GetNormalized();
			Pose[Mid].Rotation  = (NewRoot.Inverse() * NewMid).GetNormalized();
			Pose[End].Rotation  = (NewMid.Inverse() * NewEnd).GetNormalized();
			Animation.IkTouched[Root] = 1;
			Animation.IkTouched[Mid]  = 1;
			Animation.IkTouched[End]  = 1;
		}
	}

	void ResolveLookAt(const FScene& Scene, const FAnimationRuntime& Animation, FLookAtComponent& LookAt)
	{
		FLookAtRuntime& Runtime = LookAt.Runtime;
		if (Runtime.ResolvedSet == Animation.Set.get() && Runtime.ResolvedBones == LookAt.Bones && Runtime.ResolvedForwardAxis == LookAt.ForwardAxis)
		{
			return;
		}
		Runtime.ResolvedSet         = Animation.Set.get();
		Runtime.ResolvedBones       = LookAt.Bones;
		Runtime.ResolvedForwardAxis = LookAt.ForwardAxis;
		Runtime.Bones.clear();
		const std::vector<std::string> Names = GetNodeNames(Scene, Animation);
		for (const std::string& Bone : AnimIKMath::SplitBoneList(LookAt.Bones))
		{
			const int32 Node = FindNodeIndex(Names, Bone);
			if (Node < 0)
			{
				E_LOG(LogAnimation, Warning, "시선: 뼈 '{}'를 찾을 수 없습니다", Bone);
				continue;
			}
			Runtime.Bones.push_back(Node);
		}
		if (Runtime.Bones.empty())
		{
			return;
		}
		// 마지막 뼈 로컬 앞 축 = 기본 포즈에서 모델 공간 ForwardAxis
		std::vector<FQuat> RestRotations;
		ComputeModelRotations(Animation.Set->RestPose, Animation.Set->NodeParents, RestRotations);
		const FVector3 Forward = LookAt.ForwardAxis.IsNearlyZero() ? FVector3::ForwardVector : LookAt.ForwardAxis.GetNormalized();
		Runtime.LocalForward   = RestRotations[static_cast<size_t>(Runtime.Bones.back())].UnrotateVector(Forward);
	}

	void ApplyLookAt(FScene& Scene, FEntity Entity, FAnimationRuntime& Animation, FLookAtComponent& LookAt, float DeltaSeconds)
	{
		ResolveLookAt(Scene, Animation, LookAt);
		FLookAtRuntime& Runtime = LookAt.Runtime;
		if (Runtime.Bones.empty())
		{
			return;
		}
		// 목표: Lua 점/엔티티 → Target 엔티티
		const FRegistry& Registry = Scene.GetRegistry();
		bool             bTarget  = false;
		FVector3         Target;
		if (Runtime.bHasScriptTarget)
		{
			bTarget = true;
			Target  = Registry.IsValid(Runtime.ScriptTargetEntity) ? Scene.GetTransform(Runtime.ScriptTargetEntity).GetWorldPosition() : Runtime.ScriptTarget;
		}
		else if (Registry.IsValid(LookAt.Target))
		{
			bTarget = true;
			Target  = Scene.GetTransform(LookAt.Target).GetWorldPosition();
		}
		if (bTarget)
		{
			Runtime.LastTarget     = Target;
			Runtime.bHasLastTarget = true;
		}
		const float Desired   = LookAt.bEnabled && bTarget ? FMath::Clamp(LookAt.Weight, 0.0f, 1.0f) : 0.0f;
		Runtime.CurrentWeight = AnimIKMath::SmoothTowards(Runtime.CurrentWeight, Desired, LookAt.BlendSpeed, DeltaSeconds);
		if (Runtime.CurrentWeight < 1.0e-3f || !Runtime.bHasLastTarget)
		{
			return;
		}

		const std::vector<int32>& Parents  = Animation.Set->NodeParents;
		std::vector<FNodePose>&   Pose     = Animation.PoseScratch;
		std::vector<FMatrix4x4>&  Matrices = Animation.IkMatrices;
		std::vector<FQuat>&       Rotations = Animation.IkRotations;
		AnimationMath::ComputeModelMatrices(Pose, Parents, Matrices);
		ComputeModelRotations(Pose, Parents, Rotations);
		const size_t   Head        = static_cast<size_t>(Runtime.Bones.back());
		const FVector3 TargetModel = Scene.GetTransform(Entity).WorldMatrix.GetInverse().TransformPosition(Runtime.LastTarget);
		const FVector3 Forward     = Rotations[Head].RotateVector(Runtime.LocalForward);
		const FQuat    Delta = AnimIKMath::ComputeLookAtDelta(Forward, TargetModel - Matrices[Head].GetOrigin(), FMath::DegreesToRadians(LookAt.MaxAngle),
		                                                      Runtime.CurrentWeight);
		// 뼈 개수로 나눠 위 뼈부터 (모델 공간 — 아래 뼈들도 함께 돈다)
		const FQuat Part = FQuat::Slerp(FQuat::Identity, Delta, 1.0f / static_cast<float>(Runtime.Bones.size()));
		for (const int32 Bone : Runtime.Bones)
		{
			const int32 Parent         = Parents[static_cast<size_t>(Bone)];
			const FQuat ParentRotation = Parent >= 0 ? Rotations[static_cast<size_t>(Parent)] : FQuat::Identity;
			Pose[static_cast<size_t>(Bone)].Rotation = (ParentRotation.Inverse() * Part * Rotations[static_cast<size_t>(Bone)]).GetNormalized();
			Animation.IkTouched[static_cast<size_t>(Bone)] = 1;
			ComputeModelRotations(Pose, Parents, Rotations);
		}
	}

	// IK 단계 (발 → 시선). 래그돌 중에는 호출되지 않는다
	void ApplyIk(FScene& Scene, FEntity Entity, FAnimationRuntime& Animation, float DeltaSeconds)
	{
		FRegistry& Registry = Scene.GetRegistry();
		// 한 번 IK가 바꾼 노드는 계속 기록한다 (IK가 꺼져도 채널 없는 노드가 IK 자세로 남지 않게 — 기본 포즈로 돌아간다)
		if (Animation.IkTouched.size() != Animation.PoseScratch.size())
		{
			Animation.IkTouched.assign(Animation.PoseScratch.size(), 0);
		}
		if (Animation.PoseScratch.size() != Animation.Set->NodeParents.size())
		{
			return;
		}
		if (FFootIkComponent* FootIk = Registry.TryGet<FFootIkComponent>(Entity))
		{
			ApplyFootIk(Scene, Entity, Animation, *FootIk, DeltaSeconds);
		}
		if (FLookAtComponent* LookAt = Registry.TryGet<FLookAtComponent>(Entity))
		{
			ApplyLookAt(Scene, Entity, Animation, *LookAt, DeltaSeconds);
		}
	}

	// ---- 갱신 단계 (FAnimationSystem::Update): 준비(메인, 엔티티 순서) → 포즈 평가(병렬) → 마무리(메인, 엔티티 순서)

	struct FAnimViewPoint
	{
		bool     bValid = false;
		FVector3 Position;
		float    FovYRadians   = 1.0f;
		bool     bOrthographic = false;
		float    OrthoHeight   = 1000.0f;
	};

	struct FAnimUpdateItem
	{
		FEntity              Entity;
		FAnimationComponent* Animation    = nullptr;
		FAnimGraphComponent* Graph        = nullptr;
		float                DeltaSeconds = 0.0f; // 이번 평가가 진행할 시간 (건너뛰며 모은 시간 포함)
		bool                 bHavePose    = false;
	};

	FAnimViewPoint FindViewPoint(FScene& Scene)
	{
		FAnimViewPoint          View;
		const FEntity           Camera    = FCameraProjection::FindActiveCamera(Scene);
		const FCameraComponent* Component = Camera.IsValid() ? Scene.GetRegistry().TryGet<FCameraComponent>(Camera) : nullptr;
		if (Component == nullptr)
		{
			return View;
		}
		View.bValid        = true;
		View.Position      = Scene.GetTransform(Camera).WorldMatrix.GetOrigin();
		View.FovYRadians   = FMath::DegreesToRadians(Component->FovYDegrees);
		View.bOrthographic = Component->bOrthographic;
		View.OrthoHeight   = Component->OrthoHeight;
		return View;
	}

	// 이번 프레임 평가 간격 (1 = 매 프레임). 제외 규칙은 Scene/AnimUpdateRate.h
	uint32 SelectUpdateInterval(FScene& Scene, FEntity Entity, const FAnimationComponent& Animation, const FAnimViewPoint& View)
	{
		const bool bRootMotion = Animation.RootMotionMode != ERootMotionMode::None || Animation.bRootMotion;
		if (!View.bValid || !CVarUro.Get() || !Animation.bPlaying || bRootMotion)
		{
			return 1;
		}
		const FMatrix4x4& World = Scene.GetTransform(Entity).WorldMatrix;
		const float       Scale = FMath::Max(World.TransformVector(FVector3(1.0f, 0.0f, 0.0f)).Length(),
		                                     FMath::Max(World.TransformVector(FVector3(0.0f, 1.0f, 0.0f)).Length(),
		                                                World.TransformVector(FVector3(0.0f, 0.0f, 1.0f)).Length()));
		const float Distance   = (World.GetOrigin() - View.Position).Length();
		const float ScreenSize = AnimUpdateRateMath::ComputeScreenSize(CVarUroRadius.Get() * Scale, Distance, View.FovYRadians, View.bOrthographic,
		                                                               View.OrthoHeight);
		AnimUpdateRateMath::FSettings Settings;
		Settings.FullRateScreenSize = CVarUroFull.Get();
		Settings.HalfRateScreenSize = CVarUroHalf.Get();
		Settings.MaxInterval        = static_cast<uint32>(FMath::Max(CVarUroMaxInterval.Get(), 1));
		return AnimUpdateRateMath::SelectInterval(ScreenSize, Settings);
	}

	// 준비 (메인 스레드): 세트/그래프 해석, 래그돌, 갱신 빈도, 클립 변경·IK 뼈 해석 (로그·라이브러리 접근은 여기서만). 평가할 항목이면 true
	bool PrepareAnimation(FScene& Scene, FEntity Entity, FAnimationComponent& Animation, float DeltaSeconds, const FAnimViewPoint& View,
	                      FAnimUpdateItem& OutItem)
	{
		FAnimationRuntime& Runtime = Animation.Runtime;
		if (!Runtime.Set)
		{
			return false;
		}
		Runtime.RootMotion = {};
		// 리타기팅 소스(컴포넌트 목록 / "<모델>:<클립>" 이름)가 바뀌면 세트를 다시 만든다 — 그래프를 묶기 전에 (없는 클립 경고 방지)
		FAnimGraphComponent* Graph = Scene.GetRegistry().TryGet<FAnimGraphComponent>(Entity);
		ResolveRetargeting(Scene, Entity, Animation, Graph);
		if (Graph != nullptr && !ResolveGraph(Scene, *Graph, Runtime))
		{
			Graph = nullptr;
		}
		if (Runtime.Set->Clips.empty())
		{
			return false;
		}
		Runtime.PendingNotifies.clear();
		Runtime.PendingMontageEvents.clear();
		if (Runtime.bPhysicsPose)
		{
			// 래그돌: 물리가 뼈 트랜스폼을 쓴다 (재생 시간·노티파이·몽타주·IK도 멈춘다). 끝났을 때 튀지 않게 IK 보정은 0으로
			if (FFootIkComponent* FootIk = Scene.GetRegistry().TryGet<FFootIkComponent>(Entity))
			{
				FootIk->Runtime.ResetBlend();
			}
			if (FLookAtComponent* LookAt = Scene.GetRegistry().TryGet<FLookAtComponent>(Entity))
			{
				LookAt->Runtime.CurrentWeight = 0.0f;
			}
			Runtime.UpdateRatePending = 0.0f;
			return false;
		}

		// 갱신 빈도 LOD: 건너뛰는 프레임은 시간만 모은다
		const uint32 Interval      = SelectUpdateInterval(Scene, Entity, Animation, View);
		const uint32 Tick          = Runtime.UpdateRateTick++;
		Runtime.UpdateRateInterval = static_cast<uint8>(FMath::Min<uint32>(Interval, 255u));
		if (!AnimUpdateRateMath::ShouldEvaluate(Tick, Entity.Index, Interval, !Runtime.bUpdateRateEvaluated))
		{
			Runtime.UpdateRatePending += DeltaSeconds;
			return false;
		}
		Runtime.bUpdateRateEvaluated = true;
		OutItem.DeltaSeconds         = DeltaSeconds + Runtime.UpdateRatePending;
		Runtime.UpdateRatePending    = 0.0f;

		if (Graph == nullptr)
		{
			ResolveClipChange(Animation);
		}
		if (FFootIkComponent* FootIk = Scene.GetRegistry().TryGet<FFootIkComponent>(Entity))
		{
			ResolveFootIk(Scene, Runtime, *FootIk);
		}
		if (FLookAtComponent* LookAt = Scene.GetRegistry().TryGet<FLookAtComponent>(Entity))
		{
			ResolveLookAt(Scene, Runtime, *LookAt);
		}
		OutItem.Entity    = Entity;
		OutItem.Animation = &Animation;
		OutItem.Graph     = Graph;
		return true;
	}

	// 포즈 평가 (작업 스레드 가능): 원천(그래프 | 클립) → (몽타주) → (IK) → 이 모델의 노드 엔티티 로컬 트랜스폼에 기록.
	//   이 런타임·이 모델 노드만 쓰고 다른 엔티티는 읽기만 한다 (월드 행렬 — 이 단계에서 아무도 쓰지 않음). ECS 구조 변경·로그·콜백 없음
	void EvaluateAnimation(FScene& Scene, FAnimUpdateItem& Item)
	{
		FAnimationComponent& Animation    = *Item.Animation;
		FAnimationRuntime&   Runtime      = Animation.Runtime;
		const FEntity        Entity       = Item.Entity;
		const float          DeltaSeconds = Item.DeltaSeconds;
		// 몽타주 진행을 먼저 (몸 전체 몽타주가 덮는 중이면 아래 원천의 노티파이를 멈춘다)
		const float              Delta      = Animation.bPlaying ? DeltaSeconds * Animation.Speed : 0.0f;
		const bool               bSuppress  = !Runtime.Montages.empty() && UpdateMontages(Runtime, Entity, Item.Graph, Delta, DeltaSeconds);
		const FRootMotionContext RootMotion = MakeRootMotionContext(Animation, *Runtime.Set);

		bool bHavePose = Item.Graph != nullptr ? EvaluateGraph(Entity, Animation, *Item.Graph, DeltaSeconds, bSuppress, RootMotion, Runtime.PoseScratch)
		                                       : EvaluateClip(Entity, Animation, DeltaSeconds, bSuppress, RootMotion);
		bHavePose = ApplyMontages(Runtime, Item.Graph, bHavePose, RootMotion);
		if (bHavePose)
		{
			ApplyIk(Scene, Entity, Runtime, DeltaSeconds);
			WritePose(Scene, Runtime, Runtime.PoseScratch, Runtime.IkTouched);
		}
		else
		{
			Runtime.RootMotion = {};
		}
		Item.bHavePose = bHavePose;
	}

	// 마무리 (메인 스레드, 엔티티 순서): 루트 모션 적용 (수신자 콜백 → 캐릭터 이동, 없으면 엔티티 로컬 트랜스폼)
	void FinishAnimation(FScene& Scene, const FAnimUpdateItem& Item)
	{
		const FAnimationRuntime& Runtime = Item.Animation->Runtime;
		if (Item.bHavePose && !Runtime.RootMotion.IsZero())
		{
			ApplyRootMotion(Scene, Item.Entity, Runtime.RootMotion, Item.DeltaSeconds);
		}
	}

	// 클립 이름 찾기. "<모델>:<클립>"인데 아직 없으면 그 모델을 리타기팅 소스로 요청하고 세트를 다시 만든 뒤 찾는다
	int32 FindOrRequestClip(FScene& Scene, FEntity Entity, FAnimationComponent& Animation, std::string_view ClipName)
	{
		FAnimationRuntime& Runtime = Animation.Runtime;
		if (!Runtime.Set)
		{
			return -1;
		}
		if (const int32 Found = Runtime.Set->FindClip(ClipName); Found >= 0)
		{
			return Found;
		}
		std::string Model;
		std::string Clip;
		if (!FAnimRetargetLibrary::SplitQualifiedName(ClipName, Model, Clip))
		{
			return -1;
		}
		AddUniqueSource(Runtime.RequestedSources, Model);
		const FAnimGraphComponent* Graph = Scene.GetRegistry().TryGet<FAnimGraphComponent>(Entity);
		ResolveRetargeting(Scene, Entity, Animation, Graph);
		return Runtime.Set->FindClip(ClipName);
	}
} // namespace

void FAnimationSystem::SetRootMotionReceiver(FRootMotionReceiver Receiver)
{
	GRootMotionReceiver = Receiver;
}

FRootMotionDelta FAnimationSystem::GetLastRootMotion(FScene& Scene, FEntity Entity)
{
	const FEntity Target = FindAnimation(Scene, Entity);
	return Target.IsValid() ? Scene.GetRegistry().Get<FAnimationComponent>(Target).Runtime.RootMotion : FRootMotionDelta{};
}

bool FAnimationSystem::RefreshRetargeting(FScene& Scene, FEntity Entity)
{
	FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity);
	if (Animation == nullptr)
	{
		return false;
	}
	FAnimGraphComponent* Graph    = Scene.GetRegistry().TryGet<FAnimGraphComponent>(Entity);
	const bool           bChanged = ResolveRetargeting(Scene, Entity, *Animation, Graph);
	if (bChanged && Graph != nullptr)
	{
		ResolveGraph(Scene, *Graph, Animation->Runtime);
	}
	return bChanged;
}

// 준비(메인, 엔티티 순서) → 포즈 평가(FParallel, 엔티티별 독립) → 마무리(메인, 엔티티 순서).
// 노티파이·몽타주 끝 이벤트는 런타임별 Pending 목록에 쌓이므로 평가 순서와 무관하고, 루트 모션 콜백은 마무리에서 엔티티 순서대로 부른다
// (평가 중에는 노드 로컬 트랜스폼만 바뀌고 월드 행렬은 다음 UpdateTransforms까지 그대로 — 순차 실행과 결과가 같다)
void FAnimationSystem::Update(FScene& Scene, float DeltaSeconds, std::vector<FTransformChangedSubtree>* OutWritten)
{
	E_PROFILE_SCOPE("애니메이션");
	const FAnimViewPoint         View = FindViewPoint(Scene);
	std::vector<FAnimUpdateItem> Items;
	{
		E_PROFILE_SCOPE("애니메이션 준비");
		Scene.GetRegistry().View<FAnimationComponent>().Each([&](FEntity Entity, FAnimationComponent& Animation) {
			FAnimUpdateItem Item;
			if (PrepareAnimation(Scene, Entity, Animation, DeltaSeconds, View, Item))
			{
				Items.push_back(Item);
			}
		});
	}
	{
		E_PROFILE_SCOPE("애니메이션 평가");
		const auto Evaluate = [&](uint32 Begin, uint32 End) {
			for (uint32 Index = Begin; Index < End; ++Index)
			{
				EvaluateAnimation(Scene, Items[Index]);
			}
		};
		if (CVarParallel.Get())
		{
			FParallel::ParallelFor(static_cast<uint32>(Items.size()), 4, Evaluate);
		}
		else
		{
			Evaluate(0, static_cast<uint32>(Items.size()));
		}
	}
	for (const FAnimUpdateItem& Item : Items)
	{
		FinishAnimation(Scene, Item);
		if (OutWritten != nullptr)
		{
			// 평가 단계는 이 모델 노드만(포즈·IK), 마무리는 루트만(루트 모션 — 수신자가 있으면 캐릭터 이동에 쌓기만) 쓴다
			OutWritten->push_back({ Item.Entity, Item.Animation->Runtime.NodeEntities });
		}
	}
}

bool FAnimationSystem::Play(FScene& Scene, FEntity Entity, std::string_view ClipName, float BlendTime)
{
	FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity);
	if (Animation == nullptr || FindOrRequestClip(Scene, Entity, *Animation, ClipName) < 0)
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
		// 리타기팅 클립이 앞 클립과 이름이 같으면 "<모델>:<클립>" 별칭으로 (그 이름으로 Play하면 그 클립)
		const FAnimationSet& Set = *Animation->Runtime.Set;
		for (size_t Index = 0; Index < Set.Clips.size(); ++Index)
		{
			std::string Name = Set.Clips[Index].Name;
			if (Set.FindClip(Name) != static_cast<int32>(Index))
			{
				for (const auto& [Alias, AliasIndex] : Set.ClipAliases)
				{
					if (AliasIndex == static_cast<int32>(Index))
					{
						Name = Alias;
						break;
					}
				}
			}
			Names.push_back(std::move(Name));
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
	FAnimationComponent& Animation = Scene.GetRegistry().Get<FAnimationComponent>(Target);
	FAnimationRuntime&   Runtime   = Animation.Runtime;
	const int32          Clip      = FindOrRequestClip(Scene, Target, Animation, ClipName);
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

// ---------------------------------------------------------------- 시선 IK

namespace
{
	FLookAtRuntime* FindLookAtRuntime(FScene& Scene, FEntity Entity)
	{
		const FEntity    Target   = FAnimationSystem::FindAnimation(Scene, Entity);
		FLookAtComponent* LookAt  = Target.IsValid() ? Scene.GetRegistry().TryGet<FLookAtComponent>(Target) : nullptr;
		return LookAt != nullptr ? &LookAt->Runtime : nullptr;
	}
} // namespace

bool FAnimationSystem::SetLookAtTarget(FScene& Scene, FEntity Entity, const FVector3& WorldPosition)
{
	FLookAtRuntime* Runtime = FindLookAtRuntime(Scene, Entity);
	if (Runtime == nullptr)
	{
		return false;
	}
	Runtime->bHasScriptTarget   = true;
	Runtime->ScriptTarget       = WorldPosition;
	Runtime->ScriptTargetEntity = NullEntity;
	return true;
}

bool FAnimationSystem::SetLookAtTargetEntity(FScene& Scene, FEntity Entity, FEntity Target)
{
	FLookAtRuntime* Runtime = FindLookAtRuntime(Scene, Entity);
	if (Runtime == nullptr || !Scene.GetRegistry().IsValid(Target))
	{
		return false;
	}
	Runtime->bHasScriptTarget   = true;
	Runtime->ScriptTargetEntity = Target;
	Runtime->ScriptTarget       = Scene.GetTransform(Target).GetWorldPosition();
	return true;
}

bool FAnimationSystem::ClearLookAtTarget(FScene& Scene, FEntity Entity)
{
	FLookAtRuntime* Runtime = FindLookAtRuntime(Scene, Entity);
	if (Runtime == nullptr)
	{
		return false;
	}
	Runtime->bHasScriptTarget   = false;
	Runtime->ScriptTargetEntity = NullEntity;
	return true;
}
