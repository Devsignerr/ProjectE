#include "Scene/AnimRootMotion.h"

#include <cmath>

namespace
{
	const FAnimationChannel* FindChannel(const FAnimationClip& Clip, int32 Node, EAnimationPath Path)
	{
		for (const FAnimationChannel& Channel : Clip.Channels)
		{
			if (Channel.Node == Node && Channel.Path == Path)
			{
				return &Channel;
			}
		}
		return nullptr;
	}

	// 시각 Time의 루트 노드 로컬 포즈 (채널이 없는 속성은 기본 포즈)
	FNodePose SampleLocal(const FRootMotionTrack& Track, float Time)
	{
		FNodePose Pose = Track.Rest;
		if (Track.Translation != nullptr)
		{
			const FVector4 Value = AnimationMath::SampleChannel(*Track.Translation, Time);
			Pose.Translation     = FVector3(Value.X, Value.Y, Value.Z);
		}
		if (Track.Rotation != nullptr)
		{
			const FVector4 Value = AnimationMath::SampleChannel(*Track.Rotation, Time);
			Pose.Rotation        = FQuat(Value.X, Value.Y, Value.Z, Value.W).GetNormalized();
		}
		return Pose;
	}
} // namespace

namespace RootMotionMath
{
	float NormalizeAngle(float Radians)
	{
		if (!std::isfinite(Radians))
		{
			return 0.0f;
		}
		float Wrapped = std::fmod(Radians + FMath::Pi, FMath::TwoPi);
		if (Wrapped < 0.0f)
		{
			Wrapped += FMath::TwoPi;
		}
		return Wrapped - FMath::Pi;
	}

	float ExtractYaw(const FQuat& Rotation)
	{
		// twist = (0, 0, z, w) 정규화 → 각도 2·atan2(z, w)
		if (FMath::Abs(Rotation.Z) <= 1.0e-8f && FMath::Abs(Rotation.W) <= 1.0e-8f)
		{
			return 0.0f; // Z축과 수직인 축으로 180° — twist 정의 불가
		}
		return NormalizeAngle(2.0f * std::atan2(Rotation.Z, Rotation.W));
	}

	FQuat MakeYawRotation(float Radians) { return FQuat::FromAxisAngle(FVector3::UpVector, Radians); }

	FRootMotionTrack MakeTrack(const FAnimationSet& Set, int32 Clip, int32 Node, bool bRotation)
	{
		FRootMotionTrack Track;
		if (Clip < 0 || Clip >= static_cast<int32>(Set.Clips.size()))
		{
			return Track;
		}
		if (Node < 0 && Clip < static_cast<int32>(Set.ClipRootMotionNodes.size()))
		{
			Node = Set.ClipRootMotionNodes[static_cast<size_t>(Clip)];
		}
		if (Node < 0 || Node >= static_cast<int32>(Set.NodeParents.size()))
		{
			return Track;
		}
		const FAnimationClip& Data = Set.Clips[static_cast<size_t>(Clip)];
		Track.Node                 = Node;
		Track.bRotation            = bRotation;
		Track.Translation          = FindChannel(Data, Node, EAnimationPath::Translation);
		Track.Rotation             = FindChannel(Data, Node, EAnimationPath::Rotation);
		Track.Rest                 = Node < static_cast<int32>(Set.RestPose.size()) ? Set.RestPose[static_cast<size_t>(Node)] : FNodePose{};

		const int32 Parent = Set.NodeParents[static_cast<size_t>(Node)];
		if (Parent >= 0 && Parent < static_cast<int32>(Set.RestModelMatrices.size()))
		{
			Track.ParentToModel = Set.RestModelMatrices[static_cast<size_t>(Parent)];
			// 부모 모델 회전 = 조상 기본 회전의 곱 (균등 스케일 가정)
			FQuat Rotation = FQuat::Identity;
			for (int32 Current = Parent, Guard = 0; Current >= 0 && Guard <= static_cast<int32>(Set.NodeParents.size()); Current = Set.NodeParents[static_cast<size_t>(Current)], ++Guard)
			{
				Rotation = (Set.RestPose[static_cast<size_t>(Current)].Rotation * Rotation).GetNormalized();
			}
			Track.ParentRotation = Rotation;
		}
		const FNodePose Start = SampleLocal(Track, 0.0f);
		Track.StartPosition   = Track.ParentToModel.TransformPosition(Start.Translation);
		Track.StartRotation   = (Track.ParentRotation * Start.Rotation).GetNormalized();
		return Track;
	}

