#include "Scene/AnimRetarget.h"

#include "Core/Log.h"
#include "Scene/ModelMetadata.h"

#include <algorithm>
#include <cctype>
#include <cmath>

E_DECLARE_LOG_CATEGORY(LogAnimation)

namespace
{
	using enum EHumanoidBone;

	constexpr const char* GBoneNames[HumanoidBoneCount] = {
		"Root",          "Hips",          "Spine",         "Chest",         "UpperChest",    "Neck",          "Head",
		"LeftShoulder",  "LeftUpperArm",  "LeftLowerArm",  "LeftHand",      "RightShoulder", "RightUpperArm", "RightLowerArm",
		"RightHand",     "LeftUpperLeg",  "LeftLowerLeg",  "LeftFoot",      "LeftToes",      "RightUpperLeg", "RightLowerLeg",
		"RightFoot",     "RightToes",
	};

	constexpr size_t ToIndex(EHumanoidBone Bone) { return static_cast<size_t>(Bone); }

	// 이름 규칙의 "부위" (좌우 없는 종류)
	enum class EPart : uint8
	{
		Root,
		Hips,
		Neck,
		Head,
		Shoulder,
		UpperArm,
		LowerArm,
		Hand,
		UpperLeg,
		LowerLeg,
		Foot,
		Toes,
		None,
	};

	struct FPartRule
	{
		const char* Base;
		EPart       Part;
		int32       Priority; // 작을수록 우선
	};

	constexpr FPartRule GPartRules[] = {
		{ "root", EPart::Root, 0 },          { "hips", EPart::Hips, 0 },          { "pelvis", EPart::Hips, 0 },       { "hip", EPart::Hips, 1 },
		{ "neck", EPart::Neck, 0 },          { "head", EPart::Head, 0 },          { "shoulder", EPart::Shoulder, 0 }, { "clavicle", EPart::Shoulder, 0 },
		{ "collar", EPart::Shoulder, 1 },    { "upperarm", EPart::UpperArm, 0 },  { "arm", EPart::UpperArm, 1 },      { "lowerarm", EPart::LowerArm, 0 },
		{ "forearm", EPart::LowerArm, 0 },   { "wrist", EPart::Hand, 0 },         { "hand", EPart::Hand, 1 },         { "upperleg", EPart::UpperLeg, 0 },
		{ "thigh", EPart::UpperLeg, 0 },     { "upleg", EPart::UpperLeg, 0 },     { "lowerleg", EPart::LowerLeg, 0 }, { "calf", EPart::LowerLeg, 0 },
		{ "shin", EPart::LowerLeg, 0 },      { "leg", EPart::LowerLeg, 1 },       { "foot", EPart::Foot, 0 },         { "ankle", EPart::Foot, 1 },
		{ "toes", EPart::Toes, 0 },          { "toe", EPart::Toes, 0 },           { "toebase", EPart::Toes, 0 },      { "ball", EPart::Toes, 1 },
	};

	bool IsSidedPart(EPart Part) { return Part >= EPart::Shoulder && Part <= EPart::Toes; }

	EHumanoidBone ToBone(EPart Part, EBoneSide Side)
	{
		switch (Part)
		{
		case EPart::Root: return Root;
		case EPart::Hips: return Hips;
		case EPart::Neck: return Neck;
		case EPart::Head: return Head;
		default: break;
		}
		const bool bLeft = Side == EBoneSide::Left;
		switch (Part)
		{
		case EPart::Shoulder: return bLeft ? LeftShoulder : RightShoulder;
		case EPart::UpperArm: return bLeft ? LeftUpperArm : RightUpperArm;
		case EPart::LowerArm: return bLeft ? LeftLowerArm : RightLowerArm;
		case EPart::Hand:     return bLeft ? LeftHand : RightHand;
		case EPart::UpperLeg: return bLeft ? LeftUpperLeg : RightUpperLeg;
		case EPart::LowerLeg: return bLeft ? LeftLowerLeg : RightLowerLeg;
		case EPart::Foot:     return bLeft ? LeftFoot : RightFoot;
		case EPart::Toes:     return bLeft ? LeftToes : RightToes;
		default: return Count;
		}
	}

	bool IsAncestor(const std::vector<int32>& Parents, int32 Ancestor, int32 Node)
	{
		if (Ancestor < 0 || Node < 0)
		{
			return false;
		}
		for (int32 Current = Parents[static_cast<size_t>(Node)], Guard = 0; Current >= 0 && Guard <= static_cast<int32>(Parents.size());
		     Current = Parents[static_cast<size_t>(Current)], ++Guard)
		{
			if (Current == Ancestor)
			{
				return true;
			}
		}
		return false;
	}

