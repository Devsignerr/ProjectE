#include "Physics/Physics2DSystem.h"

// 네트워크 물리 예측용 바디 상태 API (FGameWorld — World/GameWorldPhysicsPrediction2D.cpp). 3D FPhysicsSystem의 같은 이름 함수와 같은 규칙:
// 최신 스텝 상태(CurrentPosition/Angle)를 읽고 쓰며, 보정은 렌더 보간 직전 상태(Previous)도 같이 옮겨 화면 보간이 끊기지 않게 한다.

bool FPhysics2DSystem::GetBodyMotion(FEntity Entity, FPhysics2DBodyMotion& OutMotion) const
{
	const auto Found = Bodies.find(Entity);
	if (!World || Found == Bodies.end() || Found->second.CreatedDesc.Type != EBodyType2D::Dynamic)
	{
		return false;
	}
	const FBodyState& State   = Found->second;
	OutMotion.Position        = State.CurrentPosition;
	OutMotion.Angle           = State.CurrentAngle;
	OutMotion.LinearVelocity  = World->GetLinearVelocity(State.Body);
	OutMotion.AngularVelocity = World->GetAngularVelocity(State.Body);
	return true;
}

void FPhysics2DSystem::SetBodyMotion(FEntity Entity, const FPhysics2DBodyMotion& Motion)
{
	const auto Found = Bodies.find(Entity);
	if (!World || Found == Bodies.end() || Found->second.CreatedDesc.Type != EBodyType2D::Dynamic)
	{
		return;
	}
	FBodyState& State = Found->second;
	World->SetTransform(State.Body, Motion.Position, Motion.Angle);
	World->SetLinearVelocity(State.Body, Motion.LinearVelocity);
	World->SetAngularVelocity(State.Body, Motion.AngularVelocity);
	State.PreviousPosition = State.CurrentPosition = Motion.Position;
	State.PreviousAngle    = State.CurrentAngle    = Motion.Angle;
}

void FPhysics2DSystem::CorrectBody(FEntity Entity, const FVector2& DeltaPosition, float DeltaAngle, const FVector2& DeltaVelocity, float DeltaAngularVelocity)
{
	const auto Found = Bodies.find(Entity);
	if (!World || Found == Bodies.end() || Found->second.CreatedDesc.Type != EBodyType2D::Dynamic)
	{
		return;
	}
	FBodyState& State      = Found->second;
	State.CurrentPosition  = State.CurrentPosition + DeltaPosition;
	State.PreviousPosition = State.PreviousPosition + DeltaPosition;
	State.CurrentAngle += DeltaAngle;
	State.PreviousAngle += DeltaAngle;
	World->SetTransform(State.Body, State.CurrentPosition, State.CurrentAngle);
	if (DeltaVelocity.LengthSquared() > 0.0f)
	{
		World->SetLinearVelocity(State.Body, World->GetLinearVelocity(State.Body) + DeltaVelocity);
	}
	if (DeltaAngularVelocity != 0.0f)
	{
		World->SetAngularVelocity(State.Body, World->GetAngularVelocity(State.Body) + DeltaAngularVelocity);
	}
}

void FPhysics2DSystem::PoseBody(FEntity Entity, const FVector2& Position, float Angle)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end() && Found->second.CreatedDesc.Type == EBodyType2D::Dynamic)
	{
		World->SetTransform(Found->second.Body, Position, Angle);
	}
}

void FPhysics2DSystem::RestoreBodyPose(FEntity Entity)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end() && Found->second.CreatedDesc.Type == EBodyType2D::Dynamic)
	{
		World->SetTransform(Found->second.Body, Found->second.CurrentPosition, Found->second.CurrentAngle);
	}
}
