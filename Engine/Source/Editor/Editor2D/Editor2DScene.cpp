#include "Editor/Editor2D/Editor2DScene.h"

#include "Core/Settings/ProjectSettings.h"
#include "Editor/Editor2D/Collider2DShapes.h"
#include "Editor/Editor2D/Editor2DMath.h"
#include "Editor/EditorContext.h"
#include "Renderer/Camera.h"
#include "Renderer/SpriteDraw.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/FlipbookAsset.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"
#include "Scene/Sprite/SpriteAsset.h"
#include "Scene/Sprite/TilemapCollision.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <cmath>
#include <filesystem>

namespace
{
	std::string MakeName(const std::string& Path, const char* Fallback)
	{
		if (Path.empty())
		{
			return Fallback;
		}
		const std::string Stem = std::filesystem::path(Path).stem().string();
		return Stem.empty() ? std::string(Fallback) : Stem;
	}

} // namespace

bool Editor2DScene::RayToEntityPlane(const FMatrix4x4& World, const FRay& Ray, FVector2& OutLocal, float& OutT)
{
	FMatrix4x4 Inverse;
	if (!World.TryGetInverse(Inverse))
	{
		return false;
	}
	const FVector3 Origin    = Inverse.TransformPosition(Ray.Origin);
	const FVector3 Direction = Inverse.TransformVector(Ray.Direction);
	if (std::abs(Direction.Y) < 1.0e-8f)
	{
		return false;
	}
	OutT = -Origin.Y / Direction.Y; // 선형 변환이라 로컬 매개변수 = 월드 광선 거리
	if (OutT < 0.0f)
	{
		return false;
	}
	const FVector3 Hit = Origin + Direction * OutT;
	OutLocal           = FVector2(Hit.X, Hit.Z);
	return true;
}

FRay Editor2DScene::MakeRay(const FMatrix4x4& ViewProjection, const FVector2& Pixel, const FVector2& ImageSize)
{
	const float NdcX = ImageSize.X > 0.0f ? (Pixel.X / ImageSize.X) * 2.0f - 1.0f : 0.0f;
	const float NdcY = ImageSize.Y > 0.0f ? 1.0f - (Pixel.Y / ImageSize.Y) * 2.0f : 0.0f;
	return FRay::FromNdc(NdcX, NdcY, ViewProjection.GetInverse());
}

bool Editor2DScene::RayToPlaneY(const FRay& Ray, float PlaneY, FVector3& OutPoint)
{
	if (std::abs(Ray.Direction.Y) < 1.0e-5f)
	{
		return false;
	}
	const float T = (PlaneY - Ray.Origin.Y) / Ray.Direction.Y;
	if (T < 0.0f)
	{
		return false;
	}
	OutPoint   = Ray.GetPoint(T);
	OutPoint.Y = PlaneY;
	return true;
}

bool Editor2DScene::GetSpriteLocalQuad(FSpriteComponent& Sprite, FVector2 (&OutQuad)[4])
{
	const Sprite2DRuntime::FSpriteDisplay Display = Sprite2DRuntime::ResolveSprite(Sprite);
	if (Display.Asset == nullptr || Display.SliceIndex < 0 || Display.SliceIndex >= static_cast<int32>(Display.Asset->Slices.size()))
	{
		return false;
	}
	const FSpriteAsset& Asset = *Display.Asset;
	const FSpriteQuad   Quad  = SpriteMath::ComputeQuad(Asset.Slices[static_cast<size_t>(Display.SliceIndex)], std::max(Asset.TextureWidth, 1),
	                                                    std::max(Asset.TextureHeight, 1), Asset.UnitsPerPixel, Sprite.Size, Sprite.bFlipX, Sprite.bFlipY);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		OutQuad[Index] = Quad.Positions[Index];
	}
	return true;
}