	int32 GetDepth(const std::vector<int32>& Parents, int32 Node)
	{
		int32 Depth = 0;
		for (int32 Current = Parents[static_cast<size_t>(Node)]; Current >= 0 && Depth <= static_cast<int32>(Parents.size()); Current = Parents[static_cast<size_t>(Current)])
		{
			++Depth;
		}
		return Depth;
	}

	// 현재 포즈에서 노드의 모델 공간 회전/행렬 (조상 사슬을 따라 — 부모 순서 무관)
	FQuat ChainRotation(const std::vector<FNodePose>& Pose, const std::vector<int32>& Parents, int32 Node)
	{
		FQuat Rotation = FQuat::Identity;
		for (int32 Current = Node, Guard = 0; Current >= 0 && Guard <= static_cast<int32>(Parents.size()); Current = Parents[static_cast<size_t>(Current)], ++Guard)
		{
			Rotation = (Pose[static_cast<size_t>(Current)].Rotation * Rotation).GetNormalized();
		}
		return Rotation;
	}

	FMatrix4x4 ChainMatrix(const std::vector<FNodePose>& Pose, const std::vector<int32>& Parents, int32 Node)
	{
		FMatrix4x4 Matrix = FMatrix4x4::Identity;
		for (int32 Current = Node, Guard = 0; Current >= 0 && Guard <= static_cast<int32>(Parents.size()); Current = Parents[static_cast<size_t>(Current)], ++Guard)
		{
			Matrix = Matrix * Pose[static_cast<size_t>(Current)].ToMatrix();
		}
		return Matrix;
	}

	std::string ToLower(std::string_view Text)
	{
		std::string Result(Text);
		std::transform(Result.begin(), Result.end(), Result.begin(), [](char C) { return static_cast<char>(std::tolower(static_cast<unsigned char>(C))); });
		return Result;
	}
} // namespace

namespace AnimRetargetMath
{
	const char* GetBoneName(EHumanoidBone Bone) { return ToIndex(Bone) < HumanoidBoneCount ? GBoneNames[ToIndex(Bone)] : ""; }

	EHumanoidBone FindBone(std::string_view Name)
	{
		for (size_t Index = 0; Index < HumanoidBoneCount; ++Index)
		{
			if (Name == GBoneNames[Index])
			{
				return static_cast<EHumanoidBone>(Index);
			}
		}
		return Count;
	}

	EHumanoidBone GetParentBone(EHumanoidBone Bone)
	{
		switch (Bone)
		{
		case Root:
		case Hips: return Count;
		case Spine: return Hips;
		case Chest: return Spine;
		case UpperChest: return Chest;
		case Neck: return UpperChest;
		case Head: return Neck;
		case LeftShoulder:
		case RightShoulder: return UpperChest;
		case LeftUpperArm: return LeftShoulder;
		case RightUpperArm: return RightShoulder;
		case LeftLowerArm: return LeftUpperArm;
		case RightLowerArm: return RightUpperArm;
		case LeftHand: return LeftLowerArm;
		case RightHand: return RightLowerArm;
		case LeftUpperLeg:
		case RightUpperLeg: return Hips;
		case LeftLowerLeg: return LeftUpperLeg;
		case RightLowerLeg: return RightUpperLeg;
		case LeftFoot: return LeftLowerLeg;
		case RightFoot: return RightLowerLeg;
		case LeftToes: return LeftFoot;
		case RightToes: return RightFoot;
		default: return Count;
		}
	}

	std::vector<EHumanoidBone> GetDirectionChildren(EHumanoidBone Bone)
	{
		switch (Bone)
		{
		case Spine: return { Chest, UpperChest, Neck, Head };
		case Chest: return { UpperChest, Neck, Head };
		case UpperChest: return { Neck, Head };
		case Neck: return { Head };
		case LeftShoulder: return { LeftUpperArm };
		case RightShoulder: return { RightUpperArm };
		case LeftUpperArm: return { LeftLowerArm, LeftHand };
		case RightUpperArm: return { RightLowerArm, RightHand };
		case LeftLowerArm: return { LeftHand };
		case RightLowerArm: return { RightHand };
		case LeftUpperLeg: return { LeftLowerLeg, LeftFoot };
		case RightUpperLeg: return { RightLowerLeg, RightFoot };
		case LeftLowerLeg: return { LeftFoot };
		case RightLowerLeg: return { RightFoot };
		case LeftFoot: return { LeftToes };
		case RightFoot: return { RightToes };
		default: return {};
		}
	}

