#include "Editor/Editor2D/Collider2DShapes.h"

#include "Editor/Editor2D/Editor2DMath.h"
#include "Physics/CharacterMovement2D.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DMath.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr int32 GCircleSegments = 32;

	// 바디 자세 (물리와 같은 분해)
	struct FBodyFrame
	{
		FVector3 Position;
		FVector3 Scale = FVector3::OneVector;
		float    Angle = 0.0f; // 라디안, 반시계 +

		FVector3 ToWorld(const FVector2& BodyPoint) const
		{
			const FVector2 Rotated = Physics2DMath::Rotate(BodyPoint, Angle);
			return FVector3(Position.X + Rotated.X, Position.Y, Position.Z + Rotated.Y);
		}
	};

	FBodyFrame MakeFrame(const FScene& Scene, FEntity Entity)
	{
		FBodyFrame Frame;
		FQuat      Rotation;
		Scene.GetTransform(Entity).WorldMatrix.Decompose(Frame.Position, Rotation, Frame.Scale);
		Frame.Angle = Physics2DMath::AngleFromRotation(Rotation);
		return Frame;
	}

	FVector2 ScalePoint(const FVector2& Point, const FVector3& Scale) { return FVector2(Point.X * Scale.X, Point.Y * Scale.Z); }

	// 세로 캡슐 (가운데 Center, 반원 반지름 Radius, 원기둥 반 길이 HalfSegment — 0이면 원), 바디 공간
	void AddCapsule(std::vector<FVector2>& Out, const FVector2& Center, float Radius, float HalfSegment)
	{
		constexpr int32 Half = GCircleSegments / 2;
		for (int32 Index = 0; Index <= Half; ++Index) // 위 반원 (오른쪽 → 왼쪽)
		{
			const float Angle = FMath::Pi * static_cast<float>(Index) / static_cast<float>(Half);
			Out.push_back(Center + FVector2(FMath::Cos(Angle) * Radius, HalfSegment + FMath::Sin(Angle) * Radius));
		}
		for (int32 Index = 0; Index <= Half; ++Index) // 아래 반원 (왼쪽 → 오른쪽)
		{
			const float Angle = FMath::Pi + FMath::Pi * static_cast<float>(Index) / static_cast<float>(Half);
			Out.push_back(Center + FVector2(FMath::Cos(Angle) * Radius, -HalfSegment + FMath::Sin(Angle) * Radius));
		}
	}

	template <typename TCollider>
	Collider2DShapes::FOutline MakeOutline(const FBodyFrame& Frame, const std::vector<FVector2>& BodyPoints, bool bClosed, const TCollider& Collider)
	{
		Collider2DShapes::FOutline Outline;
		Outline.Points.reserve(BodyPoints.size());
		for (const FVector2& Point : BodyPoints)
		{
			Outline.Points.push_back(Frame.ToWorld(Point));
		}
		Outline.bClosed  = bClosed;
		Outline.bTrigger = Collider.bIsTrigger;
		Outline.bOneWay  = Collider.bOneWay;
		return Outline;
	}
} // namespace

bool Collider2DShapes::HasShapes(const FScene& Scene, FEntity Entity)
{
	const FRegistry& Registry = Scene.GetRegistry();
	return Registry.Has<FBoxCollider2DComponent>(Entity) || Registry.Has<FCircleCollider2DComponent>(Entity) ||
	       Registry.Has<FCapsuleCollider2DComponent>(Entity) || Registry.Has<FPolygonCollider2DComponent>(Entity) ||
	       Registry.Has<FEdgeCollider2DComponent>(Entity) || Registry.Has<FCharacterMovement2DComponent>(Entity);
}