namespace
{
	struct FBillboardViewState
	{
		FVector3 Right   = FVector3(1.0f, 0.0f, 0.0f);
		FVector3 Forward = FVector3(0.0f, -1.0f, 0.0f);
		FVector3 Up      = FVector3(0.0f, 0.0f, 1.0f);
		bool     bValid  = false;
	};
	FBillboardViewState GBillboardView; // 에디터 메인 스레드 전용
} // namespace

void Editor2DScene::SetBillboardView(const FVector3& Right, const FVector3& Forward, const FVector3& Up)
{
	GBillboardView = { Right, Forward, Up, true };
}

FMatrix4x4 Editor2DScene::GetSpriteWorld(const FMatrix4x4& World, const FSpriteComponent& Sprite)
{
	if (Sprite.Billboard == 0 || !GBillboardView.bValid)
	{
		return World;
	}
	return SpriteMath::ComputeBillboardWorld(World, static_cast<ESpriteBillboard>(Sprite.Billboard), GBillboardView.Right, GBillboardView.Forward,
	                                         GBillboardView.Up);
}

bool Editor2DScene::GetTilemapCellSize(FTilemapComponent& Tilemap, FVector2& OutCellSize)
{
	if (const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(Tilemap))
	{
		OutCellSize = TilemapCollision::ResolveCellSize(*Tileset, Tilemap.CellSize);
	}
	else
	{
		OutCellSize = Tilemap.CellSize;
	}
	return OutCellSize.X > 0.0f && OutCellSize.Y > 0.0f;
}

FEntity Editor2DScene::Pick(FScene& Scene, const FRay& Ray, float EdgeTolerance, float& OutDistance)
{
	FRegistry&                           Registry = Scene.GetRegistry();
	const FSortingLayerSettings&         Layers   = FProjectSettings::Get().SortingLayers;
	std::vector<Editor2DMath::FPick2DHit> Hits;

	Registry.View<FTransformComponent, FSpriteComponent>().Each([&](FEntity Entity, FTransformComponent& Transform, FSpriteComponent& Sprite) {
		FVector2 Quad[4];
		FVector2 Local;
		float    T = 0.0f;
		if (!Sprite.bVisible || !GetSpriteLocalQuad(Sprite, Quad) || !RayToEntityPlane(GetSpriteWorld(Transform.WorldMatrix, Sprite), Ray, Local, T) ||
		    !Editor2DMath::IsPointInConvexPolygon(Quad, 4, Local))
		{
			return;
		}
		Hits.push_back({ Entity, 1, Layers.ResolveLayer(Sprite.SortingLayer), Sprite.OrderInLayer, T });
	});
	Registry.View<FTransformComponent, FTilemapComponent>().Each([&](FEntity Entity, FTransformComponent& Transform, FTilemapComponent& Tilemap) {
		FVector2 CellSize;
		FVector2 Local;
		float    T = 0.0f;
		if (!GetTilemapCellSize(Tilemap, CellSize) || !RayToEntityPlane(Transform.WorldMatrix, Ray, Local, T))
		{
			return;
		}
		const FTileCoord Cell = TilemapMath::LocalToCell(Local, CellSize);
		if (TileCell::IsEmpty(Sprite2DRuntime::GetTilemapData(Tilemap).Get(Cell.X, Cell.Y)))
		{
			return;
		}
		Hits.push_back({ Entity, 1, Layers.ResolveLayer(Tilemap.SortingLayer), Tilemap.OrderInLayer, T });
	});
	// 콜라이더만 있는 엔티티 (스프라이트·타일맵이 있는 엔티티는 위에서 사각형/칸으로 판정했다)
	std::vector<Collider2DShapes::FOutline> Outlines;
	Registry.View<FTransformComponent>().Each([&](FEntity Entity, FTransformComponent& Transform) {
		if (Registry.Has<FSpriteComponent>(Entity) || Registry.Has<FTilemapComponent>(Entity) || !Collider2DShapes::HasShapes(Scene, Entity))
		{
			return;
		}
		FVector3 PlanePoint;
		if (!RayToPlaneY(Ray, Transform.WorldMatrix.GetOrigin().Y, PlanePoint))
		{
			return;
		}
		Outlines.clear();
		Collider2DShapes::Collect(Scene, Entity, Outlines);
		for (const Collider2DShapes::FOutline& Outline : Outlines)
		{
			if (Collider2DShapes::Contains(Outline, FVector2(PlanePoint.X, PlanePoint.Z), EdgeTolerance))
			{
				Hits.push_back({ Entity, 0, 0, 0, (PlanePoint - Ray.Origin).Length() });
				break;
			}
		}
	});

	const int32 Best = Editor2DMath::FindFrontmost(Hits);
	if (Best < 0)
	{
		return NullEntity;
	}
	OutDistance = Hits[static_cast<size_t>(Best)].Distance;
	return Hits[static_cast<size_t>(Best)].Entity;
}