	FBoneNameInfo ParseBoneName(std::string_view Name)
	{
		std::string Text = ToLower(Name);
		// 네임스페이스 (mixamorig:Hips, Armature|Hips)
		if (const size_t Separator = Text.find_last_of(":|"); Separator != std::string::npos)
		{
			Text = Text.substr(Separator + 1);
		}
		if (Text.rfind("mixamorig", 0) == 0)
		{
			Text = Text.substr(9);
		}
		FBoneNameInfo Info;
		// "left"/"right" 단어
		if (const size_t At = Text.find("left"); At != std::string::npos)
		{
			Info.Side = EBoneSide::Left;
			Text.erase(At, 4);
		}
		else if (const size_t AtRight = Text.find("right"); AtRight != std::string::npos)
		{
			Info.Side = EBoneSide::Right;
			Text.erase(AtRight, 5);
		}
		else
		{
			// 끝 ".l" "_l" "-l" " l" / 앞 "l_" "l." "l-" "l " (+ 끝에 붙은 숫자는 그 앞에서 판정: thigh_l01 등은 드물어 무시)
			const auto IsSeparator = [](char C) { return C == '.' || C == '_' || C == '-' || C == ' '; };
			if (Text.size() >= 3 && IsSeparator(Text[Text.size() - 2]) && (Text.back() == 'l' || Text.back() == 'r'))
			{
				Info.Side = Text.back() == 'l' ? EBoneSide::Left : EBoneSide::Right;
				Text.resize(Text.size() - 2);
			}
			else if (Text.size() >= 3 && IsSeparator(Text[1]) && (Text[0] == 'l' || Text[0] == 'r'))
			{
				Info.Side = Text[0] == 'l' ? EBoneSide::Left : EBoneSide::Right;
				Text.erase(0, 2);
			}
		}
		for (const char C : Text)
		{
			if (C >= 'a' && C <= 'z')
			{
				Info.Base.push_back(C);
			}
		}
		return Info;
	}

