#include "Scene/AnimIK.h"

#include <cmath>

void FFootIkRuntime::ResetBlend()
{
	PelvisOffset = 0.0f;
	ProbeAge     = 1.0e9f;
	for (FFootIkFoot& Foot : Feet)
	{
		Foot.Offset = 0.0f;
		Foot.Normal = FVector3::UpVector;
		Foot.bHit   = false;
	}
}

namespace AnimIKMath
{
	FQuat FromToRotation(const FVector3& From, const FVector3& To)
	{
		const FVector3 A = From.GetNormalized();
		const FVector3 B = To.GetNormalized();
		if (A.IsNearlyZero() || B.IsNearlyZero())
		{
			return FQuat::Identity;
		}
		const float Dot = FMath::Clamp(FVector3::Dot(A, B), -1.0f, 1.0f);
		if (Dot > 1.0f - 1.0e-6f)
		{
			return FQuat::Identity;
		}
		if (Dot < -1.0f + 1.0e-6f)
		{
			// 반대 방향: A에 수직인 아무 축으로 180도
			FVector3 Axis = FVector3::Cross(A, FVector3::UpVector);
			if (Axis.LengthSquared() < 1.0e-6f)
			{
				Axis = FVector3::Cross(A, FVector3::ForwardVector);
			}
			return FQuat::FromAxisAngle(Axis.GetNormalized(), FMath::Pi);
		}
		return FQuat::FromAxisAngle(FVector3::Cross(A, B).GetNormalized(), std::acos(Dot));
	}

	FTwoBoneResult SolveTwoBone(const FVector3& Root, const FVector3& Mid, const FVector3& End, const FVector3& Target, const FVector3& Pole)
	{
		FTwoBoneResult Result;
		Result.RootDelta   = FQuat::Identity;
		Result.MidDelta    = FQuat::Identity;
		Result.EndPosition = End;
		const float UpperLength = (Mid - Root).Length();
		const float LowerLength = (End - Mid).Length();
		const FVector3 ToTarget = Target - Root;
		const float    Distance = ToTarget.Length();
		if (UpperLength < FMath::SmallNumber || LowerLength < FMath::SmallNumber || Distance < FMath::SmallNumber)
		{
			return Result;
		}
		const float MaxReach  = (UpperLength + LowerLength) * 0.9999f;
		const float MinReach  = FMath::Abs(UpperLength - LowerLength) * 1.0001f + FMath::SmallNumber;
		const float Reach     = FMath::Clamp(Distance, MinReach, MaxReach);
		Result.bReachable     = Distance <= UpperLength + LowerLength;
		const FVector3 Direction = ToTarget * (1.0f / Distance);

		// 무릎이 놓일 평면: 목표 방향에 수직인 무릎 방향 성분 (요청 방향 → 지금 무릎 → 아무 수직 방향 순)
		const auto Perpendicular = [&Direction](const FVector3& V) { return V - Direction * FVector3::Dot(V, Direction); };
		FVector3   Bend          = Perpendicular(Pole);
		if (Pole.IsNearlyZero() || Bend.LengthSquared() < 1.0e-8f)
		{
			Bend = Perpendicular(Mid - Root);
		}
		if (Bend.LengthSquared() < 1.0e-8f)
		{
			Bend = Perpendicular(FMath::Abs(Direction.Z) < 0.9f ? FVector3::UpVector : FVector3::ForwardVector);
		}
		Bend = Bend.GetNormalized();

		// 코사인 법칙: 허벅지와 목표 방향 사이 각
		const float CosAngle = FMath::Clamp((UpperLength * UpperLength + Reach * Reach - LowerLength * LowerLength) / (2.0f * UpperLength * Reach), -1.0f, 1.0f);
		const float SinAngle = std::sqrt(FMath::Max(0.0f, 1.0f - CosAngle * CosAngle));
		const FVector3 DesiredMid = Root + Direction * (UpperLength * CosAngle) + Bend * (UpperLength * SinAngle);
		const FVector3 DesiredEnd = Root + Direction * Reach;

		Result.RootDelta          = FromToRotation(Mid - Root, DesiredMid - Root);
		const FVector3 RotatedEnd = DesiredMid + Result.RootDelta.RotateVector(End - Mid);
		Result.MidDelta           = FromToRotation(RotatedEnd - DesiredMid, DesiredEnd - DesiredMid);
		Result.EndPosition        = DesiredEnd;
		return Result;
	}

	FQuat ComputeLookAtDelta(const FVector3& CurrentForward, const FVector3& Desired, float MaxAngleRadians, float Weight)
	{
		const FVector3 A = CurrentForward.GetNormalized();
		const FVector3 B = Desired.GetNormalized();
		if (A.IsNearlyZero() || B.IsNearlyZero() || Weight <= 0.0f)
		{
			return FQuat::Identity;
		}
		const float Dot   = FMath::Clamp(FVector3::Dot(A, B), -1.0f, 1.0f);
		const float Angle = std::acos(Dot);
		if (Angle < 1.0e-5f)
		{
			return FQuat::Identity;
		}
		FVector3 Axis = FVector3::Cross(A, B);
		if (Axis.LengthSquared() < 1.0e-10f)
		{
			Axis = FVector3::Cross(A, FMath::Abs(A.Z) < 0.9f ? FVector3::UpVector : FVector3::ForwardVector); // 정반대: 아무 수직 축
		}
		const float Clamped = FMath::Min(Angle, FMath::Max(MaxAngleRadians, 0.0f)) * FMath::Clamp(Weight, 0.0f, 1.0f);
		return FQuat::FromAxisAngle(Axis.GetNormalized(), Clamped);
	}

	float SmoothTowards(float Current, float Target, float Speed, float DeltaSeconds)
	{
		if (DeltaSeconds <= 0.0f)
		{
			return Current;
		}
		if (Speed <= 0.0f)
		{
			return Target;
		}
		return Current + (Target - Current) * (1.0f - std::exp(-Speed * DeltaSeconds));
	}

	std::vector<std::string> SplitBoneList(const std::string& Text)
	{
		std::vector<std::string> Names;
		size_t                   Start = 0;
		while (Start <= Text.size())
		{
			size_t Comma = Text.find(',', Start);
			if (Comma == std::string::npos)
			{
				Comma = Text.size();
			}
			std::string Name = Text.substr(Start, Comma - Start);
			const size_t First = Name.find_first_not_of(" \t");
			const size_t Last  = Name.find_last_not_of(" \t");
			if (First != std::string::npos)
			{
				Names.push_back(Name.substr(First, Last - First + 1));
			}
			Start = Comma + 1;
		}
		return Names;
	}
} // namespace AnimIKMath
