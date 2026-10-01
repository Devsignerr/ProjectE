// FPhysicsSystem의 래그돌 부분 (규칙은 Physics/Ragdoll.h 머리 주석)
#include "Core/Log.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/Ragdoll.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
	constexpr float RagdollLinearDamping  = 0.05f;
	constexpr float RagdollAngularDamping = 0.6f;  // 관절 마찰과 함께 흐느적거림을 줄인다
	constexpr float JointFrictionPerKg    = 0.15f; // N·m / kg (관절 마찰 토크 = 이 값 × 자식 캡슐 질량)
	constexpr float OverlapMargin         = 1.0f;  // cm, 처음부터 이만큼 가까운 캡슐끼리는 충돌을 끈다

	FAnimationComponent* FindAnimation(FScene& Scene, FEntity Entity)
	{
		FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(Entity);
		return Animation != nullptr && Animation->Runtime.Set && !Animation->Runtime.NodeEntities.empty() ? Animation : nullptr;
	}

	FEntity FindModelRecursive(const FScene& Scene, FEntity Entity, bool bRequireRagdollComponent)
	{
		const FRegistry& Registry = Scene.GetRegistry();
		const FAnimationComponent* Animation = Registry.TryGet<FAnimationComponent>(Entity);
		if (Animation != nullptr && Animation->Runtime.Set && (!bRequireRagdollComponent || Registry.Has<FRagdollComponent>(Entity)))
		{
			return Entity;
		}
		for (const FEntity Child : Scene.GetChildren(Entity))
		{
			if (const FEntity Found = FindModelRecursive(Scene, Child, bRequireRagdollComponent); Found.IsValid())
			{
				return Found;
			}
		}
		return NullEntity;
	}

	float CapsuleVolume(float Radius, float HalfHeight)
	{
		return FMath::Pi * Radius * Radius * (2.0f * HalfHeight) + (4.0f / 3.0f) * FMath::Pi * Radius * Radius * Radius;
	}
} // namespace

FEntity FPhysicsSystem::FindRagdollModel(const FScene& Scene, FEntity Entity)
{
	if (!Entity.IsValid() || !Scene.GetRegistry().IsValid(Entity))
	{
		return NullEntity;
	}
	const FEntity WithComponent = FindModelRecursive(Scene, Entity, true); // 래그돌 설정이 있는 모델을 먼저
	return WithComponent.IsValid() ? WithComponent : FindModelRecursive(Scene, Entity, false);
}

bool FPhysicsSystem::IsRagdollActive(const FScene& Scene, FEntity Entity) const
{
	return Ragdolls.contains(FindRagdollModel(Scene, Entity));
}

uint32 FPhysicsSystem::GetRagdollPartCount(const FScene& Scene, FEntity Entity) const
{
	const auto Found = Ragdolls.find(FindRagdollModel(Scene, Entity));
	return Found != Ragdolls.end() ? static_cast<uint32>(Found->second.Parts.size()) : 0u;
}