	FHumanoidMapping AutoMap(const std::vector<std::string>& Names, const std::vector<int32>& Parents, const std::vector<FMatrix4x4>& RestMatrices)
	{
		FHumanoidMapping Mapping;
		Mapping.fill(-1);
		const size_t NodeCount = std::min(Names.size(), Parents.size());
		struct FBest
		{
			int32 Priority = 1 << 30;
			int32 Depth    = 1 << 30;
			int32 Node     = -1;
		};
		std::array<FBest, HumanoidBoneCount> Best{};
		for (size_t Node = 0; Node < NodeCount; ++Node)
		{
			const FBoneNameInfo Info = ParseBoneName(Names[Node]);
			for (const FPartRule& Rule : GPartRules)
			{
				if (Info.Base != Rule.Base)
				{
					continue;
				}
				EBoneSide Side = Info.Side;
				if (IsSidedPart(Rule.Part))
				{
					if (Side == EBoneSide::None && Node < RestMatrices.size())
					{
						// 좌우 토큰 없음: 기본 포즈 모델 공간 Y (+Y = 왼쪽)
						const float Y = RestMatrices[Node].GetOrigin().Y;
						Side          = Y > 0.01f ? EBoneSide::Left : (Y < -0.01f ? EBoneSide::Right : EBoneSide::None);
					}
					if (Side == EBoneSide::None)
					{
						break;
					}
				}
				else if (Side != EBoneSide::None)
				{
					break; // 좌우가 붙은 몸통 이름은 다른 뼈 (예: LeftHip은 다리)
				}
				const EHumanoidBone Bone  = ToBone(Rule.Part, Side);
				const int32         Depth = GetDepth(Parents, static_cast<int32>(Node));
				FBest&              Slot  = Best[ToIndex(Bone)];
				if (Rule.Priority < Slot.Priority || (Rule.Priority == Slot.Priority && Depth < Slot.Depth))
				{
					Slot = { Rule.Priority, Depth, static_cast<int32>(Node) };
				}
				break;
			}
		}
		for (size_t Index = 0; Index < HumanoidBoneCount; ++Index)
		{
			Mapping[Index] = Best[Index].Node;
		}

		// 계층 검증
		const int32 HipsNode = Mapping[ToIndex(Hips)];
		if (HipsNode < 0)
		{
			Mapping.fill(-1);
			return Mapping;
		}
		if (!IsAncestor(Parents, Mapping[ToIndex(Root)], HipsNode))
		{
			Mapping[ToIndex(Root)] = -1;
		}
		for (size_t Index = ToIndex(Neck); Index < HumanoidBoneCount; ++Index)
		{
			if (Mapping[Index] >= 0 && !IsAncestor(Parents, HipsNode, Mapping[Index]))
			{
				Mapping[Index] = -1;
			}
		}
		const auto RequireChild = [&](EHumanoidBone Parent, EHumanoidBone Child) {
			if (Mapping[ToIndex(Child)] >= 0 && Mapping[ToIndex(Parent)] >= 0 && !IsAncestor(Parents, Mapping[ToIndex(Parent)], Mapping[ToIndex(Child)]))
			{
				Mapping[ToIndex(Child)] = -1;
			}
		};
		for (const auto& [Parent, Child] : { std::pair{ LeftUpperArm, LeftLowerArm }, std::pair{ LeftLowerArm, LeftHand }, std::pair{ RightUpperArm, RightLowerArm },
		                                     std::pair{ RightLowerArm, RightHand }, std::pair{ LeftUpperLeg, LeftLowerLeg }, std::pair{ LeftLowerLeg, LeftFoot },
		                                     std::pair{ LeftFoot, LeftToes }, std::pair{ RightUpperLeg, RightLowerLeg }, std::pair{ RightLowerLeg, RightFoot },
		                                     std::pair{ RightFoot, RightToes }, std::pair{ LeftShoulder, LeftUpperArm }, std::pair{ RightShoulder, RightUpperArm },
		                                     std::pair{ Neck, Head } })
		{
			RequireChild(Parent, Child);
		}

		// 척추 사슬: Hips와 Neck(없으면 Head) 사이 조상
		const int32        Top = Mapping[ToIndex(Neck)] >= 0 ? Mapping[ToIndex(Neck)] : Mapping[ToIndex(Head)];
		std::vector<int32> Chain;
		if (Top >= 0)
		{
			for (int32 Current = Parents[static_cast<size_t>(Top)], Guard = 0; Current >= 0 && Current != HipsNode && Guard <= static_cast<int32>(NodeCount);
			     Current = Parents[static_cast<size_t>(Current)], ++Guard)
			{
				Chain.push_back(Current);
			}
			std::reverse(Chain.begin(), Chain.end()); // Hips 쪽부터
		}
		if (!Chain.empty())
		{
			Mapping[ToIndex(Spine)] = Chain.front();
			if (Chain.size() == 2)
			{
				Mapping[ToIndex(Chest)] = Chain[1];
			}
			else if (Chain.size() >= 3)
			{
				Mapping[ToIndex(Chest)]      = Chain[Chain.size() / 2];
				Mapping[ToIndex(UpperChest)] = Chain.back();
				if (Chain.size() == 3)
				{
					Mapping[ToIndex(Chest)] = Chain[1];
				}
			}
		}

		// 어깨 (계층): 이름으로 못 찾았으면 위팔의 부모가 척추 사슬 밖이고 Hips 자손이면 그것
		for (const auto& [Shoulder, UpperArm] : { std::pair{ LeftShoulder, LeftUpperArm }, std::pair{ RightShoulder, RightUpperArm } })
		{
			const int32 Arm = Mapping[ToIndex(UpperArm)];
			if (Mapping[ToIndex(Shoulder)] >= 0 || Arm < 0)
			{
				continue;
			}
			const int32 Parent = Parents[static_cast<size_t>(Arm)];
			if (Parent >= 0 && Parent != HipsNode && std::find(Chain.begin(), Chain.end(), Parent) == Chain.end() && IsAncestor(Parents, HipsNode, Parent) &&
			    Parent != Mapping[ToIndex(Neck)])
			{
				Mapping[ToIndex(Shoulder)] = Parent;
			}
		}
		return Mapping;
	}

	FHumanoidMapping ResolveMapping(const FAnimationSet& Set, const FModelMetadata* Metadata)
	{
		FHumanoidMapping Mapping = AutoMap(Set.NodeNames, Set.NodeParents, Set.RestModelMatrices);
		if (Metadata == nullptr)
		{
			return Mapping;
		}
		for (const FRetargetBoneOverride& Override : Metadata->RetargetBones)
		{
			const EHumanoidBone Bone = FindBone(Override.Bone);
			if (Bone == Count)
			{
				continue;
			}
			Mapping[ToIndex(Bone)] = Override.Node.empty() ? -1 : Set.FindNode(Override.Node);
		}
		return Mapping;
	}

