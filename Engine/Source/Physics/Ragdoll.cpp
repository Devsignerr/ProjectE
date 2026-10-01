#include "Physics/Ragdoll.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
	char ToLowerAscii(char Char) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Char))); }

	bool ContainsCaseInsensitive(std::string_view Text, std::string_view Part)
	{
		if (Part.empty() || Part.size() > Text.size())
		{
			return false;
		}
		for (size_t Start = 0; Start + Part.size() <= Text.size(); ++Start)
		{
			size_t Index = 0;
			while (Index < Part.size() && ToLowerAscii(Text[Start + Index]) == ToLowerAscii(Part[Index]))
			{
				++Index;
			}
			if (Index == Part.size())
			{
				return true;
			}
		}
		return false;
	}
} // namespace

namespace RagdollMath
{
	bool MatchesExclude(std::string_view Name, std::string_view ExcludeList)
	{
		while (!ExcludeList.empty())
		{
			const size_t     Comma = ExcludeList.find(',');
			std::string_view Item  = ExcludeList.substr(0, Comma);
			ExcludeList            = Comma == std::string_view::npos ? std::string_view() : ExcludeList.substr(Comma + 1);
			while (!Item.empty() && std::isspace(static_cast<unsigned char>(Item.front())))
			{
				Item.remove_prefix(1);
			}
			while (!Item.empty() && std::isspace(static_cast<unsigned char>(Item.back())))
			{
				Item.remove_suffix(1);
			}
			if (ContainsCaseInsensitive(Name, Item))
			{
				return true;
			}
		}
		return false;
	}

	float SegmentDistance(const FVector3& A0, const FVector3& A1, const FVector3& B0, const FVector3& B1)
	{
		// 두 선분의 최근접점 (Real-Time Collision Detection 5.1.9)
		const FVector3  D1 = A1 - A0;
		const FVector3  D2 = B1 - B0;
		const FVector3  R  = A0 - B0;
		const float     A  = FVector3::Dot(D1, D1);
		const float     E  = FVector3::Dot(D2, D2);
		const float     F  = FVector3::Dot(D2, R);
		constexpr float Epsilon = 1.0e-8f;
		float           S = 0.0f;
		float           T = 0.0f;
		if (A <= Epsilon && E <= Epsilon)
		{
			return FVector3::Distance(A0, B0);
		}
		if (A <= Epsilon)
		{
			T = std::clamp(F / E, 0.0f, 1.0f);
		}
		else
		{
			const float C = FVector3::Dot(D1, R);
			if (E <= Epsilon)
			{
				S = std::clamp(-C / A, 0.0f, 1.0f);
			}
			else
			{
				const float B     = FVector3::Dot(D1, D2);
				const float Denom = A * E - B * B;
				S                 = Denom > Epsilon ? std::clamp((B * F - C * E) / Denom, 0.0f, 1.0f) : 0.0f;
				T                 = (B * S + F) / E;
				if (T < 0.0f)
				{
					T = 0.0f;
					S = std::clamp(-C / A, 0.0f, 1.0f);
				}
				else if (T > 1.0f)
				{
					T = 1.0f;
					S = std::clamp((B - C) / A, 0.0f, 1.0f);
				}
			}
		}
		return FVector3::Distance(A0 + D1 * S, B0 + D2 * T);
	}

	FQuat RotationFromZ(const FVector3& Direction)
	{
		const FVector3 Z    = FVector3::UpVector;
		const FVector3 Axis = FVector3::Cross(Z, Direction);
		const float    Sin  = Axis.Length();
		const float    Cos  = FVector3::Dot(Z, Direction);
		if (Sin < 1.0e-6f)
		{
			return Cos > 0.0f ? FQuat::Identity : FQuat::FromAxisAngle(FVector3::ForwardVector, FMath::Pi);
		}
		return FQuat::FromAxisAngle(Axis * (1.0f / Sin), std::atan2(Sin, Cos));
	}