bool FPhysicsSystem::EnableRagdoll(FScene& Scene, FEntity Entity)
{
	const FEntity Model = FindRagdollModel(Scene, Entity);
	if (!World || !Model.IsValid() || Ragdolls.contains(Model))
	{
		return false;
	}
	FAnimationComponent* Animation = FindAnimation(Scene, Model);
	if (Animation == nullptr)
	{
		return false;
	}
	FRegistry&                  Registry = Scene.GetRegistry();
	const FRagdollComponent     Defaults;
	const FRagdollComponent*    Found    = Registry.TryGet<FRagdollComponent>(Model);
	const FRagdollComponent&    Settings = Found != nullptr ? *Found : Defaults;
	const std::vector<FEntity>& Nodes    = Animation->Runtime.NodeEntities;
	const std::vector<int32>&   Parents  = Animation->Runtime.Set->NodeParents;
	const int32                 Count    = static_cast<int32>(Nodes.size());

	// 스킨 뼈 (이 모델의 노드)
	std::unordered_map<FEntity, int32> NodeIndex;
	for (int32 Node = 0; Node < Count; ++Node)
	{
		if (Registry.IsValid(Nodes[Node]))
		{
			NodeIndex[Nodes[Node]] = Node;
		}
	}
	std::vector<FRagdollBoneInput> Bones(Count);
	Registry.View<FSkinComponent>().Each([&](FEntity, FSkinComponent& Skin) {
		for (const FEntity Joint : Skin.Joints)
		{
			if (const auto It = NodeIndex.find(Joint); It != NodeIndex.end())
			{
				Bones[It->second].bJoint = true;
			}
		}
	});
	for (int32 Node = 0; Node < Count; ++Node)
	{
		Bones[Node].Parent = Node < static_cast<int32>(Parents.size()) ? Parents[Node] : -1;
		if (!Registry.IsValid(Nodes[Node]))
		{
			Bones[Node].bJoint = false;
			continue;
		}
		Bones[Node].Position = Scene.GetTransform(Nodes[Node]).GetWorldPosition();
		if (const FNameComponent* Name = Registry.TryGet<FNameComponent>(Nodes[Node]); Name != nullptr && !Settings.ExcludeBones.empty())
		{
			Bones[Node].bExclude = RagdollMath::MatchesExclude(Name->Name, Settings.ExcludeBones);
		}
	}
	const std::vector<FRagdollPartLayout> Layout = RagdollMath::BuildLayout(Bones, Settings);
	if (Layout.empty())
	{
		E_LOG(LogPhysics, Warning, "래그돌을 만들 뼈가 없습니다 (스킨 뼈 {}개 중 캡슐 0)", Count);
		return false;
	}

	// 질량은 캡슐 부피 비율
	std::vector<float> HalfHeights(Layout.size());
	float              TotalVolume = 0.0f;
	for (size_t Index = 0; Index < Layout.size(); ++Index)
	{
		const float Length  = FVector3::Distance(Layout[Index].Start, Layout[Index].End);
		HalfHeights[Index]  = std::max(0.5f * Length - Layout[Index].Radius, 0.1f);
		TotalVolume        += CapsuleVolume(Layout[Index].Radius, HalfHeights[Index]);
	}

	// 처음 속도: 주인(조상 중 바디/캐릭터가 있는 엔티티) 속도
	FVector3             InitialVelocity;
	std::vector<uint32>  OwnerBodies;
	for (FEntity Current = Model; Current.IsValid() && Registry.IsValid(Current); Current = Scene.GetParent(Current))
	{
		if (const auto Body = Bodies.find(Current); Body != Bodies.end())
		{
			OwnerBodies.push_back(Body->second.Body);
		}
		else if (const auto Character = Characters.find(Current); Character != Characters.end())
		{
			OwnerBodies.push_back(World->GetCharacterInnerBody(Character->second.Character));
		}
		else
		{
			continue;
		}
		if (InitialVelocity.LengthSquared() == 0.0f)
		{
			InitialVelocity = GetVelocity(Current);
		}
	}

	FRagdollState State;
	State.PartOfNode.assign(Count, -1);
	const float TotalMass = std::max(Settings.Mass, 0.1f);
	for (size_t Index = 0; Index < Layout.size(); ++Index)
	{
		const FRagdollPartLayout& Part      = Layout[Index];
		const FVector3            Direction = (Part.End - Part.Start).GetNormalized();
		FPhysicsBodyDesc          Desc;
		Desc.MotionType        = EPhysicsMotionType::Dynamic;
		Desc.Shape             = EPhysicsShape::Capsule;
		Desc.Radius            = Part.Radius;
		Desc.HalfHeight        = HalfHeights[Index];
		Desc.Position          = (Part.Start + Part.End) * 0.5f;
		Desc.Rotation          = RagdollMath::RotationFromZ(Direction);
		Desc.Mass              = std::max(TotalMass * CapsuleVolume(Part.Radius, HalfHeights[Index]) / std::max(TotalVolume, 1.0e-6f), 0.05f);
		Desc.Friction          = Settings.Friction;
		Desc.LinearDamping     = RagdollLinearDamping;
		Desc.AngularDamping    = RagdollAngularDamping;
		Desc.RollingResistance = 0.0f;
		Desc.UserData          = Nodes[Part.Node].ToId(); // 레이캐스트 = 뼈 엔티티
		FRagdollPart Created;
		Created.Node = Part.Node;
		Created.Body = World->CreateBody(Desc);
		if (Created.Body == FPhysicsWorld::InvalidBody)
		{
			DestroyRagdollBodies(State);
			return false;
		}
		World->SetLinearVelocity(Created.Body, InitialVelocity);

		FVector3 BonePosition;
		FQuat    BoneRotation;
		PhysicsMath::DecomposeWorld(Scene.GetTransform(Nodes[Part.Node]).WorldMatrix, BonePosition, BoneRotation, Created.BoneScale);
		const FMatrix4x4 BoneRigid = FMatrix4x4::MakeTransform(BonePosition, BoneRotation, FVector3::OneVector);
		const FMatrix4x4 BodyWorld = FMatrix4x4::MakeTransform(Desc.Position, Desc.Rotation, FVector3::OneVector);
		Created.BoneFromBody       = BoneRigid * BodyWorld.GetInverse();
		Created.PreviousPosition = Created.CurrentPosition = Desc.Position;
		Created.PreviousRotation = Created.CurrentRotation = Desc.Rotation;
		State.PartOfNode[Part.Node] = static_cast<int32>(State.Parts.size());
		State.Parts.push_back(Created);
	}

	// 관절 (부모 캡슐 ↔ 자식 캡슐, 뼈 시작점) + 충돌 끄기 (이웃은 관절이 끈다, 처음 겹친 쌍, 주인 바디)
	for (size_t Index = 0; Index < Layout.size(); ++Index)
	{
		const FRagdollPartLayout& Part = Layout[Index];
		if (Part.ParentPart < 0)
		{
			continue;
		}
		const FVector3         Direction = (Part.End - Part.Start).GetNormalized();
		FPhysicsConstraintDesc Joint;
		Joint.Type           = EPhysicsConstraintType::SwingTwist;
		Joint.Body1          = State.Parts[Part.ParentPart].Body;
		Joint.Body2          = State.Parts[Index].Body;
		Joint.Point1         = Part.Start;
		Joint.Axis           = Direction;
		Joint.ConeHalfAngle  = FMath::DegreesToRadians(FMath::Clamp(Settings.SwingLimit, 0.0f, 180.0f));
		Joint.TwistMin       = -FMath::DegreesToRadians(FMath::Clamp(Settings.TwistLimit, 0.0f, 180.0f));
		Joint.TwistMax       = -Joint.TwistMin;
		Joint.FrictionTorque = JointFrictionPerKg * World->GetMass(Joint.Body2);
		if (const uint32 Constraint = World->CreateConstraint(Joint); Constraint != FPhysicsWorld::InvalidBody)
		{
			State.Constraints.push_back(Constraint);
		}
	}
	for (size_t A = 0; A < Layout.size(); ++A)
	{
		for (size_t B = A + 1; B < Layout.size(); ++B)
		{
			const float Gap = RagdollMath::SegmentDistance(Layout[A].Start, Layout[A].End, Layout[B].Start, Layout[B].End);
			if (Gap < Layout[A].Radius + Layout[B].Radius + OverlapMargin)
			{
				World->DisableCollision(State.Parts[A].Body, State.Parts[B].Body);
			}
		}
		for (const uint32 Owner : OwnerBodies)
		{
			World->DisableCollision(State.Parts[A].Body, Owner);
		}
	}

	// 되돌릴 로컬 트랜스폼 + 부모 먼저 순서
	State.Saved.resize(Count);
	std::vector<int32> Depth(Count, 0);
	for (int32 Node = 0; Node < Count; ++Node)
	{
		if (Registry.IsValid(Nodes[Node]))
		{
			const FTransformComponent& Transform = Scene.GetTransform(Nodes[Node]);
			State.Saved[Node]                    = { Transform.Position, Transform.Rotation, Transform.Scale };
		}
		int32 Steps = 0;
		for (int32 Current = Node; Current >= 0 && Current < Count && Steps <= Count; Current = Parents.size() > static_cast<size_t>(Current) ? Parents[Current] : -1)
		{
			++Steps;
		}
		Depth[Node] = Steps;
		State.NodeOrder.push_back(Node);
	}
	std::stable_sort(State.NodeOrder.begin(), State.NodeOrder.end(), [&](int32 A, int32 B) { return Depth[A] < Depth[B]; });

	Animation->Runtime.bPhysicsPose = true;
	E_LOG(LogPhysics, Display, "래그돌 켜짐: 캡슐 {}개, 관절 {}개, {:.1f}kg", State.Parts.size(), State.Constraints.size(), TotalMass);
	Ragdolls.emplace(Model, std::move(State));
	return true;
}