	FQuat RetargetRotation(const FQuat& SourceBind, const FQuat& Source, const FQuat& TargetReference)
	{
		return (Source * SourceBind.Inverse() * TargetReference).GetNormalized();
	}

	FQuat MakeRotationBetween(const FVector3& From, const FVector3& To)
	{
		const float FromLength = From.Length();
		const float ToLength   = To.Length();
		if (FromLength <= 1.0e-6f || ToLength <= 1.0e-6f)
		{
			return FQuat::Identity;
		}
		const FVector3 A   = From / FromLength;
		const FVector3 B   = To / ToLength;
		const float    Dot = FMath::Clamp(FVector3::Dot(A, B), -1.0f, 1.0f);
		if (Dot >= 1.0f - 1.0e-7f)
		{
			return FQuat::Identity;
		}
		if (Dot <= -1.0f + 1.0e-6f)
		{
			// 반대 방향: A와 수직인 아무 축으로 180°
			FVector3 Axis = FVector3::Cross(A, FVector3::ForwardVector);
			if (Axis.LengthSquared() < 1.0e-6f)
			{
				Axis = FVector3::Cross(A, FVector3::RightVector);
			}
			return FQuat::FromAxisAngle(Axis.GetNormalized(), FMath::Pi);
		}
		const FVector3 Axis = FVector3::Cross(A, B).GetNormalized();
		return FQuat::FromAxisAngle(Axis, std::acos(Dot));
	}

	float ComputeHeightRatio(float SourceHipsHeight, float TargetHipsHeight)
	{
		if (SourceHipsHeight <= 1.0f || TargetHipsHeight <= 1.0f)
		{
			return 1.0f;
		}
		return TargetHipsHeight / SourceHipsHeight;
	}

	FRetargetSkeleton MakeSkeleton(const FAnimationSet& Set, const FHumanoidMapping& Mapping)
	{
		FRetargetSkeleton Skeleton;
		Skeleton.Parents      = Set.NodeParents;
		Skeleton.RestPose     = Set.RestPose;
		Skeleton.RestMatrices = Set.RestModelMatrices;
		if (Skeleton.RestMatrices.size() != Skeleton.Parents.size())
		{
			AnimationMath::ComputeModelMatrices(Skeleton.RestPose, Skeleton.Parents, Skeleton.RestMatrices);
		}
		AnimationMath::ComputeModelRotations(Skeleton.RestPose, Skeleton.Parents, Skeleton.RestRotations);
		Skeleton.Mapping = Mapping;
		for (int32& Node : Skeleton.Mapping)
		{
			if (Node >= static_cast<int32>(Skeleton.Parents.size()))
			{
				Node = -1;
			}
		}
		return Skeleton;
	}