bool Editor2DScene::AddBounds(FScene& Scene, FEntity Entity, FBox& InOutBounds)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Entity) || !Registry.Has<FTransformComponent>(Entity))
	{
		return false;
	}
	const FMatrix4x4& World  = Scene.GetTransform(Entity).WorldMatrix;
	bool              bAdded = false;
	if (FSpriteComponent* Sprite = Registry.TryGet<FSpriteComponent>(Entity))
	{
		FVector2 Quad[4];
		if (GetSpriteLocalQuad(*Sprite, Quad))
		{
			const FMatrix4x4 SpriteWorld = GetSpriteWorld(World, *Sprite);
			for (const FVector2& Corner : Quad)
			{
				InOutBounds.AddPoint(SpriteWorld.TransformPosition(FVector3(Corner.X, 0.0f, Corner.Y)));
			}
			bAdded = true;
		}
	}
	if (FTilemapComponent* Tilemap = Registry.TryGet<FTilemapComponent>(Entity))
	{
		FVector2        CellSize;
		const FTileRect Rect = Sprite2DRuntime::GetTilemapData(*Tilemap).GetBounds();
		if (Rect.IsValid() && GetTilemapCellSize(*Tilemap, CellSize))
		{
			const FVector2 Min = TilemapMath::CellToLocal(Rect.MinX, Rect.MinY, CellSize);
			const FVector2 Max = TilemapMath::CellToLocal(Rect.MaxX + 1, Rect.MaxY + 1, CellSize);
			InOutBounds.AddPoint(World.TransformPosition(FVector3(Min.X, 0.0f, Min.Y)));
			InOutBounds.AddPoint(World.TransformPosition(FVector3(Max.X, 0.0f, Max.Y)));
			bAdded = true;
		}
	}
	std::vector<Collider2DShapes::FOutline> Outlines;
	Collider2DShapes::Collect(Scene, Entity, Outlines);
	for (const Collider2DShapes::FOutline& Outline : Outlines)
	{
		for (const FVector3& Point : Outline.Points)
		{
			InOutBounds.AddPoint(Point);
			bAdded = true;
		}
	}
	return bAdded;
}

FEntity Editor2DScene::CreateSprite(FScene& Scene, const std::string& SpritePath, const FVector3& Position)
{
	const FEntity Entity               = Scene.CreateEntity(MakeName(SpritePath, "Sprite"));
	Scene.GetTransform(Entity).Position = Position;
	FSpriteComponent& Sprite            = Scene.GetRegistry().Emplace<FSpriteComponent>(Entity);
	Sprite.Sprite                       = SpritePath; // Slice 비움 = 첫 슬라이스
	return Entity;
}