void FPhysicsSystem::DestroyRagdollBodies(FRagdollState& State)
{
	if (!World)
	{
		return;
	}
	for (const uint32 Constraint : State.Constraints)
	{
		World->DestroyConstraint(Constraint); // 바디보다 먼저
	}
	for (const FRagdollPart& Part : State.Parts)
	{
		World->DestroyBody(Part.Body); // 충돌 끄기 항목도 함께 사라진다
	}
	State.Constraints.clear();
	State.Parts.clear();
}

void FPhysicsSystem::DisableRagdoll(FScene& Scene, FEntity Entity)
{
	const FEntity Model = FindRagdollModel(Scene, Entity);
	const auto    Found = Ragdolls.find(Model);
	if (Found == Ragdolls.end())
	{
		return;
	}
	FRagdollState& State = Found->second;
	DestroyRagdollBodies(State);
	if (FAnimationComponent* Animation = FindAnimation(Scene, Model))
	{
		const std::vector<FEntity>& Nodes = Animation->Runtime.NodeEntities;
		for (size_t Node = 0; Node < Nodes.size() && Node < State.Saved.size(); ++Node)
		{
			if (Scene.GetRegistry().IsValid(Nodes[Node]))
			{
				FTransformComponent& Transform = Scene.GetTransform(Nodes[Node]);
				Transform.Position             = State.Saved[Node].Position;
				Transform.Rotation             = State.Saved[Node].Rotation;
				Transform.Scale                = State.Saved[Node].Scale;
			}
		}
		Animation->Runtime.bPhysicsPose = false; // 다음 애니메이션 갱신부터 다시 포즈를 쓴다
	}
	Ragdolls.erase(Found);
	E_LOG(LogPhysics, Display, "래그돌 꺼짐");
}

