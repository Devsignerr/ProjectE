// Lua 2D 바인딩 (Phase 56 — 규약은 Scene/Sprite/SpriteAsset.h 머리 주석. 나머지 값은 리플렉션 프로퍼티로 직접 읽고 쓴다)
// 플립북 (FFlipbookComponent, 같은 엔티티):
//   entity:PlayFlipbook([path]) → 컴포넌트가 있는지. path가 지금과 다르면 바꿔 처음부터, 생략/같으면 현재 것을 처음부터 (시작 프레임 이벤트 다시) + 재생 켬
//   entity:StopFlipbook() → 현재 프레임에 멈춤 (Playing = false)   entity:SetFlipbookTime(초) → 스크럽 (이벤트 없음)
//   entity:GetFlipbookFrame() → 프레임 번호 (없으면 -1)            entity:IsFlipbookFinished() → Once가 끝에 닿았는가
//   이벤트: function T:OnFlipbookEvent_<이름>(frame) end, function T:OnFlipbookFinished() end — 플립북 엔티티의 스크립트, 없으면 가장 가까운
//   조상의 스크립트(노티파이와 같음). 플레이 중, 표시 틱에서 난 이벤트를 다음 게임플레이 틱 OnUpdate 뒤(노티파이 다음)에 배달
// 스프라이트: entity:SetSpriteFlip(flipX, flipY) → 컴포넌트가 있는지
// 타일맵 (FTilemapComponent, 셀 (0,0) 왼쪽 아래 = 엔티티 원점 — Scene/Sprite/TilemapData.h). 컴포넌트가 없으면 Lua 오류:
//   entity:GetTile(x, y) → id(빈칸 nil), flipX, flipY, rot90       entity:GetTileTags(x, y) → 태그 문자열 표 (빈칸/정의 없음 = 빈 표)
//   entity:SetTile(x, y, id[, flipX, flipY, rot90]) / entity:EraseTile(x, y) — TileData까지 써 넣는다(CommitTilemapData) → 저장·복제·
//     2D 물리(다음 물리 갱신에 충돌 다시 만듦)에 반영된다. 셀을 많이 바꾸면 그때마다 인코딩하므로 큰 편집은 C++로
//   entity:WorldToCell(Vector3 | Vector2(X, Z)) → x, y               entity:CellToWorld(x, y) → 셀 가운데 월드 위치 (Vector3, 깊이 = 엔티티 평면)
#include "Scene/Scene.h"
#include "Scene/Sprite/FlipbookSystem.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/TilemapCollision.h"
#include "Scene/Sprite/TilesetAsset.h"
#include "Scripting/LuaRuntime.h"

#include <format>
#include <stdexcept>
#include <tuple>

namespace
{
	// 셀 크기 (cm): 타일셋이 있으면 0 축은 타일 px × UnitsPerPixel, 없으면 CellSize가 양수여야 한다
	FVector2 ResolveCellSizeOrThrow(FTilemapComponent& Tilemap, const char* ApiName)
	{
		const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(Tilemap);
		const FVector2 CellSize = Tileset != nullptr ? TilemapCollision::ResolveCellSize(*Tileset, Tilemap.CellSize) : Tilemap.CellSize;
		if (CellSize.X <= 0.0f || CellSize.Y <= 0.0f)
		{
			throw std::runtime_error(std::format("{}: 셀 크기를 알 수 없습니다 (타일셋 '{}'을 읽지 못했고 CellSize가 0)", ApiName, Tilemap.Tileset));
		}
		return CellSize;
	}

	FMatrix4x4 ComputeWorldMatrix(const FScene& Scene, FEntity Entity)
	{
		return Scene.GetTransform(Entity).GetLocalMatrix() * Scene.GetParentWorldMatrix(Entity);
	}
} // namespace