FEntity Editor2DScene::CreateFlipbook(FScene& Scene, const std::string& FlipbookPath, const FVector3& Position)
{
	const FEntity Entity               = Scene.CreateEntity(MakeName(FlipbookPath, "Flipbook"));
	Scene.GetTransform(Entity).Position = Position;
	FSpriteComponent& Sprite            = Scene.GetRegistry().Emplace<FSpriteComponent>(Entity);
	// 플립북이 표시를 정하지만, 스프라이트 경로도 플립북 아틀라스로 채워 둔다 (플립북을 지워도 같은 그림)
	if (const std::shared_ptr<const FFlipbookAsset> Flipbook = FSprite2DLibrary::Get().LoadFlipbook(FlipbookPath))
	{
		Sprite.Sprite = FSprite2DLibrary::ResolveReference(FlipbookPath, Flipbook->Sprite);
	}
	Scene.GetRegistry().Emplace<FFlipbookComponent>(Entity).Flipbook = FlipbookPath;
	return Entity;
}

FEntity Editor2DScene::CreateTilemap(FScene& Scene, const std::string& TilesetPath, const FVector3& Position)
{
	const FEntity Entity               = Scene.CreateEntity(MakeName(TilesetPath, "Tilemap"));
	Scene.GetTransform(Entity).Position = Position;
	Scene.GetRegistry().Emplace<FTilemapComponent>(Entity).Tileset = TilesetPath; // 빈 맵, 셀 크기 0 = 타일 px × UnitsPerPixel
	return Entity;
}

FEntity Editor2DScene::CreateCamera2D(FScene& Scene, const FVector3& Target)
{
	const FEntity        Entity    = Scene.CreateEntity("Camera2D");
	FTransformComponent& Transform = Scene.GetTransform(Entity);
	Transform.Position             = FVector3(Target.X, 2000.0f, Target.Z);
	Transform.Rotation             = Editor2DMath::Get2DCameraRotation();
	FCameraComponent& Camera       = Scene.GetRegistry().Emplace<FCameraComponent>(Entity);
	Camera.bOrthographic           = true;
	Camera.OrthoHeight             = 1000.0f;
	Camera.FarZ                    = 20000.0f;
	return Entity;
}

FEntity Editor2DScene::CreateFromMenu(FEditorContext& Context, ECreate2D Kind, FEntity Parent)
{
	FScene&  Scene = *Context.Scene;
	FVector3 Target;
	if (Context.Camera != nullptr)
	{
		const FRay Ray(Context.Camera->GetPosition(), Context.Camera->GetForwardVector());
		if (!RayToPlaneY(Ray, 0.0f, Target) || (Target - Ray.Origin).LengthSquared() > 1.0e10f)
		{
			Target = FVector3::ZeroVector;
		}
	}
	FEntity     Created;
	const char* Label = "2D 엔티티 추가";
	switch (Kind)
	{
	case ECreate2D::Sprite:
		Created = CreateSprite(Scene, std::string(), Target);
		Label   = "2D 스프라이트 추가";
		break;
	case ECreate2D::Tilemap:
		Created = CreateTilemap(Scene, std::string(), Target);
		Label   = "2D 타일맵 추가";
		break;
	case ECreate2D::Camera:
		Created = CreateCamera2D(Scene, Target);
		Label   = "2D 카메라 추가";
		break;
	}
	if (Parent.IsValid() && Scene.GetRegistry().IsValid(Parent))
	{
		// 부모 아래에서도 같은 월드 위치·방향 (로컬 = 월드 × 부모 역행렬)
		const FMatrix4x4 WorldMatrix = Scene.GetTransform(Created).GetLocalMatrix();
		Scene.SetParent(Created, Parent);
		const FMatrix4x4 Local = WorldMatrix * Scene.GetParentWorldMatrix(Created).GetInverse();
		FTransformComponent& Transform = Scene.GetTransform(Created);
		Local.Decompose(Transform.Position, Transform.Rotation, Transform.Scale);
	}
	Scene.UpdateTransforms();
	Context.Select(Created);
	Context.MarkEdited(Label);
	return Created;
}