	std::vector<FRagdollPartLayout> BuildLayout(const std::vector<FRagdollBoneInput>& Bones, const FRagdollComponent& Settings)
	{
		const int32 Count = static_cast<int32>(Bones.size());
		auto ParentOf = [&](int32 Node) {
			const int32 Parent = Bones[Node].Parent;
			return Parent >= 0 && Parent < Count && Parent != Node ? Parent : -1;
		};

		// 제외(자신 또는 조상) / 대상 뼈 / 깊이 (부모 먼저 순서)
		std::vector<uint8> Excluded(Count, 0);
		std::vector<int32> Depth(Count, 0);
		for (int32 Node = 0; Node < Count; ++Node)
		{
			int32 Steps = 0;
			for (int32 Current = Node; Current >= 0 && Steps <= Count; Current = ParentOf(Current), ++Steps)
			{
				Excluded[Node] = Excluded[Node] || Bones[Current].bExclude;
			}
			Depth[Node] = Steps;
		}
		auto Eligible = [&](int32 Node) { return Bones[Node].bJoint && !Excluded[Node]; };
		auto NearestEligibleAncestor = [&](int32 Node) {
			int32 Steps = 0;
			for (int32 Current = ParentOf(Node); Current >= 0 && Steps <= Count; Current = ParentOf(Current), ++Steps)
			{
				if (Eligible(Current))
				{
					return Current;
				}
			}
			return -1;
		};

		std::vector<int32> Order(Count);
		for (int32 Node = 0; Node < Count; ++Node)
		{
			Order[Node] = Node;
		}
		std::stable_sort(Order.begin(), Order.end(), [&](int32 A, int32 B) { return Depth[A] < Depth[B]; });

		// 부모 뼈와 거의 같은 자리의 뼈(MinBoneLength 미만)는 부모에 합친다: 캡슐 없음, 그 자식은 부모의 자식으로 본다
		// (예: 척추 → 1cm 위 목 → 머리: 척추 캡슐이 머리까지 간다)
		const float        MinLength = std::max(Settings.MinBoneLength, 0.01f);
		std::vector<uint8> Collapsed(Count, 0);
		for (const int32 Node : Order)
		{
			if (Eligible(Node))
			{
				const int32 Parent = NearestEligibleAncestor(Node);
				Collapsed[Node]    = Parent >= 0 && FVector3::Distance(Bones[Node].Position, Bones[Parent].Position) < MinLength ? 1 : 0;
			}
		}
		auto EffectiveParent = [&](int32 Node) {
			int32 Parent = NearestEligibleAncestor(Node);
			for (int32 Steps = 0; Parent >= 0 && Collapsed[Parent] && Steps <= Count; ++Steps)
			{
				Parent = NearestEligibleAncestor(Parent);
			}
			return Parent;
		};
		std::vector<std::vector<int32>> Children(Count);
		std::vector<int32>              ParentBone(Count, -1);
		for (int32 Node = 0; Node < Count; ++Node)
		{
			if (Eligible(Node) && !Collapsed[Node])
			{
				ParentBone[Node] = EffectiveParent(Node);
				if (ParentBone[Node] >= 0)
				{
					Children[ParentBone[Node]].push_back(Node);
				}
			}
		}

		std::vector<FRagdollPartLayout> Parts;
		std::vector<int32>              PartOfNode(Count, -1);
		for (const int32 Node : Order)
		{
			if (!Eligible(Node) || Collapsed[Node])
			{
				continue;
			}
			const FVector3 Start = Bones[Node].Position;
			FVector3       End;
			if (!Children[Node].empty())
			{
				FVector3 Sum;
				float    Farthest = -1.0f;
				FVector3 FarthestPosition;
				for (const int32 Child : Children[Node])
				{
					Sum += Bones[Child].Position;
					const float Distance = FVector3::Distance(Start, Bones[Child].Position);
					if (Distance > Farthest)
					{
						Farthest         = Distance;
						FarthestPosition = Bones[Child].Position;
					}
				}
				End = Sum * (1.0f / static_cast<float>(Children[Node].size()));
				if (FVector3::Distance(Start, End) < 0.5f * Farthest)
				{
					// 자식이 사방으로 퍼진 뼈(골반: 척추 + 양 다리): 평균 쪽으로 가장 먼 자식 거리의 절반만큼 (평균이 제자리면 먼 자식 쪽)
					FVector3 Direction = End - Start;
					if (Direction.Length() < 1.0e-4f)
					{
						Direction = FarthestPosition - Start;
					}
					End = Start + Direction.GetNormalized() * (0.5f * Farthest);
				}
			}
			else
			{
				// 끝 뼈: 부모 → 자신 방향으로 부모 뼈 길이의 절반만큼 연장 (손, 머리, 발끝)
				const int32 Parent = ParentBone[Node];
				if (Parent < 0)
				{
					continue;
				}
				const FVector3 Direction = Start - Bones[Parent].Position;
				const float    Length    = Direction.Length();
				if (Length < 1.0e-4f)
				{
					continue;
				}
				End = Start + Direction * (std::max(0.5f * Length, MinLength) / Length);
			}
			const float Length = FVector3::Distance(Start, End);
			if (Length < MinLength)
			{
				continue; // 짧은 뼈: 부모 캡슐을 따라간다
			}

			FRagdollPartLayout Part;
			Part.Node   = Node;
			Part.Start  = Start;
			Part.End    = End;
			Part.Radius = std::min(std::max(Length * Settings.RadiusScale, Settings.MinRadius), 0.5f * Length);
			for (int32 Ancestor = ParentOf(Node), Steps = 0; Ancestor >= 0 && Steps <= Count; Ancestor = ParentOf(Ancestor), ++Steps)
			{
				if (PartOfNode[Ancestor] >= 0)
				{
					Part.ParentPart = PartOfNode[Ancestor];
					break;
				}
			}
			PartOfNode[Node] = static_cast<int32>(Parts.size());
			Parts.push_back(Part);
		}
		return Parts;
	}
} // namespace RagdollMath