void FPhysicsSystem::SyncRagdolls(FScene& Scene)
{
	for (auto It = Ragdolls.begin(); It != Ragdolls.end();)
	{
		if (FindAnimation(Scene, It->first) == nullptr)
		{
			DestroyRagdollBodies(It->second); // 모델이 사라짐
			It = Ragdolls.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FPhysicsSystem::WriteRagdollPoses(FScene& Scene)
{
	const float Alpha = bInterpolate ? Stepper.GetAlpha() : 1.0f;
	for (auto& [Model, State] : Ragdolls)
	{
		FAnimationComponent* Animation = FindAnimation(Scene, Model);
		if (Animation == nullptr)
		{
			continue;
		}
		const FRegistry&            Registry = Scene.GetRegistry();
		const std::vector<FEntity>& Nodes    = Animation->Runtime.NodeEntities;
		const std::vector<int32>&   Parents  = Animation->Runtime.Set->NodeParents;
		std::vector<FMatrix4x4>     NewWorld(Nodes.size());
		std::vector<uint8>          bHasWorld(Nodes.size(), 0);
		for (const int32 Node : State.NodeOrder)
		{
			if (Node < 0 || Node >= static_cast<int32>(Nodes.size()) || !Registry.IsValid(Nodes[Node]))
			{
				continue;
			}
			const int32       Parent      = Node < static_cast<int32>(Parents.size()) ? Parents[Node] : -1;
			const FMatrix4x4  ParentWorld = Parent >= 0 && bHasWorld[Parent] ? NewWorld[Parent] : Scene.GetParentWorldMatrix(Nodes[Node]);
			FTransformComponent& Transform = Scene.GetTransform(Nodes[Node]);
			const int32       PartIndex   = Node < static_cast<int32>(State.PartOfNode.size()) ? State.PartOfNode[Node] : -1;
			if (PartIndex >= 0)
			{
				const FRagdollPart& Part      = State.Parts[PartIndex];
				const FMatrix4x4    BodyWorld = FMatrix4x4::MakeTransform(FVector3::Lerp(Part.PreviousPosition, Part.CurrentPosition, Alpha),
				                                                          FQuat::Slerp(Part.PreviousRotation, Part.CurrentRotation, Alpha).GetNormalized(),
				                                                          FVector3::OneVector);
				FVector3 BonePosition;
				FQuat    BoneRotation;
				FVector3 Unused;
				PhysicsMath::DecomposeWorld(Part.BoneFromBody * BodyWorld, BonePosition, BoneRotation, Unused);
				NewWorld[Node]         = FMatrix4x4::MakeTransform(BonePosition, BoneRotation, Part.BoneScale);
				const FMatrix4x4 Local = NewWorld[Node] * ParentWorld.GetInverse();
				Local.Decompose(Transform.Position, Transform.Rotation, Transform.Scale);
				Transform.Rotation.Normalize();
			}
			else
			{
				NewWorld[Node] = Transform.GetLocalMatrix() * ParentWorld; // 캡슐 없는 뼈: 부모를 따라간다
			}
			bHasWorld[Node] = 1;
		}
	}
}
