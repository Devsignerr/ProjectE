// Lua 물리 알림 전달 (FGameWorld가 물리 스텝 뒤에 부른다 — 규칙은 Physics/PhysicsSystem.h "충돌 알림")
//   function T:OnCollisionBegin(other, info) end  -- info.Point (Vector3, cm), info.Normal (나를 상대에서 밀어내는 방향),
//                                                 --   info.Impulse (충격 세기 추정 kg·cm/s), info.Speed (다가오던 속력 cm/s)
//   function T:OnCollisionEnd(other) end          -- other는 파괴됐으면 nil
//   function T:OnTriggerEnter(other) end / function T:OnTriggerExit(other) end  -- 트리거 쪽과 들어온 쪽 둘 다 받는다
// 래그돌 (Physics/Ragdoll.h — 엔티티 자신이나 자손의 스켈레탈 모델, 물리 훅이 없으면 false/무시):
//   entity:EnableRagdoll() → 켰는가 (이미 켜져 있거나 뼈대가 없으면 false)   entity:DisableRagdoll()   entity:IsRagdollActive()
//   각 프로세스 로컬 연출이다 (복제되지 않음). 사망 연동은 RagdollComponent.EnableOnDeath가 자동으로 한다
// 2D 물리 (Box2D, 평면 = 월드 X·Z — Physics/Physics2DMath.h. 물리 훅이 없으면 nil / 빈 표):
//   벡터 인자는 Vector3(Y 무시) 또는 Vector2(X = 월드 X, Y = 월드 Z). layers = 충돌 레이어 이름 표나 이름 하나 (생략 = 전부, 없는 이름은 Lua 오류)
//   Physics2D.Raycast(origin, direction, maxDistance, layers?) → { Entity, Point, Normal, Distance, Fraction } 또는 nil
//     Point/Normal은 Vector3 (Point의 Y = origin이 Vector3면 그 Y, 아니면 0 / Normal의 Y = 0). 트리거 제외
//   Physics2D.OverlapBox(center, size, angle?, layers?) — size = 전체 크기, angle = 도(반시계 +) / Physics2D.OverlapCircle(center, radius, layers?)
//     → 엔티티 배열 (트리거 제외)
//   entity:AddForce/AddImpulse/SetVelocity/GetVelocity/GetMass는 2D 강체만 있는 엔티티면 2D로 동작한다 (Vector3의 X·Z)
#include "Core/Settings/ProjectSettings.h"
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#include <format>
#include <stdexcept>