	FRetargetPlan MakePlan(const FRetargetSkeleton& Source, const FRetargetSkeleton& Target)
	{
		FRetargetPlan Plan;
		Plan.Source = &Source;
		Plan.Target = &Target;
		Plan.Align.fill(FQuat::Identity);
		for (size_t Index = 0; Index < HumanoidBoneCount; ++Index)
		{
			Plan.bActive[Index] = Source.Mapping[Index] >= 0 && Target.Mapping[Index] >= 0 ? 1 : 0;
		}
		const auto SourcePosition = [&](EHumanoidBone Bone) { return Source.RestMatrices[static_cast<size_t>(Source.Mapping[ToIndex(Bone)])].GetOrigin(); };
		const auto TargetPosition = [&](EHumanoidBone Bone) { return Target.RestMatrices[static_cast<size_t>(Target.Mapping[ToIndex(Bone)])].GetOrigin(); };

		// 정면 보정: 왼다리 - 오른다리 수평 방향
		Plan.Facing = FQuat::Identity;
		if (Plan.bActive[ToIndex(LeftUpperLeg)] && Plan.bActive[ToIndex(RightUpperLeg)])
		{
			FVector3 SourceLateral = SourcePosition(LeftUpperLeg) - SourcePosition(RightUpperLeg);
			FVector3 TargetLateral = TargetPosition(LeftUpperLeg) - TargetPosition(RightUpperLeg);
			SourceLateral.Z        = 0.0f;
			TargetLateral.Z        = 0.0f;
			if (SourceLateral.LengthSquared() > 1.0e-4f && TargetLateral.LengthSquared() > 1.0e-4f)
			{
				const float Angle = std::atan2(TargetLateral.Y, TargetLateral.X) - std::atan2(SourceLateral.Y, SourceLateral.X);
				Plan.Facing       = FQuat::FromAxisAngle(FVector3::UpVector, Angle);
			}
		}
		if (Plan.bActive[ToIndex(Hips)])
		{
			Plan.HeightRatio = ComputeHeightRatio(SourcePosition(Hips).Z, TargetPosition(Hips).Z);
		}

		// 정렬 보정 (리그 부모가 enum에서 먼저 오므로 순서대로 이어받을 수 있다)
		for (size_t Index = 0; Index < HumanoidBoneCount; ++Index)
		{
			const EHumanoidBone Bone = static_cast<EHumanoidBone>(Index);
			if (!Plan.bActive[Index] || Bone == Root || Bone == Hips)
			{
				continue;
			}
			bool bFound = false;
			for (const EHumanoidBone Child : GetDirectionChildren(Bone))
			{
				if (!Plan.bActive[ToIndex(Child)])
				{
					continue;
				}
				const FVector3 SourceDirection = Plan.Facing.RotateVector(SourcePosition(Child) - SourcePosition(Bone));
				const FVector3 TargetDirection = TargetPosition(Child) - TargetPosition(Bone);
				Plan.Align[Index]              = MakeRotationBetween(TargetDirection, SourceDirection);
				bFound                         = true;
				break;
			}
			if (!bFound)
			{
				for (EHumanoidBone Parent = GetParentBone(Bone); Parent != Count; Parent = GetParentBone(Parent))
				{
					if (Plan.bActive[ToIndex(Parent)])
					{
						Plan.Align[Index] = Plan.Align[ToIndex(Parent)];
						break;
					}
				}
			}
		}

		// 대상 노드 깊이 순 (부모 먼저 — 로컬 변환에 부모의 새 모델 회전이 필요)
		for (size_t Index = 0; Index < HumanoidBoneCount; ++Index)
		{
			if (Plan.bActive[Index])
			{
				Plan.Order.push_back(static_cast<EHumanoidBone>(Index));
			}
		}
		std::stable_sort(Plan.Order.begin(), Plan.Order.end(), [&](EHumanoidBone A, EHumanoidBone B) {
			return GetDepth(Target.Parents, Target.Mapping[ToIndex(A)]) < GetDepth(Target.Parents, Target.Mapping[ToIndex(B)]);
		});
		return Plan;
	}

