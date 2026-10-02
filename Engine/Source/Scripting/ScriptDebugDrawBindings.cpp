// Lua 3D 디버그 그리기 (Phase 41-3) — FScriptDebugDrawHooks → FGameWorld → Renderer/DebugDraw.h (규칙은 그 헤더 주석)
//   Debug.DrawLine(from, to, color?, duration?, onTop?)
//   Debug.DrawArrow(from, to, color?, duration?, onTop?)
//   Debug.DrawSphere(center, radius, color?, duration?, onTop?)
//   Debug.DrawBox(center, halfExtents, color?, duration?, rotation?, onTop?)
//   Debug.DrawCapsule(center, radius, halfHeight, color?, duration?, rotation?, onTop?)   -- 회전 전 +Z 축
// 단위 cm. color = Vector3(r, g, b) 또는 Vector4(r, g, b, a), sRGB 0~1 (없으면 흰색). duration 초 (없거나 0 = 한 프레임).
// onTop = true면 씬에 가려지지 않는다. GPU 없는 서버/훅 없음이면 아무것도 하지 않는다
#include "Scripting/LuaRuntime.h"

#include <stdexcept>

namespace
{
	FVector4 ToColor(const sol::object& Value)
	{
		if (!Value.valid() || Value.get_type() == sol::type::lua_nil)
		{
			return FVector4(1.0f, 1.0f, 1.0f, 1.0f);
		}
		if (Value.is<FVector4>())
		{
			return Value.as<FVector4>();
		}
		if (Value.is<FVector3>())
		{
			const FVector3 Rgb = Value.as<FVector3>();
			return FVector4(Rgb.X, Rgb.Y, Rgb.Z, 1.0f);
		}
		throw std::runtime_error("Debug: 색은 Vector3(r, g, b) 또는 Vector4(r, g, b, a) (sRGB 0~1)");
	}
} // namespace

void FLuaRuntime::RegisterDebugDrawBindings()
{
	sol::table DebugTable = Lua.create_named_table("Debug");
	DebugTable["DrawLine"] = [this](const FVector3& Start, const FVector3& End, sol::object Color, sol::optional<float> Duration, sol::optional<bool> bOnTop) {
		if (DebugDrawHooks != nullptr && DebugDrawHooks->DrawLine)
		{
			DebugDrawHooks->DrawLine(Start, End, ToColor(Color), Duration.value_or(0.0f), !bOnTop.value_or(false));
		}
	};
	DebugTable["DrawArrow"] = [this](const FVector3& From, const FVector3& To, sol::object Color, sol::optional<float> Duration, sol::optional<bool> bOnTop) {
		if (DebugDrawHooks != nullptr && DebugDrawHooks->DrawArrow)
		{
			DebugDrawHooks->DrawArrow(From, To, ToColor(Color), Duration.value_or(0.0f), !bOnTop.value_or(false));
		}
	};
	DebugTable["DrawSphere"] = [this](const FVector3& Center, float Radius, sol::object Color, sol::optional<float> Duration, sol::optional<bool> bOnTop) {
		if (DebugDrawHooks != nullptr && DebugDrawHooks->DrawSphere)
		{
			DebugDrawHooks->DrawSphere(Center, Radius, ToColor(Color), Duration.value_or(0.0f), !bOnTop.value_or(false));
		}
	};
	DebugTable["DrawBox"] = [this](const FVector3& Center, const FVector3& HalfExtents, sol::object Color, sol::optional<float> Duration,
	                               sol::optional<FQuat> Rotation, sol::optional<bool> bOnTop) {
		if (DebugDrawHooks != nullptr && DebugDrawHooks->DrawBox)
		{
			DebugDrawHooks->DrawBox(Center, HalfExtents, Rotation ? Rotation->GetNormalized() : FQuat::Identity, ToColor(Color), Duration.value_or(0.0f),
			                        !bOnTop.value_or(false));
		}
	};
	DebugTable["DrawCapsule"] = [this](const FVector3& Center, float Radius, float HalfHeight, sol::object Color, sol::optional<float> Duration,
	                                   sol::optional<FQuat> Rotation, sol::optional<bool> bOnTop) {
		if (DebugDrawHooks != nullptr && DebugDrawHooks->DrawCapsule)
		{
			DebugDrawHooks->DrawCapsule(Center, Radius, HalfHeight, Rotation ? Rotation->GetNormalized() : FQuat::Identity, ToColor(Color),
			                            Duration.value_or(0.0f), !bOnTop.value_or(false));
		}
	};
}