void FLuaRuntime::RegisterPhysicsBindings()
{
	const auto Require = [this](const FScriptEntity& Entity) {
		if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity.Entity))
		{
			throw std::runtime_error("유효하지 않은 엔티티입니다");
		}
	};
	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["EnableRagdoll"] = [this, Require](const FScriptEntity& Entity) {
		Require(Entity);
		return PhysicsHooks != nullptr && PhysicsHooks->EnableRagdoll && PhysicsHooks->EnableRagdoll(Entity.Entity);
	};
	EntityType["DisableRagdoll"] = [this, Require](const FScriptEntity& Entity) {
		Require(Entity);
		if (PhysicsHooks != nullptr && PhysicsHooks->DisableRagdoll)
		{
			PhysicsHooks->DisableRagdoll(Entity.Entity);
		}
	};
	EntityType["IsRagdollActive"] = [this, Require](const FScriptEntity& Entity) {
		Require(Entity);
		return PhysicsHooks != nullptr && PhysicsHooks->IsRagdollActive && PhysicsHooks->IsRagdollActive(Entity.Entity);
	};

	// ---- 모양 질의 (Phase 41-2, cm — FScriptPhysicsHooks::Overlap/Sweep). 트리거 제외, ignore 엔티티(와 충돌을 끈 쌍) 제외
	//   Physics.OverlapSphere(center, radius, ignore?) / OverlapBox(center, halfExtents, rotation?, ignore?) /
	//   OverlapCapsule(center, radius, halfHeight, rotation?, ignore?) → 엔티티 배열 (없으면 빈 표)
	//   Physics.SphereCast(start, radius, direction, maxDistance, ignore?) / BoxCast(start, halfExtents, direction, maxDistance, rotation?, ignore?) /
	//   CapsuleCast(start, radius, halfHeight, direction, maxDistance, rotation?, ignore?)
	//     → { entity, position(닿은 점), normal, distance(이동 거리 — 그때 가운데 = start + 방향 × distance) } 또는 nil
	//   캡슐은 회전 전 +Z 축 (콜라이더와 같음). 물리 훅이 없으면 빈 표 / nil
	const auto IgnoreOf = [](const sol::optional<FScriptEntity>& Ignore) { return Ignore ? Ignore->Entity : NullEntity; };
	const auto RunOverlap = [this](const FScriptQueryShape& Shape, const FVector3& Position, FEntity Ignore) {
		sol::table Result = Lua.create_table();
		if (PhysicsHooks == nullptr || !PhysicsHooks->Overlap)
		{
			return Result;
		}
		std::vector<FEntity> Entities;
		PhysicsHooks->Overlap(Shape, Position, Ignore, Entities);
		int32 Index = 1;
		for (const FEntity Entity : Entities)
		{
			if (Scene != nullptr && Scene->GetRegistry().IsValid(Entity))
			{
				Result[Index++] = FScriptEntity{ Entity };
			}
		}
		return Result;
	};
	const auto RunSweep = [this](const FScriptQueryShape& Shape, const FVector3& Start, const FVector3& Direction, float MaxDistance,
	                             FEntity Ignore) -> sol::object {
		FScriptRayHit Hit;
		if (PhysicsHooks == nullptr || !PhysicsHooks->Sweep || !PhysicsHooks->Sweep(Shape, Start, Direction, MaxDistance, Ignore, Hit))
		{
			return sol::lua_nil;
		}
		sol::table Result  = Lua.create_table();
		Result["entity"]   = Scene != nullptr && Scene->GetRegistry().IsValid(Hit.Entity) ? sol::make_object(Lua, FScriptEntity{ Hit.Entity }) : sol::object(sol::lua_nil);
		Result["position"] = Hit.Position;
		Result["normal"]   = Hit.Normal;
		Result["distance"] = Hit.Distance;
		return Result;
	};
	const auto MakeShape = [](EScriptQueryShape Type, const FVector3& HalfExtents, float Radius, float HalfHeight, const sol::optional<FQuat>& Rotation) {
		FScriptQueryShape Shape;
		Shape.Shape       = Type;
		Shape.HalfExtents = HalfExtents;
		Shape.Radius      = Radius;
		Shape.HalfHeight  = HalfHeight;
		Shape.Rotation    = Rotation ? Rotation->GetNormalized() : FQuat::Identity;
		return Shape;
	};

	sol::table PhysicsTable = Lua["Physics"];
	PhysicsTable["OverlapSphere"] = [=](const FVector3& Center, float Radius, sol::optional<FScriptEntity> Ignore) {
		return RunOverlap(MakeShape(EScriptQueryShape::Sphere, FVector3::ZeroVector, Radius, 0.0f, sol::nullopt), Center, IgnoreOf(Ignore));
	};
	PhysicsTable["OverlapBox"] = [=](const FVector3& Center, const FVector3& HalfExtents, sol::optional<FQuat> Rotation, sol::optional<FScriptEntity> Ignore) {
		return RunOverlap(MakeShape(EScriptQueryShape::Box, HalfExtents, 0.0f, 0.0f, Rotation), Center, IgnoreOf(Ignore));
	};
	PhysicsTable["OverlapCapsule"] = [=](const FVector3& Center, float Radius, float HalfHeight, sol::optional<FQuat> Rotation,
	                                     sol::optional<FScriptEntity> Ignore) {
		return RunOverlap(MakeShape(EScriptQueryShape::Capsule, FVector3::ZeroVector, Radius, HalfHeight, Rotation), Center, IgnoreOf(Ignore));
	};
	PhysicsTable["SphereCast"] = [=](const FVector3& Start, float Radius, const FVector3& Direction, float MaxDistance, sol::optional<FScriptEntity> Ignore) {
		return RunSweep(MakeShape(EScriptQueryShape::Sphere, FVector3::ZeroVector, Radius, 0.0f, sol::nullopt), Start, Direction, MaxDistance, IgnoreOf(Ignore));
	};
	PhysicsTable["BoxCast"] = [=](const FVector3& Start, const FVector3& HalfExtents, const FVector3& Direction, float MaxDistance,
	                              sol::optional<FQuat> Rotation, sol::optional<FScriptEntity> Ignore) {
		return RunSweep(MakeShape(EScriptQueryShape::Box, HalfExtents, 0.0f, 0.0f, Rotation), Start, Direction, MaxDistance, IgnoreOf(Ignore));
	};
	PhysicsTable["CapsuleCast"] = [=](const FVector3& Start, float Radius, float HalfHeight, const FVector3& Direction, float MaxDistance,
	                                  sol::optional<FQuat> Rotation, sol::optional<FScriptEntity> Ignore) {
		return RunSweep(MakeShape(EScriptQueryShape::Capsule, FVector3::ZeroVector, Radius, HalfHeight, Rotation), Start, Direction, MaxDistance,
		                IgnoreOf(Ignore));
	};
	RegisterPhysics2DBindings();
}

