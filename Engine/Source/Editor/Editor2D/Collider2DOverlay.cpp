#include <imgui.h>

#include "Editor/Editor2D/Collider2DOverlay.h"

#include "Editor/Editor2D/Collider2DShapes.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/PhysicsComponents.h"
#include "Renderer/Camera.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
	ImU32 MakeColor(const ImVec4& Color, float Alpha)
	{
		return ImGui::ColorConvertFloat4ToU32(ImVec4(Color.x, Color.y, Color.z, Alpha));
	}

	// 월드 → 화면 투영 + 선 그리기 (원근이면 카메라 뒤 점은 버린다)
	struct FProjector
	{
		FMatrix4x4  ViewProjection;
		ImVec2      ImagePosition;
		ImVec2      ImageSize;
		ImDrawList* DrawList = nullptr;

		bool Project(const FVector3& P, ImVec2& Out) const
		{
			const FVector4 Clip = ViewProjection.TransformVector4(FVector4(P, 1.0f));
			if (Clip.W <= 1.0e-4f)
			{
				return false;
			}
			Out = ImVec2(ImagePosition.x + (Clip.X / Clip.W * 0.5f + 0.5f) * ImageSize.x, ImagePosition.y + (0.5f - Clip.Y / Clip.W * 0.5f) * ImageSize.y);
			return true;
		}
		void Line(const FVector3& A, const FVector3& B, ImU32 Color, float Thickness = 1.5f) const
		{
			ImVec2 SA;
			ImVec2 SB;
			if (Project(A, SA) && Project(B, SB))
			{
				DrawList->AddLine(SA, SB, Color, Thickness);
			}
		}
		void Polyline(const std::vector<FVector3>& Points, bool bClosed, ImU32 Color, float Thickness = 1.5f) const
		{
			for (size_t Index = 0; Index + 1 < Points.size(); ++Index)
			{
				Line(Points[Index], Points[Index + 1], Color, Thickness);
			}
			if (bClosed && Points.size() > 2)
			{
				Line(Points.back(), Points.front(), Color, Thickness);
			}
		}
		// 화면 픽셀 길이의 화살표 (월드 Base에서 월드 방향 Direction 쪽으로)
		void Arrow(const FVector3& Base, const FVector3& Direction, float LengthPixels, ImU32 Color) const
		{
			ImVec2 SA;
			ImVec2 SB;
			if (!Project(Base, SA) || !Project(Base + Direction, SB))
			{
				return;
			}
			ImVec2      Delta(SB.x - SA.x, SB.y - SA.y);
			const float Length = std::sqrt(Delta.x * Delta.x + Delta.y * Delta.y);
			if (Length < 1.0e-3f)
			{
				return;
			}
			Delta             = ImVec2(Delta.x / Length, Delta.y / Length);
			const ImVec2 Tip(SA.x + Delta.x * LengthPixels, SA.y + Delta.y * LengthPixels);
			const ImVec2 Side(-Delta.y * LengthPixels * 0.3f, Delta.x * LengthPixels * 0.3f);
			const ImVec2 Back(Tip.x - Delta.x * LengthPixels * 0.4f, Tip.y - Delta.y * LengthPixels * 0.4f);
			DrawList->AddLine(SA, Tip, Color, 1.5f);
			DrawList->AddLine(Tip, ImVec2(Back.x + Side.x, Back.y + Side.y), Color, 1.5f);
			DrawList->AddLine(Tip, ImVec2(Back.x - Side.x, Back.y - Side.y), Color, 1.5f);
		}
		void Cross(const FVector3& Point, float SizePixels, ImU32 Color) const
		{
			ImVec2 S;
			if (Project(Point, S))
			{
				DrawList->AddLine(ImVec2(S.x - SizePixels, S.y), ImVec2(S.x + SizePixels, S.y), Color, 2.0f);
				DrawList->AddLine(ImVec2(S.x, S.y - SizePixels), ImVec2(S.x, S.y + SizePixels), Color, 2.0f);
			}
		}
		void ScreenCircle(const FVector3& Center, float RadiusPixels, ImU32 Color) const
		{
			ImVec2 S;
			if (Project(Center, S))
			{
				DrawList->AddCircle(S, RadiusPixels, Color, 24, 1.5f);
			}
		}
		void Circle(const FVector3& Center, const FVector3& U, const FVector3& V, float Radius, ImU32 Color) const
		{
			constexpr int32 Segments = 32;
			FVector3        Previous = Center + U * Radius;
			for (int32 Index = 1; Index <= Segments; ++Index)
			{
				const float    Angle = FMath::TwoPi * static_cast<float>(Index) / static_cast<float>(Segments);
				const FVector3 Point = Center + (U * FMath::Cos(Angle) + V * FMath::Sin(Angle)) * Radius;
				Line(Previous, Point, Color);
				Previous = Point;
			}
		}
	};

	FVector3 PlaneLocalToWorld(const FMatrix4x4& World, const FVector2& Local) { return World.TransformPosition(FVector3(Local.X, 0.0f, Local.Y)); }

	// 2D 관절 그리기 (Target 쪽 점 = 대상 원점, 대상이 없으면 앵커 자리)
	template <typename TJoint>
	void DrawJoint2D(const FProjector& Projector, const FRegistry& Registry, const FMatrix4x4& World, const TJoint& Joint, ImU32 Color, ImU32 Faint,
	                 FVector3& OutAnchor)
	{
		OutAnchor = PlaneLocalToWorld(World, Joint.Anchor);
		Projector.Cross(OutAnchor, 6.0f, Color);
		if (Joint.Target.IsValid() && Registry.IsValid(Joint.Target))
		{
			if (const FTransformComponent* Target = Registry.TryGet<FTransformComponent>(Joint.Target))
			{
				Projector.Line(OutAnchor, Target->WorldMatrix.GetOrigin(), Faint);
			}
		}
	}

	void DrawAxis2D(const FProjector& Projector, const FMatrix4x4& World, const FVector3& Anchor, const FVector2& Axis, ImU32 Color)
	{
		FVector3 Direction = World.TransformVector(FVector3(Axis.X, 0.0f, Axis.Y));
		if (Direction.IsNearlyZero())
		{
			return;
		}
		Direction = Direction.GetNormalized() * 60.0f;
		Projector.Line(Anchor - Direction, Anchor + Direction, Color);
	}
} // namespace

