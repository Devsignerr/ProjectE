#include "Scene/Animation.h"

#include <algorithm>
#include <cmath>

int32 FAnimationSet::FindClip(std::string_view Name) const
{
	for (size_t Index = 0; Index < Clips.size(); ++Index)
	{
		if (Clips[Index].Name == Name)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

std::shared_ptr<const FAnimationSet> MakeAnimationSet(std::vector<FAnimationClip> Clips, std::vector<int32> NodeParents,
                                                      std::vector<FNodePose> RestPose)
{
	auto Set         = std::make_shared<FAnimationSet>();
	Set->Clips       = std::move(Clips);
	Set->NodeParents = std::move(NodeParents);
	Set->RestPose    = std::move(RestPose);
	Set->RestPose.resize(Set->NodeParents.size());

	const size_t NodeCount = Set->NodeParents.size();
	Set->AnimatedNodes.assign(NodeCount, 0);
	std::vector<uint8> HasTranslation(NodeCount, 0);
	for (const FAnimationClip& Clip : Set->Clips)
	{
		for (const FAnimationChannel& Channel : Clip.Channels)
		{
			if (Channel.Node >= 0 && static_cast<size_t>(Channel.Node) < NodeCount)
			{
				Set->AnimatedNodes[Channel.Node] = 1;
				if (Channel.Path == EAnimationPath::Translation)
				{
					HasTranslation[Channel.Node] = 1;
				}
			}
		}
	}

	// 루트 모션 노드: 조상에 이동 채널이 없는 이동 애니메이션 노드 중 가장 얕은 것
	int32 BestDepth = -1;
	for (size_t Node = 0; Node < NodeCount; ++Node)
	{
		if (!HasTranslation[Node])
		{
			continue;
		}
		bool  bAncestorAnimated = false;
		int32 Depth             = 0;
		for (int32 Parent = Set->NodeParents[Node]; Parent >= 0 && Depth <= static_cast<int32>(NodeCount); Parent = Set->NodeParents[Parent])
		{
			bAncestorAnimated |= HasTranslation[Parent] != 0;
			++Depth;
		}
		if (!bAncestorAnimated && (BestDepth < 0 || Depth < BestDepth))
		{
			BestDepth           = Depth;
			Set->RootMotionNode = static_cast<int32>(Node);
		}
	}
	if (Set->RootMotionNode >= 0)
	{
		std::vector<FMatrix4x4> RestMatrices;
		AnimationMath::ComputeModelMatrices(Set->RestPose, Set->NodeParents, RestMatrices);
		const int32 Parent            = Set->NodeParents[Set->RootMotionNode];
		Set->RootMotionParentToModel = Parent >= 0 ? RestMatrices[Parent] : FMatrix4x4::Identity;
	}
	return Set;
}

namespace AnimationMath
{
	FVector4 SampleChannel(const FAnimationChannel& Channel, float Time)
	{
		const size_t Count = FMath::Min(Channel.Times.size(), Channel.Values.size());
		if (Count == 0)
		{
			return Channel.Path == EAnimationPath::Rotation ? FVector4(0.0f, 0.0f, 0.0f, 1.0f) : FVector4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		if (Count == 1 || Time <= Channel.Times[0])
		{
			return Channel.Values[0];
		}
		if (Time >= Channel.Times[Count - 1])
		{
			return Channel.Values[Count - 1];
		}

		// Times[Next - 1] <= Time < Times[Next]
		const auto   Upper = std::upper_bound(Channel.Times.begin(), Channel.Times.begin() + static_cast<std::ptrdiff_t>(Count), Time);
		const size_t Next  = static_cast<size_t>(Upper - Channel.Times.begin());
		const size_t Prev  = Next - 1;
		if (Channel.Interpolation == EAnimationInterpolation::Step)
		{
			return Channel.Values[Prev];
		}

		const float Span  = Channel.Times[Next] - Channel.Times[Prev];
		const float Alpha = Span > FMath::SmallNumber ? (Time - Channel.Times[Prev]) / Span : 0.0f;
		const FVector4& A = Channel.Values[Prev];
		const FVector4& B = Channel.Values[Next];
		if (Channel.Path == EAnimationPath::Rotation)
		{
			const FQuat Q = FQuat::Slerp(FQuat(A.X, A.Y, A.Z, A.W), FQuat(B.X, B.Y, B.Z, B.W), Alpha);
			return FVector4(Q.X, Q.Y, Q.Z, Q.W);
		}
		return A + (B - A) * Alpha;
	}

	void SampleClip(const FAnimationClip& Clip, float Time, std::vector<FNodePose>& InOutPose)
	{
		for (const FAnimationChannel& Channel : Clip.Channels)
		{
			if (Channel.Node < 0 || static_cast<size_t>(Channel.Node) >= InOutPose.size())
			{
				continue;
			}
			const FVector4 Value = SampleChannel(Channel, Time);
			FNodePose&     Pose  = InOutPose[Channel.Node];
			switch (Channel.Path)
			{
			case EAnimationPath::Translation: Pose.Translation = FVector3(Value.X, Value.Y, Value.Z); break;
			case EAnimationPath::Rotation:    Pose.Rotation    = FQuat(Value.X, Value.Y, Value.Z, Value.W).GetNormalized(); break;
			case EAnimationPath::Scale:       Pose.Scale       = FVector3(Value.X, Value.Y, Value.Z); break;
			}
		}
	}

	FNodePose BlendPose(const FNodePose& A, const FNodePose& B, float Alpha)
	{
		FNodePose Result;
		Result.Translation = FVector3::Lerp(A.Translation, B.Translation, Alpha);
		Result.Rotation    = FQuat::Slerp(A.Rotation, B.Rotation, Alpha);
		Result.Scale       = FVector3::Lerp(A.Scale, B.Scale, Alpha);
		return Result;
	}

	void BlendPoses(const std::vector<FNodePose>& A, const std::vector<FNodePose>& B, float Alpha, std::vector<FNodePose>& Out)
	{
		const size_t Count = FMath::Min(A.size(), B.size());
		Out.resize(Count);
		for (size_t Index = 0; Index < Count; ++Index)
		{
			Out[Index] = BlendPose(A[Index], B[Index], Alpha);
		}
	}

	float AdvanceTime(float Time, float Delta, float Duration, bool bLoop, bool& bOutWrapped)
	{
		bOutWrapped = false;
		if (Duration <= FMath::SmallNumber)
		{
			return 0.0f;
		}
		const float Next = Time + Delta;
		if (!bLoop)
		{
			return FMath::Clamp(Next, 0.0f, Duration);
		}
		if (Next >= 0.0f && Next < Duration)
		{
			return Next;
		}
		bOutWrapped   = true;
		float Wrapped = std::fmod(Next, Duration);
		if (Wrapped < 0.0f)
		{
			Wrapped += Duration;
		}
		return Wrapped;
	}

	float ComputeCrossfadeWeight(float Elapsed, float Duration)
	{
		if (Duration <= FMath::SmallNumber)
		{
			return 1.0f;
		}
		const float T = FMath::Clamp(Elapsed / Duration, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	void ComputeModelMatrices(const std::vector<FNodePose>& Pose, const std::vector<int32>& Parents, std::vector<FMatrix4x4>& OutMatrices)
	{
		const size_t Count = FMath::Min(Pose.size(), Parents.size());
		OutMatrices.assign(Count, FMatrix4x4::Identity);
		std::vector<uint8> Done(Count, 0);

		// 부모가 뒤에 올 수 있으므로 조상 체인을 따라 올라갔다가 내려오며 계산
		std::vector<int32> Chain;
		for (size_t Node = 0; Node < Count; ++Node)
		{
			Chain.clear();
			for (int32 Current = static_cast<int32>(Node); Current >= 0 && !Done[Current]; Current = Parents[Current])
			{
				Chain.push_back(Current);
				if (Chain.size() > Count)
				{
					break; // 순환 방어
				}
			}
			for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
			{
				const int32 Current = *It;
				const int32 Parent  = Parents[Current];
				OutMatrices[Current] = (Parent >= 0 && static_cast<size_t>(Parent) < Count) ? Pose[Current].ToMatrix() * OutMatrices[Parent]
				                                                                            : Pose[Current].ToMatrix();
				Done[Current] = 1;
			}
		}
	}

	FVector3 ComputeRootMotionDelta(const FAnimationChannel& TranslationChannel, float PreviousTime, float NewTime, bool bWrapped, float Duration)
	{
		const auto Sample = [&](float Time) {
			const FVector4 Value = SampleChannel(TranslationChannel, Time);
			return FVector3(Value.X, Value.Y, Value.Z);
		};
		if (!bWrapped)
		{
			return Sample(NewTime) - Sample(PreviousTime);
		}
		return (Sample(Duration) - Sample(PreviousTime)) + (Sample(NewTime) - Sample(0.0f));
	}
} // namespace AnimationMath