void FLuaRuntime::RegisterPhysics2DBindings()
{
	// Vector3(Y 무시) 또는 Vector2 → 평면 좌표. OutDepth = Vector3면 그 Y
	const auto ToPlane = [](const sol::object& Value, const char* Function, const char* Name, float* OutDepth = nullptr) {
		if (Value.is<FVector3>())
		{
			const FVector3 Vector = Value.as<FVector3>();
			if (OutDepth != nullptr)
			{
				*OutDepth = Vector.Y;
			}
			return FVector2(Vector.X, Vector.Z);
		}
		if (Value.is<FVector2>())
		{
			return Value.as<FVector2>();
		}
		throw std::runtime_error(std::format("{}: {}은(는) Vector3 또는 Vector2여야 합니다", Function, Name));
	};
	const auto ToLayerMask = [](const sol::object& Layers, const char* Function) {
		uint32 LayerMask = FCollisionLayerSettings::AllLayersMask;
		if (!Layers.valid() || Layers.get_type() == sol::type::lua_nil || Layers.get_type() == sol::type::none)
		{
			return LayerMask;
		}
		std::vector<std::string> Names;
		if (Layers.is<std::string>())
		{
			Names.push_back(Layers.as<std::string>());
		}
		else if (Layers.get_type() == sol::type::table)
		{
			for (const auto& [Key, Value] : Layers.as<sol::table>())
			{
				if (!Value.is<std::string>())
				{
					throw std::runtime_error(std::format("{}: 레이어 표에는 이름 문자열만 넣습니다", Function));
				}
				Names.push_back(Value.as<std::string>());
			}
		}
		else
		{
			throw std::runtime_error(std::format("{}: 레이어 인자는 이름 표나 이름이어야 합니다", Function));
		}
		std::string Unknown;
		if (!FProjectSettings::Get().Collision.MakeMask(Names, LayerMask, &Unknown))
		{
			throw std::runtime_error(std::format("{}: 없는 충돌 레이어 '{}' (프로젝트 설정 → 충돌 레이어)", Function, Unknown));
		}
		return LayerMask;
	};
	const auto ToEntityTable = [this](const std::vector<FEntity>& Entities) {
		sol::table Result = Lua.create_table();
		int32      Index  = 1;
		for (const FEntity Entity : Entities)
		{
			if (Scene != nullptr && Scene->GetRegistry().IsValid(Entity))
			{
				Result[Index++] = FScriptEntity{ Entity };
			}
		}
		return Result;
	};

	sol::table Physics2DTable = Lua.create_named_table("Physics2D");
	Physics2DTable["Raycast"] = [=, this](const sol::object& Origin, const sol::object& Direction, float MaxDistance, sol::object Layers) -> sol::object {
		float          Depth       = 0.0f;
		const FVector2 PlaneOrigin = ToPlane(Origin, "Physics2D.Raycast", "origin", &Depth);
		const FVector2 PlaneDir    = ToPlane(Direction, "Physics2D.Raycast", "direction");
		const uint32   LayerMask   = ToLayerMask(Layers, "Physics2D.Raycast");
		FScriptRayHit2D Hit;
		if (PhysicsHooks == nullptr || !PhysicsHooks->Raycast2D || !PhysicsHooks->Raycast2D(PlaneOrigin, PlaneDir, MaxDistance, LayerMask, Hit))
		{
			return sol::lua_nil;
		}
		sol::table Result  = Lua.create_table();
		Result["Entity"]   = Scene != nullptr && Scene->GetRegistry().IsValid(Hit.Entity) ? sol::make_object(Lua, FScriptEntity{ Hit.Entity }) : sol::object(sol::lua_nil);
		Result["Point"]    = FVector3(Hit.Position.X, Depth, Hit.Position.Y);
		Result["Normal"]   = FVector3(Hit.Normal.X, 0.0f, Hit.Normal.Y);
		Result["Distance"] = Hit.Distance;
		Result["Fraction"] = Hit.Fraction;
		return Result;
	};
	Physics2DTable["OverlapBox"] = [=, this](const sol::object& Center, const sol::object& Size, sol::optional<float> AngleDegrees, sol::object Layers) {
		const FVector2 PlaneCenter = ToPlane(Center, "Physics2D.OverlapBox", "center");
		const FVector2 PlaneSize   = ToPlane(Size, "Physics2D.OverlapBox", "size");
		const uint32   LayerMask   = ToLayerMask(Layers, "Physics2D.OverlapBox");
		std::vector<FEntity> Entities;
		if (PhysicsHooks != nullptr && PhysicsHooks->OverlapBox2D)
		{
			PhysicsHooks->OverlapBox2D(PlaneCenter, PlaneSize * 0.5f, AngleDegrees.value_or(0.0f) * FMath::DegToRad, LayerMask, Entities);
		}
		return ToEntityTable(Entities);
	};
	Physics2DTable["OverlapCircle"] = [=, this](const sol::object& Center, float Radius, sol::object Layers) {
		const FVector2 PlaneCenter = ToPlane(Center, "Physics2D.OverlapCircle", "center");
		const uint32   LayerMask   = ToLayerMask(Layers, "Physics2D.OverlapCircle");
		std::vector<FEntity> Entities;
		if (PhysicsHooks != nullptr && PhysicsHooks->OverlapCircle2D)
		{
			PhysicsHooks->OverlapCircle2D(PlaneCenter, Radius, LayerMask, Entities);
		}
		return ToEntityTable(Entities);
	};
}