void FCollider2DOverlay::Draw(FEditorContext& Context, const FVector2& ImagePosition, const FVector2& ImageSize, bool bAll)
{
	++Frame;
	FScene&    Scene    = *Context.Scene;
	FRegistry& Registry = Scene.GetRegistry();

	FProjector Projector;
	Projector.ViewProjection = Context.Camera->GetViewProjectionMatrix();
	Projector.ImagePosition  = ImVec2(ImagePosition.X, ImagePosition.Y);
	Projector.ImageSize      = ImVec2(ImageSize.X, ImageSize.Y);
	Projector.DrawList       = ImGui::GetWindowDrawList();

	const ImU32 SolidColor     = MakeColor(FEditorTheme::Success, 0.95f);
	const ImU32 TriggerColor   = MakeColor(FEditorTheme::Success, 0.45f);
	const ImU32 OneWayColor    = MakeColor(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), 0.95f);
	const ImU32 CharacterColor = MakeColor(FEditorTheme::PrefabText, 0.95f);
	const ImU32 JointColor     = MakeColor(FEditorTheme::Accent, 0.95f);
	const ImU32 JointFaint     = MakeColor(FEditorTheme::Accent, 0.5f);

	std::vector<FEntity> Targets;
	if (bAll)
	{
		Registry.View<FTransformComponent>().Each([&](FEntity Entity, FTransformComponent&) { Targets.push_back(Entity); });
	}
	else
	{
		Targets = Context.Selection.GetEntities();
	}

	Projector.DrawList->PushClipRect(Projector.ImagePosition, ImVec2(ImagePosition.X + ImageSize.X, ImagePosition.Y + ImageSize.Y), true);
	std::vector<Collider2DShapes::FOutline> Outlines;
	for (const FEntity Entity : Targets)
	{
		if (!Registry.IsValid(Entity) || !Registry.Has<FTransformComponent>(Entity))
		{
			continue;
		}
		const FMatrix4x4& World = Scene.GetTransform(Entity).WorldMatrix;

		// ---- 2D 콜라이더 / 이동기 캡슐
		Outlines.clear();
		Collider2DShapes::Collect(Scene, Entity, Outlines);
		for (const Collider2DShapes::FOutline& Outline : Outlines)
		{
			const ImU32 Color = Outline.Kind == Collider2DShapes::EKind::Character ? CharacterColor
			                    : Outline.bOneWay                                  ? OneWayColor
			                    : Outline.bTrigger                                 ? TriggerColor
			                                                                       : SolidColor;
			Projector.Polyline(Outline.Points, Outline.bClosed, Color);
			if (Outline.bOneWay && !Outline.Points.empty())
			{
				FVector3 Center;
				for (const FVector3& Point : Outline.Points)
				{
					Center += Point;
				}
				Center = Center / static_cast<float>(Outline.Points.size());
				Projector.Arrow(Center, World.TransformVector(FVector3::UpVector), 18.0f, OneWayColor);
			}
		}

		// ---- 타일맵 충돌 (바뀔 때만 다시 만든다)
		if (FTilemapComponent* Tilemap = Registry.TryGet<FTilemapComponent>(Entity); Tilemap != nullptr && Tilemap->bCollision)
		{
			const FTilemapData&                        Data    = Sprite2DRuntime::GetTilemapData(*Tilemap);
			const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(*Tilemap);
			if (Tileset != nullptr)
			{
				FTilemapCache& Cache    = TilemapCaches[Entity.ToId()];
				const FVector2 CellSize = TilemapCollision::ResolveCellSize(*Tileset, Tilemap->CellSize);
				if (Cache.LastFrame == 0 || Cache.Revision != Tilemap->Runtime.Revision || Cache.Tileset != Tileset.get() || !(Cache.CellSize == CellSize))
				{
					Cache.Shapes   = TilemapCollision::BuildShapes(Data, *Tileset, CellSize);
					Cache.Revision = Tilemap->Runtime.Revision;
					Cache.Tileset  = Tileset.get();
					Cache.CellSize = CellSize;
				}
				Cache.LastFrame = Frame;
				std::vector<FVector3> Points;
				const auto            ToWorld = [&](const std::vector<FVector2>& Local) {
                    Points.clear();
                    for (const FVector2& Point : Local)
                    {
                        Points.push_back(PlaneLocalToWorld(World, Point));
                    }
                    return Points;
				};
				for (const FTileCollisionOutline& Outline : Cache.Shapes.Outlines)
				{
					Projector.Polyline(ToWorld(Outline.Points), true, SolidColor);
				}
				for (const FTileCollisionPolygon& Polygon : Cache.Shapes.Polygons)
				{
					Projector.Polyline(ToWorld(Polygon.Points), true, SolidColor);
				}
				for (const FTileCollisionPolygon& Polygon : Cache.Shapes.OneWayPolygons)
				{
					Projector.Polyline(ToWorld(Polygon.Points), true, OneWayColor);
				}
				const FVector3 Up = World.TransformVector(FVector3::UpVector);
				for (const FTileCollisionSegment& Segment : Cache.Shapes.OneWaySegments)
				{
					const FVector3 Start = PlaneLocalToWorld(World, Segment.Start);
					const FVector3 End   = PlaneLocalToWorld(World, Segment.End);
					Projector.Line(Start, End, OneWayColor, 2.0f);
					// 위 화살표: 셀 폭마다 하나 (최대 64개)
					const float Length = std::abs(Segment.End.X - Segment.Start.X);
					const int32 Count  = std::clamp(static_cast<int32>(Length / std::max(CellSize.X, 1.0f)), 1, 64);
					for (int32 Index = 0; Index < Count; ++Index)
					{
						const float Alpha = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Count);
						Projector.Arrow(Start + (End - Start) * Alpha, Up, 10.0f, OneWayColor);
					}
				}
			}
		}

		// ---- 2D 관절
		FVector3 Anchor;
		if (const FDistanceJoint2DComponent* Joint = Registry.TryGet<FDistanceJoint2DComponent>(Entity))
		{
			Anchor = PlaneLocalToWorld(World, Joint->Anchor);
			Projector.Cross(Anchor, 6.0f, JointColor);
			FVector3 Other(Joint->TargetAnchor.X, World.GetOrigin().Y, Joint->TargetAnchor.Y); // 대상 없음 = 월드 평면 위치
			if (Joint->Target.IsValid() && Registry.IsValid(Joint->Target))
			{
				if (const FTransformComponent* Target = Registry.TryGet<FTransformComponent>(Joint->Target))
				{
					Other = PlaneLocalToWorld(Target->WorldMatrix, Joint->TargetAnchor);
				}
			}
			Projector.Line(Anchor, Other, JointColor);
			Projector.Cross(Other, 4.0f, JointFaint);
		}
		if (const FRevoluteJoint2DComponent* Joint = Registry.TryGet<FRevoluteJoint2DComponent>(Entity))
		{
			DrawJoint2D(Projector, Registry, World, *Joint, JointColor, JointFaint, Anchor);
			Projector.ScreenCircle(Anchor, 9.0f, JointColor);
		}
		if (const FPrismaticJoint2DComponent* Joint = Registry.TryGet<FPrismaticJoint2DComponent>(Entity))
		{
			DrawJoint2D(Projector, Registry, World, *Joint, JointColor, JointFaint, Anchor);
			DrawAxis2D(Projector, World, Anchor, Joint->Axis, JointColor);
		}
		if (const FWeldJoint2DComponent* Joint = Registry.TryGet<FWeldJoint2DComponent>(Entity))
		{
			DrawJoint2D(Projector, Registry, World, *Joint, JointColor, JointFaint, Anchor);
		}
		if (const FWheelJoint2DComponent* Joint = Registry.TryGet<FWheelJoint2DComponent>(Entity))
		{
			DrawJoint2D(Projector, Registry, World, *Joint, JointColor, JointFaint, Anchor);
			DrawAxis2D(Projector, World, Anchor, Joint->Axis, JointColor);
			Projector.ScreenCircle(Anchor, 9.0f, JointFaint);
		}

		// ---- 3D 콜라이더 (월드 행렬 근사 — 물리와 같은 크기 규칙은 아니어도 위치·방향 확인용)
		if (const FBoxColliderComponent* Box = Registry.TryGet<FBoxColliderComponent>(Entity))
		{
			FVector3 Corners[8];
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Local = Box->Offset + FVector3((Corner & 1) ? Box->HalfExtents.X : -Box->HalfExtents.X,
				                                              (Corner & 2) ? Box->HalfExtents.Y : -Box->HalfExtents.Y,
				                                              (Corner & 4) ? Box->HalfExtents.Z : -Box->HalfExtents.Z);
				Corners[Corner] = World.TransformPosition(Local);
			}
			const int32 Edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
			for (const auto& Edge : Edges)
			{
				Projector.Line(Corners[Edge[0]], Corners[Edge[1]], Box->bIsTrigger ? TriggerColor : SolidColor);
			}
		}
		if (const FSphereColliderComponent* Sphere = Registry.TryGet<FSphereColliderComponent>(Entity))
		{
			const FVector3 Center = World.TransformPosition(Sphere->Offset);
			const float    Scale  = std::max({ World.GetAxisX().Length(), World.GetAxisY().Length(), World.GetAxisZ().Length() });
			const float    Radius = Sphere->Radius * Scale;
			const ImU32    Color  = Sphere->bIsTrigger ? TriggerColor : SolidColor;
			Projector.Circle(Center, FVector3::ForwardVector, FVector3::RightVector, Radius, Color);
			Projector.Circle(Center, FVector3::ForwardVector, FVector3::UpVector, Radius, Color);
			Projector.Circle(Center, FVector3::RightVector, FVector3::UpVector, Radius, Color);
		}
		if (const FCapsuleColliderComponent* Capsule = Registry.TryGet<FCapsuleColliderComponent>(Entity))
		{
			const FVector3 Center = World.TransformPosition(Capsule->Offset);
			const FVector3 AxisZ  = World.TransformVector(FVector3::UpVector);
			const FVector3 AxisX  = World.TransformVector(FVector3::ForwardVector).GetNormalized();
			const FVector3 AxisY  = World.TransformVector(FVector3::RightVector).GetNormalized();
			const float    Radius = Capsule->Radius * std::max(World.GetAxisX().Length(), World.GetAxisY().Length());
			const FVector3 Top    = Center + AxisZ * Capsule->HalfHeight;
			const FVector3 Bottom = Center - AxisZ * Capsule->HalfHeight;
			const ImU32    Color  = Capsule->bIsTrigger ? TriggerColor : SolidColor;
			Projector.Circle(Top, AxisX, AxisY, Radius, Color);
			Projector.Circle(Bottom, AxisX, AxisY, Radius, Color);
			const FVector3 Up = AxisZ.GetNormalized() * Radius;
			for (const FVector3& Side : { AxisX, -AxisX, AxisY, -AxisY })
			{
				Projector.Line(Top + Side * Radius, Bottom + Side * Radius, Color);
				Projector.Line(Top + Side * Radius, Top + Up, Color);
				Projector.Line(Bottom + Side * Radius, Bottom - Up, Color);
			}
		}
	}
	Projector.DrawList->PopClipRect();

	// 사라진 타일맵 캐시 정리 (몇 초 안 쓰인 것)
	if ((Frame & 255u) == 0)
	{
		std::erase_if(TilemapCaches, [&](const auto& Pair) { return Pair.second.LastFrame + 256u < Frame; });
	}
}