	FAnimationClip RetargetClip(const FAnimationClip& Clip, const FRetargetPlan& Plan, float SampleRate)
	{
		FAnimationClip Result;
		Result.Name     = Clip.Name;
		Result.Duration = Clip.Duration;
		if (Plan.Source == nullptr || Plan.Target == nullptr || Plan.Order.empty())
		{
			return Result;
		}
		const FRetargetSkeleton& Source = *Plan.Source;
		const FRetargetSkeleton& Target = *Plan.Target;
		const int32 SampleCount = Clip.Duration > 1.0e-4f ? std::max(2, static_cast<int32>(std::ceil(Clip.Duration * std::max(SampleRate, 1.0f))) + 1) : 1;

		// 채널: 활성 뼈마다 회전, Root/Hips는 이동도
		std::vector<int32> RotationChannel(HumanoidBoneCount, -1);
		std::vector<int32> TranslationChannel(HumanoidBoneCount, -1);
		for (const EHumanoidBone Bone : Plan.Order)
		{
			FAnimationChannel Channel;
			Channel.Node = Target.Mapping[ToIndex(Bone)];
			Channel.Path = EAnimationPath::Rotation;
			Channel.Times.reserve(static_cast<size_t>(SampleCount));
			Channel.Values.reserve(static_cast<size_t>(SampleCount));
			RotationChannel[ToIndex(Bone)] = static_cast<int32>(Result.Channels.size());
			Result.Channels.push_back(Channel);
			if (Bone == Root || Bone == Hips)
			{
				Channel.Path                      = EAnimationPath::Translation;
				TranslationChannel[ToIndex(Bone)] = static_cast<int32>(Result.Channels.size());
				Result.Channels.push_back(std::move(Channel));
			}
		}

		const FQuat            FacingInverse = Plan.Facing.Inverse();
		std::vector<FNodePose> SourcePose;
		std::vector<FQuat>     SourceRotations;
		std::vector<FMatrix4x4> SourceMatrices;
		std::vector<FNodePose> TargetPose;
		for (int32 Sample = 0; Sample < SampleCount; ++Sample)
		{
			const float Time = SampleCount > 1 ? std::min(Clip.Duration * static_cast<float>(Sample) / static_cast<float>(SampleCount - 1), Clip.Duration) : 0.0f;
			SourcePose       = Source.RestPose;
			AnimationMath::SampleClip(Clip, Time, SourcePose);
			AnimationMath::ComputeModelRotations(SourcePose, Source.Parents, SourceRotations);
			AnimationMath::ComputeModelMatrices(SourcePose, Source.Parents, SourceMatrices);

			TargetPose = Target.RestPose;
			for (const EHumanoidBone Bone : Plan.Order)
			{
				const size_t Index      = ToIndex(Bone);
				const int32  SourceNode = Source.Mapping[Index];
				const int32  TargetNode = Target.Mapping[Index];
				const int32  Parent     = Target.Parents[static_cast<size_t>(TargetNode)];

				// 회전: F · Gs(t) · Gs(bind)⁻¹ · F⁻¹ · Align · Gd(bind)
				const FQuat Delta = (Plan.Facing * SourceRotations[static_cast<size_t>(SourceNode)] *
				                     Source.RestRotations[static_cast<size_t>(SourceNode)].Inverse() * FacingInverse).GetNormalized();
				const FQuat Model = (Delta * Plan.Align[Index] * Target.RestRotations[static_cast<size_t>(TargetNode)]).GetNormalized();
				const FQuat ParentRotation = Parent >= 0 ? ChainRotation(TargetPose, Target.Parents, Parent) : FQuat::Identity;
				TargetPose[static_cast<size_t>(TargetNode)].Rotation = (ParentRotation.Inverse() * Model).GetNormalized();

				// 이동: Root/Hips만 (기본 포즈 대비 변위 × 키 비율)
				if (TranslationChannel[Index] >= 0)
				{
					const FVector3 Displacement = Plan.Facing.RotateVector(SourceMatrices[static_cast<size_t>(SourceNode)].GetOrigin() -
					                                                       Source.RestMatrices[static_cast<size_t>(SourceNode)].GetOrigin()) *
					                              Plan.HeightRatio;
					const FVector3   Position     = Target.RestMatrices[static_cast<size_t>(TargetNode)].GetOrigin() + Displacement;
					const FMatrix4x4 ParentMatrix = Parent >= 0 ? ChainMatrix(TargetPose, Target.Parents, Parent) : FMatrix4x4::Identity;
					TargetPose[static_cast<size_t>(TargetNode)].Translation = ParentMatrix.GetInverse().TransformPosition(Position);
					FAnimationChannel& Channel = Result.Channels[static_cast<size_t>(TranslationChannel[Index])];
					const FVector3&    Local   = TargetPose[static_cast<size_t>(TargetNode)].Translation;
					Channel.Times.push_back(Time);
					Channel.Values.emplace_back(Local.X, Local.Y, Local.Z, 0.0f);
				}
				FAnimationChannel& Channel = Result.Channels[static_cast<size_t>(RotationChannel[Index])];
				const FQuat&       Local   = TargetPose[static_cast<size_t>(TargetNode)].Rotation;
				Channel.Times.push_back(Time);
				Channel.Values.emplace_back(Local.X, Local.Y, Local.Z, Local.W);
			}
		}
		return Result;
	}
} // namespace AnimRetargetMath

// ---------------------------------------------------------------- 라이브러리

FAnimRetargetLibrary& FAnimRetargetLibrary::Get()
{
	static FAnimRetargetLibrary Instance;
	return Instance;
}

std::string FAnimRetargetLibrary::NormalizePath(std::string_view AssetPath)
{
	std::string Result = ToLower(AssetPath);
	std::replace(Result.begin(), Result.end(), '\\', '/');
	return Result;
}

bool FAnimRetargetLibrary::SplitQualifiedName(std::string_view Name, std::string& OutModel, std::string& OutClip)
{
	const size_t Separator = Name.rfind(':');
	if (Separator == std::string_view::npos || Separator + 1 >= Name.size())
	{
		return false;
	}
	const std::string Model = ToLower(Name.substr(0, Separator));
	for (const std::string_view Extension : { ".glb", ".gltf", ".fbx" })
	{
		if (Model.size() > Extension.size() && Model.compare(Model.size() - Extension.size(), Extension.size(), Extension) == 0)
		{
			OutModel = std::string(Name.substr(0, Separator));
			OutClip  = std::string(Name.substr(Separator + 1));
			return true;
		}
	}
	return false;
}