void Collider2DShapes::Collect(const FScene& Scene, FEntity Entity, std::vector<FOutline>& Out)
{
	const FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Entity) || !Registry.Has<FTransformComponent>(Entity) || !HasShapes(Scene, Entity))
	{
		return;
	}
	const FBodyFrame Frame = MakeFrame(Scene, Entity);
	const float      AbsX  = std::abs(Frame.Scale.X);
	const float      AbsZ  = std::abs(Frame.Scale.Z);

	if (const FBoxCollider2DComponent* Box = Registry.TryGet<FBoxCollider2DComponent>(Entity))
	{
		const FVector2 Half(std::abs(Box->Size.X) * 0.5f * AbsX, std::abs(Box->Size.Y) * 0.5f * AbsZ);
		const FVector2 Offset = ScalePoint(Box->Offset, Frame.Scale);
		const float    Angle  = FMath::DegreesToRadians(Box->Angle);
		const FVector2 Corners[4] = { FVector2(-Half.X, -Half.Y), FVector2(Half.X, -Half.Y), FVector2(Half.X, Half.Y), FVector2(-Half.X, Half.Y) };
		std::vector<FVector2> Points;
		for (const FVector2& Corner : Corners)
		{
			Points.push_back(Offset + Physics2DMath::Rotate(Corner, Angle));
		}
		Out.push_back(MakeOutline(Frame, Points, true, *Box));
	}
	if (const FCircleCollider2DComponent* Circle = Registry.TryGet<FCircleCollider2DComponent>(Entity))
	{
		const float    Radius = std::abs(Circle->Radius) * std::max(AbsX, AbsZ);
		const FVector2 Offset = ScalePoint(Circle->Offset, Frame.Scale);
		std::vector<FVector2> Points;
		for (int32 Index = 0; Index < GCircleSegments; ++Index)
		{
			const float Angle = FMath::TwoPi * static_cast<float>(Index) / static_cast<float>(GCircleSegments);
			Points.push_back(Offset + FVector2(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
		}
		Points.push_back(Offset); // 회전이 보이게 반지름 선 하나 (닫힌 다각형 마지막 → 가운데 → 처음)
		Out.push_back(MakeOutline(Frame, Points, true, *Circle));
	}
	if (const FCapsuleCollider2DComponent* Capsule = Registry.TryGet<FCapsuleCollider2DComponent>(Entity))
	{
		const float Radius      = std::abs(Capsule->Radius) * AbsX;
		const float HalfSegment = std::max(std::abs(Capsule->Height) * AbsZ * 0.5f - Radius, 0.0f);
		std::vector<FVector2> Points;
		AddCapsule(Points, ScalePoint(Capsule->Offset, Frame.Scale), Radius, HalfSegment);
		Out.push_back(MakeOutline(Frame, Points, true, *Capsule));
	}
	if (const FPolygonCollider2DComponent* Polygon = Registry.TryGet<FPolygonCollider2DComponent>(Entity))
	{
		std::vector<FVector2> Parsed;
		Physics2DMath::ParsePoints(Polygon->Points, Parsed); // 오류면 읽은 데까지
		const FVector2 Offset = ScalePoint(Polygon->Offset, Frame.Scale);
		for (FVector2& Point : Parsed)
		{
			Point = Offset + ScalePoint(Point, Frame.Scale);
		}
		if (Parsed.size() >= 2)
		{
			Out.push_back(MakeOutline(Frame, Parsed, Parsed.size() >= 3, *Polygon));
		}
	}
	if (const FEdgeCollider2DComponent* Edge = Registry.TryGet<FEdgeCollider2DComponent>(Entity))
	{
		std::vector<FVector2> Parsed;
		Physics2DMath::ParsePoints(Edge->Points, Parsed);
		const FVector2 Offset = ScalePoint(Edge->Offset, Frame.Scale);
		for (FVector2& Point : Parsed)
		{
			Point = Offset + ScalePoint(Point, Frame.Scale);
		}
		if (Parsed.size() >= 2)
		{
			Out.push_back(MakeOutline(Frame, Parsed, Edge->bLoop && Parsed.size() >= 3, *Edge));
		}
	}
	if (const FCharacterMovement2DComponent* Character = Registry.TryGet<FCharacterMovement2DComponent>(Entity))
	{
		const float Radius      = std::max(Character->CapsuleRadius, 0.0f);
		const float HalfSegment = std::max(Character->CapsuleHeight * 0.5f - Radius, 0.0f);
		std::vector<FVector2> Points;
		AddCapsule(Points, FVector2::ZeroVector, Radius, HalfSegment);
		FOutline Outline;
		for (const FVector2& Point : Points)
		{
			Outline.Points.push_back(FVector3(Frame.Position.X + Point.X, Frame.Position.Y, Frame.Position.Z + Point.Y));
		}
		Outline.Kind = EKind::Character;
		Out.push_back(std::move(Outline));
	}
}

bool Collider2DShapes::Contains(const FOutline& Outline, const FVector2& PlanePoint, float Tolerance)
{
	std::vector<FVector2> Plane;
	Plane.reserve(Outline.Points.size());
	for (const FVector3& Point : Outline.Points)
	{
		Plane.push_back(FVector2(Point.X, Point.Z));
	}
	if (Outline.bClosed && Editor2DMath::IsPointInPolygon(Plane, PlanePoint))
	{
		return true;
	}
	const size_t Count = Plane.size();
	for (size_t Index = 0; Index + 1 < Count + (Outline.bClosed ? 1u : 0u); ++Index)
	{
		if (Editor2DMath::DistanceToSegment(Plane[Index], Plane[(Index + 1) % Count], PlanePoint) <= Tolerance)
		{
			return true;
		}
	}
	return false;
}