void FLuaRuntime::RegisterSprite2DBindings()
{
	const auto Require = [this](const FScriptEntity& Entity) -> FScene& {
		if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity.Entity))
		{
			throw std::runtime_error("유효하지 않은 엔티티입니다");
		}
		return *Scene;
	};
	const auto RequireTilemap = [Require](const FScriptEntity& Entity, const char* ApiName) -> FTilemapComponent& {
		FTilemapComponent* Tilemap = Require(Entity).GetRegistry().TryGet<FTilemapComponent>(Entity.Entity);
		if (Tilemap == nullptr)
		{
			throw std::runtime_error(std::format("{}: 엔티티에 TilemapComponent가 없습니다", ApiName));
		}
		return *Tilemap;
	};

	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];

	// ---- 플립북
	EntityType["PlayFlipbook"] = [Require](const FScriptEntity& Entity, sol::optional<std::string> Path) {
		FScene&             Target    = Require(Entity);
		FFlipbookComponent* Component = Target.GetRegistry().TryGet<FFlipbookComponent>(Entity.Entity);
		if (Component == nullptr)
		{
			return false;
		}
		Component->bPlaying = true;
		if (Path && *Path != Component->Flipbook)
		{
			Component->Flipbook = *Path; // 경로가 바뀌면 FFlipbookSystem이 StartTime부터 다시 시작한다
		}
		return FFlipbookSystem::Restart(Target, Entity.Entity);
	};
	EntityType["StopFlipbook"] = [Require](const FScriptEntity& Entity) {
		FFlipbookComponent* Component = Require(Entity).GetRegistry().TryGet<FFlipbookComponent>(Entity.Entity);
		if (Component != nullptr)
		{
			Component->bPlaying = false;
		}
		return Component != nullptr;
	};
	EntityType["SetFlipbookTime"] = [Require](const FScriptEntity& Entity, float Seconds) {
		return FFlipbookSystem::SetTime(Require(Entity), Entity.Entity, Seconds);
	};
	EntityType["GetFlipbookFrame"] = [Require](const FScriptEntity& Entity) {
		return FFlipbookSystem::GetFrame(Require(Entity), Entity.Entity);
	};
	EntityType["IsFlipbookFinished"] = [Require](const FScriptEntity& Entity) {
		const FFlipbookComponent* Component = Require(Entity).GetRegistry().TryGet<FFlipbookComponent>(Entity.Entity);
		return Component != nullptr && Component->Runtime.bFinished;
	};

	// ---- 스프라이트
	EntityType["SetSpriteFlip"] = [Require](const FScriptEntity& Entity, bool bFlipX, bool bFlipY) {
		FSpriteComponent* Sprite = Require(Entity).GetRegistry().TryGet<FSpriteComponent>(Entity.Entity);
		if (Sprite != nullptr)
		{
			Sprite->bFlipX = bFlipX;
			Sprite->bFlipY = bFlipY;
		}
		return Sprite != nullptr;
	};

	// ---- 타일맵
	EntityType["GetTile"] = [RequireTilemap](const FScriptEntity& Entity, int32 X, int32 Y) -> std::tuple<sol::optional<int32>, bool, bool, bool> {
		FTilemapComponent& Tilemap = RequireTilemap(Entity, "GetTile");
		const uint32       Cell    = Sprite2DRuntime::GetTilemapData(Tilemap).Get(X, Y);
		if (TileCell::IsEmpty(Cell))
		{
			return { sol::nullopt, false, false, false };
		}
		return { TileCell::GetTileId(Cell), (Cell & TileCell::FlipXBit) != 0, (Cell & TileCell::FlipYBit) != 0, (Cell & TileCell::Rotate90Bit) != 0 };
	};
	EntityType["SetTile"] = [RequireTilemap](const FScriptEntity& Entity, int32 X, int32 Y, int32 TileId, sol::optional<bool> bFlipX,
	                                         sol::optional<bool> bFlipY, sol::optional<bool> bRotate90) {
		FTilemapComponent& Tilemap = RequireTilemap(Entity, "SetTile");
		if (TileId < 0 || TileId > TileCell::MaxTileId)
		{
			throw std::runtime_error(std::format("SetTile: 타일 번호 {}가 범위 밖입니다 (지우려면 EraseTile)", TileId));
		}
		const uint32 Flags = (bFlipX.value_or(false) ? TileCell::FlipXBit : 0u) | (bFlipY.value_or(false) ? TileCell::FlipYBit : 0u) |
		                     (bRotate90.value_or(false) ? TileCell::Rotate90Bit : 0u);
		Sprite2DRuntime::GetTilemapData(Tilemap); // 지금 TileData로 디코딩해 둔다
		const uint32 Cell = TileCell::Make(TileId, Flags);
		if (Tilemap.Runtime.Data.Get(X, Y) != Cell)
		{
			Tilemap.Runtime.Data.Set(X, Y, Cell);
			Sprite2DRuntime::CommitTilemapData(Tilemap);
		}
	};
	EntityType["EraseTile"] = [RequireTilemap](const FScriptEntity& Entity, int32 X, int32 Y) {
		FTilemapComponent& Tilemap = RequireTilemap(Entity, "EraseTile");
		if (!TileCell::IsEmpty(Sprite2DRuntime::GetTilemapData(Tilemap).Get(X, Y)))
		{
			Tilemap.Runtime.Data.Erase(X, Y);
			Sprite2DRuntime::CommitTilemapData(Tilemap);
		}
	};
	EntityType["GetTileTags"] = [this, RequireTilemap](const FScriptEntity& Entity, int32 X, int32 Y) {
		FTilemapComponent& Tilemap = RequireTilemap(Entity, "GetTileTags");
		sol::table         Result  = Lua.create_table();
		const uint32       Cell    = Sprite2DRuntime::GetTilemapData(Tilemap).Get(X, Y);
		const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(Tilemap);
		if (TileCell::IsEmpty(Cell) || Tileset == nullptr)
		{
			return Result;
		}
		if (const FTileDefinition* Definition = Tileset->FindTile(TileCell::GetTileId(Cell)))
		{
			for (size_t Index = 0; Index < Definition->Tags.size(); ++Index)
			{
				Result[Index + 1] = Definition->Tags[Index];
			}
		}
		return Result;
	};
	EntityType["WorldToCell"] = [Require, RequireTilemap](const FScriptEntity& Entity, const sol::object& Position) -> std::tuple<int32, int32> {
		FTilemapComponent& Tilemap  = RequireTilemap(Entity, "WorldToCell");
		const FVector2     CellSize = ResolveCellSizeOrThrow(Tilemap, "WorldToCell");
		FVector3           World;
		if (Position.is<FVector3>())
		{
			World = Position.as<FVector3>();
		}
		else if (Position.is<FVector2>())
		{
			const FVector2 Plane = Position.as<FVector2>();
			World                = FVector3(Plane.X, 0.0f, Plane.Y);
		}
		else
		{
			throw std::runtime_error("WorldToCell: 위치는 Vector3 또는 Vector2(X, Z)여야 합니다");
		}
		FMatrix4x4 Inverse;
		if (!ComputeWorldMatrix(Require(Entity), Entity.Entity).TryGetInverse(Inverse))
		{
			throw std::runtime_error("WorldToCell: 타일맵 엔티티의 트랜스폼을 뒤집을 수 없습니다 (스케일 0)");
		}
		const FVector3   Local = Inverse.TransformPosition(World);
		const FTileCoord Coord = TilemapMath::LocalToCell(FVector2(Local.X, Local.Z), CellSize);
		return { Coord.X, Coord.Y };
	};
	EntityType["CellToWorld"] = [Require, RequireTilemap](const FScriptEntity& Entity, int32 X, int32 Y) {
		FTilemapComponent& Tilemap  = RequireTilemap(Entity, "CellToWorld");
		const FVector2     CellSize = ResolveCellSizeOrThrow(Tilemap, "CellToWorld");
		const FVector2     Local    = TilemapMath::CellCenterToLocal(X, Y, CellSize);
		return ComputeWorldMatrix(Require(Entity), Entity.Entity).TransformPosition(FVector3(Local.X, 0.0f, Local.Y));
	};
}