void FAnimRetargetLibrary::AddSource(const std::string& AssetPath, FAnimSourceModel Source)
{
	const std::string Key = NormalizePath(AssetPath);
	Manual[Key]           = std::make_shared<const FAnimSourceModel>(std::move(Source));
	Invalidate(AssetPath);
}

std::shared_ptr<const FAnimSourceModel> FAnimRetargetLibrary::LoadSource(const std::string& AssetPath)
{
	const std::string Key = NormalizePath(AssetPath);
	if (const auto Found = Manual.find(Key); Found != Manual.end())
	{
		return Found->second;
	}
	if (const auto Found = Sources.find(Key); Found != Sources.end())
	{
		return Found->second;
	}
	std::shared_ptr<const FAnimSourceModel> Result;
	FAnimSourceModel                        Loaded;
	if (Loader && Loader(AssetPath, Loaded) && Loaded.Set)
	{
		Result = std::make_shared<const FAnimSourceModel>(std::move(Loaded));
		E_LOG(LogAnimation, Display, "리타기팅 소스 모델: {} (클립 {})", AssetPath, Result->Set->Clips.size());
	}
	else
	{
		E_LOG(LogAnimation, Warning, "리타기팅 소스 모델을 읽지 못했습니다: {}", AssetPath);
	}
	Sources[Key] = Result;
	return Result;
}

std::shared_ptr<const std::vector<FAnimationClip>> FAnimRetargetLibrary::GetRetargetedClips(const std::string& SourcePath, const std::string& TargetKey,
                                                                                           const FAnimationSet& Target, const FModelMetadata* TargetMetadata)
{
	const std::string Key = NormalizePath(SourcePath) + "|" + TargetKey;
	if (const auto Found = Results.find(Key); Found != Results.end())
	{
		return Found->second;
	}
	std::shared_ptr<const std::vector<FAnimationClip>> Result;
	if (const std::shared_ptr<const FAnimSourceModel> Source = LoadSource(SourcePath))
	{
		const FHumanoidMapping SourceMapping = AnimRetargetMath::ResolveMapping(*Source->Set, Source->Metadata.get());
		const FHumanoidMapping TargetMapping = AnimRetargetMath::ResolveMapping(Target, TargetMetadata);
		if (SourceMapping[ToIndex(Hips)] < 0 || TargetMapping[ToIndex(Hips)] < 0)
		{
			E_LOG(LogAnimation, Warning, "리타기팅: {} → {} — Hips를 찾지 못해 매핑할 수 없습니다 (모델 편집 창의 리타기팅에서 지정)", SourcePath, TargetKey);
		}
		else
		{
			const FRetargetSkeleton SourceSkeleton = AnimRetargetMath::MakeSkeleton(*Source->Set, SourceMapping);
			const FRetargetSkeleton TargetSkeleton = AnimRetargetMath::MakeSkeleton(Target, TargetMapping);
			const FRetargetPlan     Plan           = AnimRetargetMath::MakePlan(SourceSkeleton, TargetSkeleton);
			auto                    Clips          = std::make_shared<std::vector<FAnimationClip>>();
			Clips->reserve(Source->Set->Clips.size());
			for (const FAnimationClip& Clip : Source->Set->Clips)
			{
				Clips->push_back(AnimRetargetMath::RetargetClip(Clip, Plan));
			}
			E_LOG(LogAnimation, Display, "리타기팅: {} → {} (클립 {}, 뼈 {}, 키 비율 {:.3f})", SourcePath, TargetKey, Clips->size(), Plan.Order.size(),
			      Plan.HeightRatio);
			Result = std::move(Clips);
		}
	}
	Results[Key] = Result;
	return Result;
}

void FAnimRetargetLibrary::Invalidate(const std::string& AssetPath)
{
	const std::string Key = NormalizePath(AssetPath);
	Sources.erase(Key);
	std::erase_if(Results, [&](const auto& Entry) {
		const std::string& ResultKey = Entry.first;
		const size_t       Bar       = ResultKey.find('|');
		return ResultKey.substr(0, Bar) == Key || (Bar != std::string::npos && NormalizePath(ResultKey.substr(Bar + 1)) == Key);
	});
	++Generation;
}

void FAnimRetargetLibrary::Invalidate()
{
	Sources.clear();
	Results.clear();
	++Generation;
}