bool FLuaRuntime::InvokeMethodWithFields(FEntity Target, const std::string& MethodName, const FGameRpcArgs& Args, const FScriptEventFields& Fields)
{
	const auto Found = Instances.find(Target.ToId());
	if (Found == Instances.end() || Found->second.bFaulted || !Found->second.Self.valid())
	{
		return false;
	}
	FScriptInstance&  Instance = Found->second;
	const sol::object Method   = Instance.Self[MethodName];
	if (Method.get_type() != sol::type::function)
	{
		return false;
	}
	std::vector<sol::object> Values;
	Values.reserve(Args.size() + 1);
	for (const FGameRpcValue& Arg : Args)
	{
		Values.push_back(FromRpcValue(Arg));
	}
	sol::table Table = Lua.create_table();
	for (const auto& [Key, Value] : Fields)
	{
		Table[Key] = FromRpcValue(Value);
	}
	Values.push_back(Table);
	const sol::table               Self = Instance.Self;
	sol::protected_function        Function(Method.as<sol::function>(), Traceback);
	const FInstanceScope           Scope(*this, Instance.Entity);
	sol::protected_function_result Result = Function(Self, sol::as_args(Values));
	if (!Result.valid())
	{
		const sol::error Error = Result;
		FaultInstance(Instance, MethodName, Error.what());
		return false;
	}
	return true;
}
