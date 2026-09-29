#include "Scene/AnimationSystem.h"

#include "Core/Log.h"
#include "Scene/Components.h"
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

	void UpdateAnimation(FScene& Scene, FEntity Entity, FAnimationComponent& Animation, float DeltaSeconds)
	{
		FAnimationRuntime& Runtime = Animation.Runtime;
		if (!Runtime.Set || Runtime.Set->Clips.empty())
		{
			return;
		}
		const FAnimationSet& Set = *Runtime.Set;
		ResolveClipChange(Animation);

		const FAnimationClip& Clip  = Set.Clips[Runtime.CurrentClip];
		const float           Delta = Animation.bPlaying ? DeltaSeconds * Animation.Speed : 0.0f;

		const float PreviousCurrentTime = Runtime.CurrentTime;
		bool        bWrapped            = false;
		Runtime.CurrentTime = AnimationMath::AdvanceTime(Runtime.CurrentTime, Delta, Clip.Duration, Animation.bLoop, bWrapped);

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

		// 애니메이션 대상 노드 엔티티에 기록
		const size_t    NodeCount = FMath::Min(Runtime.NodeEntities.size(), Runtime.PoseScratch.size());
		const FRegistry& Registry = Scene.GetRegistry();
		for (size_t Node = 0; Node < NodeCount; ++Node)
		{
			const FEntity NodeEntity = Runtime.NodeEntities[Node];
			if (!Set.AnimatedNodes[Node] || !Registry.IsValid(NodeEntity))
			{
				continue;
			}
			const FNodePose&     Pose      = Runtime.PoseScratch[Node];
			FTransformComponent& Transform = Scene.GetTransform(NodeEntity);
			Transform.Position = Pose.Translation;
			Transform.Rotation = Pose.Rotation;
			Transform.Scale    = Pose.Scale;
		}
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