	void SampleModel(const FRootMotionTrack& Track, float Time, FVector3& OutPosition, float& OutYaw)
	{
		const FNodePose Local = SampleLocal(Track, Time);
		OutPosition           = Track.ParentToModel.TransformPosition(Local.Translation);
		OutYaw                = 0.0f;
		if (Track.bRotation && Track.Rotation != nullptr)
		{
			const FQuat Model = (Track.ParentRotation * Local.Rotation).GetNormalized();
			OutYaw            = ExtractYaw(Model * Track.StartRotation.Inverse());
		}
	}

	FRootMotionDelta ComputeSegment(const FRootMotionTrack& Track, float From, float To)
	{
		FRootMotionDelta Result;
		if (!Track.IsValid())
		{
			return Result;
		}
		FVector3 FromPosition;
		FVector3 ToPosition;
		float    FromYaw = 0.0f;
		float    ToYaw   = 0.0f;
		SampleModel(Track, From, FromPosition, FromYaw);
		SampleModel(Track, To, ToPosition, ToYaw);
		Result.Translation   = MakeYawRotation(-FromYaw).RotateVector(ToPosition - FromPosition);
		Result.Translation.Z = 0.0f;
		Result.Yaw           = NormalizeAngle(ToYaw - FromYaw);
		return Result;
	}

	FRootMotionDelta ComputeDelta(const FRootMotionTrack& Track, float PreviousTime, float NewTime, float Delta, bool bWrapped, float Duration)
	{
		if (!Track.IsValid() || Delta == 0.0f)
		{
			return {};
		}
		if (!bWrapped)
		{
			return ComputeSegment(Track, PreviousTime, NewTime);
		}
		const bool bForward = Delta > 0.0f;
		return Compose(ComputeSegment(Track, PreviousTime, bForward ? Duration : 0.0f), ComputeSegment(Track, bForward ? 0.0f : Duration, NewTime));
	}

	FRootMotionDelta Compose(const FRootMotionDelta& First, const FRootMotionDelta& Then)
	{
		FRootMotionDelta Result;
		Result.Translation = First.Translation + MakeYawRotation(First.Yaw).RotateVector(Then.Translation);
		Result.Yaw         = First.Yaw + Then.Yaw;
		return Result;
	}

	FRootMotionDelta Lerp(const FRootMotionDelta& A, const FRootMotionDelta& B, float Alpha)
	{
		FRootMotionDelta Result;
		Result.Translation = A.Translation + (B.Translation - A.Translation) * Alpha;
		Result.Yaw         = A.Yaw + (B.Yaw - A.Yaw) * Alpha;
		return Result;
	}

	void AddWeighted(FRootMotionDelta& Accumulator, const FRootMotionDelta& Delta, float Weight)
	{
		Accumulator.Translation = Accumulator.Translation + Delta.Translation * Weight;
		Accumulator.Yaw += Delta.Yaw * Weight;
	}

	void RemoveFromPose(const FRootMotionTrack& Track, float Time, FNodePose& InOutRootPose)
	{
		if (!Track.IsValid())
		{
			return;
		}
		FVector3 Position = Track.ParentToModel.TransformPosition(InOutRootPose.Translation);
		Position.X        = Track.StartPosition.X;
		Position.Y        = Track.StartPosition.Y;
		if (Track.bRotation && Track.Rotation != nullptr)
		{
			FVector3    Ignored;
			float       Yaw   = 0.0f;
			SampleModel(Track, Time, Ignored, Yaw);
			const FQuat Model = (Track.ParentRotation * InOutRootPose.Rotation).GetNormalized();
			InOutRootPose.Rotation = (Track.ParentRotation.Inverse() * MakeYawRotation(-Yaw) * Model).GetNormalized();
		}
		InOutRootPose.Translation = Track.ParentToModel.GetInverse().TransformPosition(Position);
	}

	void ApplyToTransform(const FRootMotionDelta& Delta, FVector3& InOutPosition, FQuat& InOutRotation, const FVector3& Scale)
	{
		InOutPosition = InOutPosition + InOutRotation.RotateVector(Delta.Translation * Scale);
		if (Delta.Yaw != 0.0f)
		{
			InOutRotation = (InOutRotation * MakeYawRotation(Delta.Yaw)).GetNormalized();
		}
	}
} // namespace RootMotionMath