void FLuaRuntime::DispatchFlipbookEvents()
{
	FRegistry& Registry = Scene->GetRegistry();
	struct FPending
	{
		FEntity     Entity;
		std::string Method;
		int32       Frame = 0;
		bool        bPassFrame = false;
	};
	// 순회 중 스크립트가 구조를 바꿀 수 있으므로 먼저 모은다
	std::vector<FPending> Calls;
	Registry.View<FFlipbookComponent>().Each([&](FEntity Entity, FFlipbookComponent& Component) {
		for (const FFlipbookEventRecord& Event : Component.Runtime.PendingEvents)
		{
			Calls.push_back({ Entity, "OnFlipbookEvent_" + Event.Name, Event.Frame, true });
		}
		if (Component.Runtime.bFinishedThisUpdate)
		{
			Calls.push_back({ Entity, "OnFlipbookFinished", 0, false });
		}
	});
	for (const FPending& Call : Calls)
	{
		// 받는 쪽: 플립북 엔티티의 스크립트, 없으면 가장 가까운 조상의 스크립트 (노티파이와 같음)
		for (FEntity Current = Call.Entity; Current.IsValid() && Registry.IsValid(Current); Current = Scene->GetParent(Current))
		{
			const auto Found = Instances.find(Current.ToId());
			if (Found == Instances.end())
			{
				continue;
			}
			FScriptInstance& Instance = Found->second;
			if (Instance.bFaulted || !Instance.bStarted)
			{
				break;
			}
			const sol::object Method = Instance.Self[Call.Method];
			if (Method.get_type() == sol::type::function)
			{
				++SideEffectCount;
				const sol::table               Self = Instance.Self;
				sol::protected_function        Function(Method.as<sol::function>(), Traceback);
				const FInstanceScope           Scope(*this, Instance.Entity);
				sol::protected_function_result Result = Call.bPassFrame ? Function(Self, Call.Frame) : Function(Self);
				if (!Result.valid())
				{
					const sol::error Error = Result;
					FaultInstance(Instance, Call.Method, Error.what());
				}
			}
			break;
		}
	}
}
