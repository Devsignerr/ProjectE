#include "Scripting/LuaRuntime.h"

#include "Core/FileSystem.h"
#include "Core/Input.h"
#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptDebugger.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

E_DEFINE_LOG_CATEGORY(LogScript, Log)

namespace
{
	// 키 이름은 InputNames와 같다 (Lua: Input.IsKeyDown("W"), "Space", "LeftShift", "F1", "0" ...)
	EKey ParseKey(std::string_view Name)
	{
		EKey Key = EKey::None;
		if (!InputNames::TryParseKey(Name, Key))
		{
			throw std::runtime_error(std::format("알 수 없는 키 이름: '{}'", Name));
		}
		return Key;
	}

	FInputSource ParseInputSource(std::string_view Name)
	{
		FInputSource Source;
		if (!InputNames::TryParseSource(Name, Source))
		{
			throw std::runtime_error(std::format("알 수 없는 입력 이름: '{}' (키 \"W\", 마우스 \"MouseLeft\", 게임패드 \"Gamepad_A\" 등)", Name));
		}
		return Source;
	}

	EMouseButton ParseMouseButton(std::string_view Name)
	{
		if (Name == "Left") return EMouseButton::Left;
		if (Name == "Right") return EMouseButton::Right;
		if (Name == "Middle") return EMouseButton::Middle;
		throw std::runtime_error(std::format("알 수 없는 마우스 버튼: '{}' (Left/Right/Middle)", Name));
	}

	// "Transform"처럼 접미사 없이 써도 찾는다
	const FTypeInfo* FindComponentType(std::string_view Name)
	{
		const FTypeRegistry& Types = FTypeRegistry::Get();
		const FTypeInfo*     Type  = Types.Find(Name);
		if (Type == nullptr || !Type->bIsComponent)
		{
			Type = Types.Find(std::string(Name) + "Component");
		}
		return (Type != nullptr && Type->bIsComponent) ? Type : nullptr;
	}

	const FTypeInfo& RequireComponentType(std::string_view Name)
	{
		const FTypeInfo* Type = FindComponentType(Name);
		if (Type == nullptr)
		{
			throw std::runtime_error(std::format("알 수 없는 컴포넌트 타입: '{}'", Name));
		}
		return *Type;
	}

	// C++ 예외 → Lua 오류 메시지 (sol 기본 핸들러는 표준 출력에 따로 찍으므로 교체)
	int HandleBindingException(lua_State* L, sol::optional<const std::exception&>, sol::string_view Description)
	{
		return sol::stack::push(L, Description);
	}

	// 디버거가 연결된 상태의 오류 메시지 처리기: 오류 지점(스택 보존)에서 디버거 오류 정지 → debug.traceback(msg, 1)과 같은 결과
	int DebugErrorHandler(lua_State* L)
	{
		const char* Message = lua_tostring(L, 1);
		if (FScriptDebugger* Debugger = FScriptDebugger::FromState(L))
		{
			Debugger->OnUnhandledError(L, Message, 1); // 1 = 이 처리기 프레임
		}
		if (Message == nullptr && !lua_isnoneornil(L, 1))
		{
			lua_pushvalue(L, 1); // 문자열이 아닌 오류 값은 그대로 (debug.traceback과 같음)
			return 1;
		}
		luaL_traceback(L, L, Message, 1);
		return 1;
	}

} // namespace

FLuaRuntime::FLuaRuntime(std::filesystem::path InContentDirectory, uint32& InErrorCounter, FScriptDebugger* InDebugger)
	: ContentDirectory(std::move(InContentDirectory))
	, ErrorCounter(InErrorCounter)
	, Debugger(InDebugger)
{
	// io/os/package는 열지 않는다 (콘텐츠 스크립트에 파일 시스템/프로세스 접근을 주지 않음)
	Lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::coroutine, sol::lib::utf8,
	                   sol::lib::debug);
	Lua.set_exception_handler(&HandleBindingException);
	if (Debugger != nullptr)
	{
		// 바인딩 등록 전 (Coroutine.Start가 잡아 두는 coroutine.create도 디버거 것이어야 코루틴에 훅을 걸 수 있다)
		Debugger->Attach(Lua.lua_state());
		lua_pushcfunction(Lua.lua_state(), &DebugErrorHandler);
		Traceback = sol::protected_function(Lua.lua_state(), -1);
		lua_pop(Lua.lua_state(), 1);
	}
	else
	{
		Traceback = Lua["debug"]["traceback"];
	}
	Lua["dofile"]   = sol::lua_nil;
	Lua["loadfile"] = sol::lua_nil;
	RegisterBindings();
}

FLuaRuntime::~FLuaRuntime()
{
	// sol 참조(테이블/함수)는 상태보다 먼저 해제되어야 한다
	PendingSpawns.clear();
	Instances.clear(); // 인스턴스의 타이머/코루틴 참조도 함께
	Tasks.clear();
	Objects.clear();
	Classes.clear();
	Modules.clear();
	ModuleCaller = sol::lua_nil;
	CoroutineCreate = sol::lua_nil;
	CoroutineResume = sol::lua_nil;
	CoroutineStatus = sol::lua_nil;
	WaitToken       = sol::table();
	Traceback = sol::lua_nil;
	if (Debugger != nullptr)
	{
		Debugger->Detach(Lua.lua_state());
	}
}

std::string FLuaRuntime::ReadScriptSource(const std::filesystem::path& Path, bool& bOutOk)
{
	std::string Text;
	bOutOk = FFileSystem::ReadTextFile(Path, Text);
	if (!bOutOk)
	{
		return std::string();
	}
	// UTF-8 BOM 제거 (Lua 파서는 BOM을 모른다)
	if (Text.size() >= 3 && static_cast<unsigned char>(Text[0]) == 0xEF && static_cast<unsigned char>(Text[1]) == 0xBB &&
	    static_cast<unsigned char>(Text[2]) == 0xBF)
	{
		Text.erase(0, 3);
	}
	return Text;
}

void FLuaRuntime::ReportError(const std::string& Message)
{
	++ErrorCounter;
	E_LOG(LogScript, Error, "{}", Message);
}

// ---------------------------------------------------------------- 바인딩

void FLuaRuntime::RegisterBindings()
{
	RegisterMathBindings();
	RegisterEntityBindings();
	RegisterGlobals();
	RegisterPrefabBindings();
	RegisterNetBindings();
	RegisterAIBindings();
	RegisterUIBindings();
	RegisterGameBindings();
	RegisterGameplayBindings();
	RegisterAnimationGraphBindings();
	RegisterPhysicsBindings();
	RegisterSequenceBindings();
	RegisterTimerBindings();
	RegisterDebugDrawBindings();
	RegisterModuleBindings();
	RegisterCameraBindings();
	RegisterDataBindings();
}

void FLuaRuntime::RegisterMathBindings()
{
	sol::usertype<FVector3> Vector3 = Lua.new_usertype<FVector3>(
		"Vector3",
		sol::call_constructor,
		sol::factories([] { return FVector3(0.0f, 0.0f, 0.0f); }, [](float X, float Y, float Z) { return FVector3(X, Y, Z); }),
		"X", &FVector3::X,
		"Y", &FVector3::Y,
		"Z", &FVector3::Z,
		sol::meta_function::addition, [](const FVector3& A, const FVector3& B) { return A + B; },
		sol::meta_function::subtraction, [](const FVector3& A, const FVector3& B) { return A - B; },
		sol::meta_function::multiplication,
		sol::overload([](const FVector3& A, float Scale) { return A * Scale; }, [](float Scale, const FVector3& A) { return A * Scale; },
		              [](const FVector3& A, const FVector3& B) { return A * B; }),
		sol::meta_function::division, [](const FVector3& A, float Scale) { return A * (1.0f / Scale); },
		sol::meta_function::unary_minus, [](const FVector3& A) { return -A; },
		sol::meta_function::equal_to, [](const FVector3& A, const FVector3& B) { return A == B; },
		sol::meta_function::to_string, [](const FVector3& A) { return std::format("Vector3({:.3f}, {:.3f}, {:.3f})", A.X, A.Y, A.Z); },
		"Length", [](const FVector3& A) { return A.Length(); },
		"LengthSquared", [](const FVector3& A) { return A.LengthSquared(); },
		"Normalized", [](const FVector3& A) { return A.GetNormalized(); },
		"Dot", [](const FVector3& A, const FVector3& B) { return FVector3::Dot(A, B); },
		"Cross", [](const FVector3& A, const FVector3& B) { return FVector3::Cross(A, B); },
		"Lerp", [](const FVector3& A, const FVector3& B, float Alpha) { return FVector3::Lerp(A, B, Alpha); },
		"Distance", [](const FVector3& A, const FVector3& B) { return FVector3::Distance(A, B); });
	// 상수는 매번 새 값을 돌려주는 함수 (공유 userdata를 수정해 상수가 바뀌는 사고 방지)
	Vector3["Zero"]    = [] { return FVector3(0.0f, 0.0f, 0.0f); };
	Vector3["One"]     = [] { return FVector3::OneVector; };
	Vector3["Forward"] = [] { return FVector3::ForwardVector; };
	Vector3["Right"]   = [] { return FVector3::RightVector; };
	Vector3["Up"]      = [] { return FVector3::UpVector; };

	Lua.new_usertype<FVector2>(
		"Vector2",
		sol::call_constructor,
		sol::factories([] { return FVector2(0.0f, 0.0f); }, [](float X, float Y) { return FVector2(X, Y); }),
		"X", &FVector2::X,
		"Y", &FVector2::Y,
		sol::meta_function::to_string, [](const FVector2& A) { return std::format("Vector2({:.3f}, {:.3f})", A.X, A.Y); });

	Lua.new_usertype<FVector4>(
		"Vector4",
		sol::call_constructor,
		sol::factories([] { return FVector4(0.0f, 0.0f, 0.0f, 0.0f); }, [](float X, float Y, float Z, float W) { return FVector4(X, Y, Z, W); }),
		"X", &FVector4::X,
		"Y", &FVector4::Y,
		"Z", &FVector4::Z,
		"W", &FVector4::W,
		sol::meta_function::to_string,
		[](const FVector4& A) { return std::format("Vector4({:.3f}, {:.3f}, {:.3f}, {:.3f})", A.X, A.Y, A.Z, A.W); });

	Lua.new_usertype<FQuat>(
		"Quat",
		sol::call_constructor,
		sol::factories([] { return FQuat(); }, [](float X, float Y, float Z, float W) { return FQuat(X, Y, Z, W); }),
		"X", &FQuat::X,
		"Y", &FQuat::Y,
		"Z", &FQuat::Z,
		"W", &FQuat::W,
		sol::meta_function::multiplication,
		sol::overload([](const FQuat& A, const FQuat& B) { return A * B; }, [](const FQuat& A, const FVector3& V) { return A.RotateVector(V); }),
		sol::meta_function::to_string, [](const FQuat& A) { return std::format("Quat({:.4f}, {:.4f}, {:.4f}, {:.4f})", A.X, A.Y, A.Z, A.W); },
		// 오일러 각은 도 단위 (UE 규약: Pitch 기수 위, Yaw 오른쪽, Roll 오른쪽 날개 아래)
		"FromEuler", [](float Pitch, float Yaw, float Roll) { return FQuat::FromEuler(Pitch, Yaw, Roll); },
		"FromAxisAngle",
		[](const FVector3& Axis, float Degrees) { return FQuat::FromAxisAngle(Axis.GetNormalized(), FMath::DegreesToRadians(Degrees)); },
		"ToEuler",
		[](const FQuat& Q) {
			float Pitch = 0.0f, Yaw = 0.0f, Roll = 0.0f;
			Q.ToEuler(Pitch, Yaw, Roll);
			return std::make_tuple(Pitch, Yaw, Roll);
		},
		"RotateVector", [](const FQuat& Q, const FVector3& V) { return Q.RotateVector(V); },
		"Inverse", [](const FQuat& Q) { return Q.Inverse(); },
		"Forward", [](const FQuat& Q) { return Q.GetForwardVector(); },
		"Right", [](const FQuat& Q) { return Q.GetRightVector(); },
		"Up", [](const FQuat& Q) { return Q.GetUpVector(); },
		"Slerp", [](const FQuat& A, const FQuat& B, float Alpha) { return FQuat::Slerp(A, B, Alpha); },
		// Forward(+X)가 Direction을 향하는 회전 (Roll 없음)
		"LookRotation",
		[](const FVector3& Direction) {
			const FVector3 D     = Direction.GetNormalized();
			const float    Yaw   = FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
			const float    Pitch = FMath::RadiansToDegrees(FMath::Atan2(D.Z, FMath::Sqrt(D.X * D.X + D.Y * D.Y)));
			return FQuat::FromEuler(Pitch, Yaw, 0.0f);
		},
		"Identity", [] { return FQuat::Identity; });
}

namespace
{
	sol::object ReadProperty(sol::state_view Lua, const FPropertyInfo& Property, void* Component)
	{
		switch (Property.Type)
		{
		case EPropertyType::Bool:    return sol::make_object(Lua, Property.GetRef<bool>(Component));
		case EPropertyType::Int32:   return sol::make_object(Lua, Property.GetRef<int32>(Component));
		case EPropertyType::UInt32:  return sol::make_object(Lua, Property.GetRef<uint32>(Component));
		case EPropertyType::Float:   return sol::make_object(Lua, Property.GetRef<float>(Component));
		case EPropertyType::String:  return sol::make_object(Lua, Property.GetRef<std::string>(Component));
		case EPropertyType::Vector2: return sol::make_object(Lua, Property.GetRef<FVector2>(Component));
		case EPropertyType::Vector3: return sol::make_object(Lua, Property.GetRef<FVector3>(Component));
		case EPropertyType::Vector4: return sol::make_object(Lua, Property.GetRef<FVector4>(Component));
		case EPropertyType::Quat:    return sol::make_object(Lua, Property.GetRef<FQuat>(Component));
		case EPropertyType::Entity:
		{
			const FEntity& Entity = Property.GetRef<FEntity>(Component);
			return Entity.IsValid() ? sol::make_object(Lua, FScriptEntity{ Entity }) : sol::object(sol::lua_nil);
		}
		case EPropertyType::ResourceHandle:
			// 핸들은 공통 레이아웃(Index, Generation) — 스크립트에는 인덱스만 (읽기 전용)
			return sol::make_object(Lua, *static_cast<const uint32*>(Property.GetPtr(Component)));
		default:
			return sol::lua_nil;
		}
	}

	float RequireNumber(const sol::object& Value, const FPropertyInfo& Property)
	{
		if (Value.get_type() != sol::type::number)
		{
			throw std::runtime_error(std::format("프로퍼티 '{}'에는 숫자가 필요합니다", Property.Name));
		}
		return Value.as<float>();
	}

	template <typename T>
	const T& RequireUserdata(const sol::object& Value, const FPropertyInfo& Property, const char* TypeName)
	{
		if (!Value.is<T>())
		{
			throw std::runtime_error(std::format("프로퍼티 '{}'에는 {}가 필요합니다", Property.Name, TypeName));
		}
		return Value.as<const T&>();
	}

	void WriteProperty(const FPropertyInfo& Property, void* Component, const sol::object& Value)
	{
		// PF_ReadOnly는 인스펙터 편집 제한이므로 스크립트는 쓸 수 있다 (예: 새로 만든 엔티티의 MeshAsset). 런타임 핸들만 금지
		if (Property.Type == EPropertyType::ResourceHandle)
		{
			throw std::runtime_error(std::format("프로퍼티 '{}'는 런타임 리소스 핸들이라 스크립트에서 쓸 수 없습니다", Property.Name));
		}
		switch (Property.Type)
		{
		case EPropertyType::Bool:
			if (Value.get_type() != sol::type::boolean)
			{
				throw std::runtime_error(std::format("프로퍼티 '{}'에는 boolean이 필요합니다", Property.Name));
			}
			Property.GetRef<bool>(Component) = Value.as<bool>();
			break;
		case EPropertyType::Int32:  Property.GetRef<int32>(Component) = static_cast<int32>(RequireNumber(Value, Property)); break;
		case EPropertyType::UInt32: Property.GetRef<uint32>(Component) = static_cast<uint32>(FMath::Max(RequireNumber(Value, Property), 0.0f)); break;
		case EPropertyType::Float:  Property.GetRef<float>(Component) = RequireNumber(Value, Property); break;
		case EPropertyType::String:
			if (Value.get_type() != sol::type::string)
			{
				throw std::runtime_error(std::format("프로퍼티 '{}'에는 문자열이 필요합니다", Property.Name));
			}
			Property.GetRef<std::string>(Component) = Value.as<std::string>();
			break;
		case EPropertyType::Vector2: Property.GetRef<FVector2>(Component) = RequireUserdata<FVector2>(Value, Property, "Vector2"); break;
		case EPropertyType::Vector3: Property.GetRef<FVector3>(Component) = RequireUserdata<FVector3>(Value, Property, "Vector3"); break;
		case EPropertyType::Vector4: Property.GetRef<FVector4>(Component) = RequireUserdata<FVector4>(Value, Property, "Vector4"); break;
		case EPropertyType::Quat:    Property.GetRef<FQuat>(Component) = RequireUserdata<FQuat>(Value, Property, "Quat").GetNormalized(); break;
		case EPropertyType::Entity:
			Property.GetRef<FEntity>(Component) = Value.get_type() == sol::type::lua_nil ? NullEntity : RequireUserdata<FScriptEntity>(Value, Property, "Entity").Entity;
			break;
		default:
			throw std::runtime_error(std::format("프로퍼티 '{}'의 타입은 스크립트에서 쓸 수 없습니다", Property.Name));
		}
	}
} // namespace

void FLuaRuntime::RegisterEntityBindings()
{
	const auto RequireRegistry = [this]() -> FRegistry& {
		if (Scene == nullptr)
		{
			throw std::runtime_error("씬이 없습니다 (플레이 중에만 엔티티를 사용할 수 있습니다)");
		}
		return Scene->GetRegistry();
	};
	const auto RequireEntity = [RequireRegistry](const FScriptEntity& Entity) -> FRegistry& {
		FRegistry& Registry = RequireRegistry();
		if (!Registry.IsValid(Entity.Entity))
		{
			throw std::runtime_error("파괴되었거나 유효하지 않은 엔티티입니다");
		}
		return Registry;
	};
	const auto RequireTransform = [RequireEntity](const FScriptEntity& Entity) -> FTransformComponent& {
		FTransformComponent* Transform = RequireEntity(Entity).TryGet<FTransformComponent>(Entity.Entity);
		if (Transform == nullptr)
		{
			throw std::runtime_error("엔티티에 트랜스폼이 없습니다");
		}
		return *Transform;
	};

	sol::usertype<FScriptEntity> EntityType = Lua.new_usertype<FScriptEntity>(
		"Entity",
		sol::no_constructor,
		"Id", sol::readonly_property([](const FScriptEntity& Entity) { return Entity.Entity.Index; }),
		sol::meta_function::equal_to, [](const FScriptEntity& A, const FScriptEntity& B) { return A.Entity == B.Entity; },
		sol::meta_function::to_string, [this](const FScriptEntity& Entity) {
			const FNameComponent* Name = Scene ? Scene->GetRegistry().TryGet<FNameComponent>(Entity.Entity) : nullptr;
			return std::format("Entity(#{} {})", Entity.Entity.Index, Name ? Name->Name : std::string("<이름 없음>"));
		},
		"IsValid", [this](const FScriptEntity& Entity) { return Scene != nullptr && Scene->GetRegistry().IsValid(Entity.Entity); },
		"GetName", [RequireEntity](const FScriptEntity& Entity) {
			const FNameComponent* Name = RequireEntity(Entity).TryGet<FNameComponent>(Entity.Entity);
			return Name ? Name->Name : std::string();
		},
		"SetName", [RequireEntity](const FScriptEntity& Entity, const std::string& NewName) {
			RequireEntity(Entity).GetOrEmplace<FNameComponent>(Entity.Entity).Name = NewName;
		},

		// ---- 트랜스폼 빠른 경로 (리플렉션 조회 없이)
		"GetPosition", [RequireTransform](const FScriptEntity& Entity) { return RequireTransform(Entity).Position; },
		"SetPosition", [RequireTransform](const FScriptEntity& Entity, const FVector3& Value) { RequireTransform(Entity).Position = Value; },
		"GetRotation", [RequireTransform](const FScriptEntity& Entity) { return RequireTransform(Entity).Rotation; },
		"SetRotation", [RequireTransform](const FScriptEntity& Entity, const FQuat& Value) { RequireTransform(Entity).Rotation = Value.GetNormalized(); },
		"GetScale", [RequireTransform](const FScriptEntity& Entity) { return RequireTransform(Entity).Scale; },
		"SetScale", [RequireTransform](const FScriptEntity& Entity, const FVector3& Value) { RequireTransform(Entity).Scale = Value; },
		// 월드 값은 직전 트랜스폼 갱신 기준 (이번 프레임 SetPosition은 다음 갱신 후 반영)
		"GetWorldPosition", [RequireTransform](const FScriptEntity& Entity) { return RequireTransform(Entity).GetWorldPosition(); },
		"GetForward", [RequireTransform](const FScriptEntity& Entity) { return RequireTransform(Entity).WorldMatrix.GetAxisX().GetNormalized(); },
		"GetRight", [RequireTransform](const FScriptEntity& Entity) { return RequireTransform(Entity).WorldMatrix.GetAxisY().GetNormalized(); },
		"GetUp", [RequireTransform](const FScriptEntity& Entity) { return RequireTransform(Entity).WorldMatrix.GetAxisZ().GetNormalized(); },

		// ---- 계층
		"GetParent", [this, RequireEntity](const FScriptEntity& Entity) -> sol::object {
			RequireEntity(Entity);
			const FEntity Parent = Scene->GetParent(Entity.Entity);
			return Parent.IsValid() ? sol::make_object(Lua, FScriptEntity{ Parent }) : sol::object(sol::lua_nil);
		},
		"FindChild", [this, RequireEntity](const FScriptEntity& Entity, const std::string& Name) -> sol::object {
			RequireEntity(Entity);
			for (const FEntity Child : Scene->GetChildren(Entity.Entity))
			{
				const FNameComponent* ChildName = Scene->GetRegistry().TryGet<FNameComponent>(Child);
				if (ChildName != nullptr && ChildName->Name == Name)
				{
					return sol::make_object(Lua, FScriptEntity{ Child });
				}
			}
			return sol::lua_nil;
		},
		"SetParent", [this, RequireEntity](const FScriptEntity& Entity, sol::optional<FScriptEntity> Parent) {
			RequireEntity(Entity);
			Scene->SetParent(Entity.Entity, Parent ? Parent->Entity : NullEntity);
			bStructureChanged = true;
		},

		// ---- 컴포넌트 (리플렉션)
		"HasComponent", [RequireEntity](const FScriptEntity& Entity, const std::string& TypeName) {
			const FTypeInfo* Type = FindComponentType(TypeName);
			return Type != nullptr && Type->HasComponent(RequireEntity(Entity), Entity.Entity);
		},
		"GetComponent", [this, RequireEntity](const FScriptEntity& Entity, const std::string& TypeName) -> sol::object {
			const FTypeInfo& Type = RequireComponentType(TypeName);
			if (!Type.HasComponent(RequireEntity(Entity), Entity.Entity))
			{
				return sol::lua_nil;
			}
			return sol::make_object(Lua, FScriptComponentRef{ Entity.Entity, &Type });
		},
		"AddComponent", [this, RequireEntity](const FScriptEntity& Entity, const std::string& TypeName) {
			const FTypeInfo& Type = RequireComponentType(TypeName);
			Type.AddComponent(RequireEntity(Entity), Entity.Entity);
			bStructureChanged = true;
			return FScriptComponentRef{ Entity.Entity, &Type };
		},
		"RemoveComponent", [this, RequireEntity](const FScriptEntity& Entity, const std::string& TypeName) {
			const FTypeInfo& Type = RequireComponentType(TypeName);
			if (!Type.bRemovable)
			{
				throw std::runtime_error(std::format("컴포넌트 '{}'는 제거할 수 없습니다", Type.Name));
			}
			Type.RemoveComponent(RequireEntity(Entity), Entity.Entity);
			bStructureChanged = true;
		},
		// 같은 엔티티의 스크립트 인스턴스(self 테이블). 없으면 nil
		"GetScript", [this](const FScriptEntity& Entity) -> sol::object {
			const auto Found = Instances.find(Entity.Entity.ToId());
			return Found != Instances.end() && Found->second.Self.valid() ? sol::object(Found->second.Self) : sol::object(sol::lua_nil);
		},
		// 이번 프레임 스크립트 갱신이 끝난 뒤 (자식 포함) 파괴
		"Destroy", [this](const FScriptEntity& Entity) { PendingDestroy.push_back(Entity.Entity); });

	// ---- 애니메이션 (FAnimationComponent가 있는 모델 루트). BlendTime 생략 시 컴포넌트 값
	EntityType["PlayAnimation"] = [RequireEntity, this](const FScriptEntity& Entity, const std::string& Clip, sol::optional<float> BlendTime) {
		RequireEntity(Entity);
		return FAnimationSystem::Play(*Scene, Entity.Entity, Clip, BlendTime.value_or(-1.0f));
	};
	EntityType["StopAnimation"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		FAnimationSystem::Stop(*Scene, Entity.Entity);
	};
	EntityType["ResumeAnimation"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		FAnimationSystem::Resume(*Scene, Entity.Entity);
	};
	EntityType["SetAnimationSpeed"] = [RequireEntity, this](const FScriptEntity& Entity, float Speed) {
		RequireEntity(Entity);
		FAnimationSystem::SetSpeed(*Scene, Entity.Entity, Speed);
	};
	EntityType["GetAnimationClip"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		return FAnimationSystem::GetCurrentClip(*Scene, Entity.Entity);
	};
	EntityType["GetAnimationClips"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		return sol::as_table(FAnimationSystem::GetClipNames(*Scene, Entity.Entity));
	};

	// ---- 소켓 부착 (Target 모델의 .emeta 소켓). 붙이면 로컬 트랜스폼을 소켓 위치로 맞추고, 떼면 현재 월드 위치를 유지한다
	EntityType["AttachToSocket"] = [RequireEntity, this](const FScriptEntity& Entity, const FScriptEntity& Target, const std::string& Socket) {
		FRegistry& Registry = RequireEntity(Entity);
		FMatrix4x4 SocketWorld;
		if (!Scene->GetSocketWorldMatrix(Target.Entity, Socket, SocketWorld))
		{
			return false;
		}
		FSocketAttachmentComponent& Attachment = Registry.GetOrEmplace<FSocketAttachmentComponent>(Entity.Entity);
		Attachment.Target                       = Target.Entity;
		Attachment.Socket                       = Socket;
		if (FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(Entity.Entity))
		{
			Transform->Position = FVector3::ZeroVector;
			Transform->Rotation = FQuat::Identity;
		}
		return true;
	};
	EntityType["DetachFromSocket"] = [RequireEntity, this](const FScriptEntity& Entity) {
		FRegistry& Registry = RequireEntity(Entity);
		if (!Registry.Has<FSocketAttachmentComponent>(Entity.Entity))
		{
			return;
		}
		Registry.Remove<FSocketAttachmentComponent>(Entity.Entity);
		if (FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(Entity.Entity))
		{
			const FMatrix4x4 Local = Transform->WorldMatrix * Scene->GetParentWorldMatrix(Entity.Entity).GetInverse();
			Local.Decompose(Transform->Position, Transform->Rotation, Transform->Scale);
		}
	};

	// ---- 오디오 (AudioSourceComponent). 앱이 훅을 연결하지 않았으면 아무것도 하지 않는다
	EntityType["PlaySound"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		if (AudioHooks && AudioHooks->Play)
		{
			AudioHooks->Play(Entity.Entity);
		}
	};
	EntityType["StopSound"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		if (AudioHooks && AudioHooks->Stop)
		{
			AudioHooks->Stop(Entity.Entity);
		}
	};

	// ---- 물리 (RigidBodyComponent + 콜라이더, 동적 바디). 훅이 없거나 바디가 없으면 무시 / 0 벡터
	EntityType["AddForce"] = [RequireEntity, this](const FScriptEntity& Entity, const FVector3& Force) {
		RequireEntity(Entity);
		if (PhysicsHooks && PhysicsHooks->AddForce)
		{
			PhysicsHooks->AddForce(Entity.Entity, Force);
		}
	};
	EntityType["AddImpulse"] = [RequireEntity, this](const FScriptEntity& Entity, const FVector3& Impulse) {
		RequireEntity(Entity);
		if (PhysicsHooks && PhysicsHooks->AddImpulse)
		{
			PhysicsHooks->AddImpulse(Entity.Entity, Impulse);
		}
	};
	EntityType["SetVelocity"] = [RequireEntity, this](const FScriptEntity& Entity, const FVector3& Velocity) {
		RequireEntity(Entity);
		if (PhysicsHooks && PhysicsHooks->SetVelocity)
		{
			PhysicsHooks->SetVelocity(Entity.Entity, Velocity);
		}
	};
	EntityType["GetVelocity"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		return PhysicsHooks && PhysicsHooks->GetVelocity ? PhysicsHooks->GetVelocity(Entity.Entity) : FVector3();
	};
	EntityType["GetMass"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		return PhysicsHooks && PhysicsHooks->GetMass ? PhysicsHooks->GetMass(Entity.Entity) : 0.0f;
	};

	// ---- 네트워크 소유권: 소유 플레이어 ID (-1 = 서버 소유), 이 기계가 조종 권한을 갖는가
	//   서버 소유(-1)는 서버(Standalone 포함)에서 참, 플레이어 소유는 그 플레이어의 기계에서 참
	// 캐릭터 이동 (CharacterMovementComponent): 이 캐릭터를 조종하는 쪽(소유 클라이언트/호스트, 서버 소유면 서버)에서 매 프레임 넘긴다
	EntityType["AddMovementInput"] = [RequireEntity, this](const FScriptEntity& Entity, const FVector3& Direction) {
		RequireEntity(Entity);
		if (PhysicsHooks && PhysicsHooks->AddMovementInput)
		{
			PhysicsHooks->AddMovementInput(Entity.Entity, Direction);
		}
	};
	EntityType["Jump"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		if (PhysicsHooks && PhysicsHooks->Jump)
		{
			PhysicsHooks->Jump(Entity.Entity);
		}
	};
	EntityType["IsGrounded"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		return PhysicsHooks && PhysicsHooks->IsGrounded && PhysicsHooks->IsGrounded(Entity.Entity);
	};

	EntityType["GetOwner"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		return GetOwner(Entity.Entity);
	};
	EntityType["IsLocallyOwned"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		const int32 Owner = GetOwner(Entity.Entity);
		return Owner < 0 ? (NetHooks == nullptr || NetHooks->bIsServer) : Owner == GetLocalPlayerId();
	};

	// 시점 방향 (yaw, pitch 도): 서버에서는 소유 플레이어가 보낸 값, 클라이언트는 자기 소유 엔티티만
	EntityType["GetControlRotation"] = [RequireEntity, this](const FScriptEntity& Entity) {
		RequireEntity(Entity);
		const FVector2 Value = NetHooks != nullptr && NetHooks->GetControlRotation ? NetHooks->GetControlRotation(Entity.Entity) : LocalControlRotation;
		return std::make_tuple(Value.X, Value.Y);
	};

	// ---- RPC: entity:CallServer("Fire", 인자...) → 그 엔티티 스크립트의 Server_Fire(self, 인자...) (Client_/Multicast_도 같은 규칙)
	EntityType["CallServer"] = [RequireEntity, this](const FScriptEntity& Entity, const std::string& Name, sol::variadic_args Args) {
		RequireEntity(Entity);
		CallRpc(Entity.Entity, EGameRpcKind::Server, Name, Args);
	};
	EntityType["CallClient"] = [RequireEntity, this](const FScriptEntity& Entity, const std::string& Name, sol::variadic_args Args) {
		RequireEntity(Entity);
		CallRpc(Entity.Entity, EGameRpcKind::Client, Name, Args);
	};
	EntityType["CallMulticast"] = [RequireEntity, this](const FScriptEntity& Entity, const std::string& Name, sol::variadic_args Args) {
		RequireEntity(Entity);
		CallRpc(Entity.Entity, EGameRpcKind::Multicast, Name, Args);
	};

	Lua.new_usertype<FScriptComponentRef>(
		"Component",
		sol::no_constructor,
		sol::meta_function::index,
		[this, RequireRegistry](const FScriptComponentRef& Ref, const std::string& Key) -> sol::object {
			void* Component = Ref.Type->GetComponent(RequireRegistry(), Ref.Entity);
			if (Component == nullptr)
			{
				throw std::runtime_error(std::format("컴포넌트 '{}'가 더 이상 없습니다", Ref.Type->Name));
			}
			const FPropertyInfo* Property = Ref.Type->FindProperty(Key);
			if (Property == nullptr)
			{
				throw std::runtime_error(std::format("컴포넌트 '{}'에 프로퍼티 '{}'가 없습니다", Ref.Type->Name, Key));
			}
			return ReadProperty(Lua, *Property, Component);
		},
		sol::meta_function::new_index,
		[this, RequireRegistry](const FScriptComponentRef& Ref, const std::string& Key, const sol::object& Value) {
			void* Component = Ref.Type->GetComponent(RequireRegistry(), Ref.Entity);
			if (Component == nullptr)
			{
				throw std::runtime_error(std::format("컴포넌트 '{}'가 더 이상 없습니다", Ref.Type->Name));
			}
			const FPropertyInfo* Property = Ref.Type->FindProperty(Key);
			if (Property == nullptr)
			{
				throw std::runtime_error(std::format("컴포넌트 '{}'에 프로퍼티 '{}'가 없습니다", Ref.Type->Name, Key));
			}
			WriteProperty(*Property, Component, Value);
			if (Property->Type == EPropertyType::String && !Property->AssetFilter.empty())
			{
				bStructureChanged = true; // 에셋 경로가 바뀌었다 → 앱이 핸들을 다시 해석 (FSceneAssetResolver)
			}
		},
		sol::meta_function::to_string, [](const FScriptComponentRef& Ref) { return std::format("Component({})", Ref.Type->Name); });

	// ---- Scene 전역 테이블
	sol::table SceneTable = Lua.create_named_table("Scene");
	SceneTable["Find"] = [this, RequireRegistry](const std::string& Name) -> sol::object {
		// 선형 탐색: OnStart에서 찾아 self에 보관할 것 (매 프레임 호출 금지)
		FEntity Found;
		RequireRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& NameComponent) {
			if (!Found.IsValid() && NameComponent.Name == Name)
			{
				Found = Entity;
			}
		});
		return Found.IsValid() ? sol::make_object(Lua, FScriptEntity{ Found }) : sol::object(sol::lua_nil);
	};
	SceneTable["Create"] = [this, RequireRegistry](sol::optional<std::string> Name) {
		RequireRegistry();
		bStructureChanged = true;
		return FScriptEntity{ Scene->CreateEntity(Name.value_or("Entity")) };
	};
	SceneTable["Destroy"] = [this](const FScriptEntity& Entity) { PendingDestroy.push_back(Entity.Entity); };
}

void FLuaRuntime::RegisterGlobals()
{
	// ---- Log.Info/Warn/Error(...) — 인자를 tostring 후 공백으로 이어 붙인다. print도 Log.Info로 연결
	const auto MakeLogFunction = [this](ELogVerbosity Verbosity) {
		return [this, Verbosity](sol::variadic_args Args) {
			sol::protected_function ToString = Lua["tostring"];
			std::string             Message;
			for (const sol::stack_proxy Arg : Args)
			{
				if (!Message.empty())
				{
					Message += ' ';
				}
				sol::protected_function_result Result = ToString(sol::object(Arg));
				Message += Result.valid() ? Result.get<std::string>() : std::string("?");
			}
			if (FLog::ShouldLog(LogScript, Verbosity))
			{
				FLog::Write(LogScript, Verbosity, "[Lua] " + Message);
			}
		};
	};
	sol::table LogTable = Lua.create_named_table("Log");
	LogTable["Info"]    = MakeLogFunction(ELogVerbosity::Display);
	LogTable["Warn"]    = MakeLogFunction(ELogVerbosity::Warning);
	LogTable["Error"]   = MakeLogFunction(ELogVerbosity::Error);
	Lua["print"]        = MakeLogFunction(ELogVerbosity::Display);

	// ---- Input (입력이 없으면 항상 false / 0)
	sol::table InputTable = Lua.create_named_table("Input");
	InputTable["IsKeyDown"]     = [this](const std::string& Key) { const EKey Code = ParseKey(Key); return Input && Input->IsKeyDown(Code); };
	InputTable["IsKeyPressed"]  = [this](const std::string& Key) { const EKey Code = ParseKey(Key); return Input && Input->IsKeyPressed(Code); };
	InputTable["IsKeyReleased"] = [this](const std::string& Key) { const EKey Code = ParseKey(Key); return Input && Input->IsKeyReleased(Code); };
	InputTable["IsMouseDown"]   = [this](const std::string& Button) {
		const EMouseButton Code = ParseMouseButton(Button);
		return Input && Input->IsMouseButtonDown(Code);
	};
	InputTable["GetMouseDelta"] = [this]() {
		return Input ? std::make_tuple(Input->GetMouseDeltaX(), Input->GetMouseDeltaY()) : std::make_tuple(0, 0);
	};
	// 원시 마우스 이동량 (시점 회전용 — 화면 가장자리/커서 잠금과 무관). 서버의 원격 입력에는 없다(0): 시점은 Net.SetControlRotation으로 보낸다
	InputTable["GetLookDelta"] = [this]() {
		return Input ? std::make_tuple(Input->GetLookDeltaX(), Input->GetLookDeltaY()) : std::make_tuple(0.0f, 0.0f);
	};

	// ---- 입력 액션 (프로젝트 설정 "입력" + 플레이어 재지정). 서버에서는 소유 플레이어가 보낸 액션 값
	//   Input.GetAction("Move") → 2D는 x, y / 1D는 숫자 / 버튼은 bool. 입력이 없으면 0/false, 모르는 액션은 오류
	//   Input.IsActionPressed(이름) = 누르고 있음(작동 중), WasActionPressed/WasActionReleased = 이번 프레임에 시작/끝
	const auto ResolveAction = [this](const std::string& Name) {
		if (Input != nullptr)
		{
			if (const FInputActionState* State = Input->FindAction(Name))
			{
				return *State;
			}
		}
		const FInputAction* Action = FProjectSettings::Get().Input.GetEffectiveMapping().Find(Name);
		if (Action == nullptr)
		{
			throw std::runtime_error(std::format("알 수 없는 입력 액션: '{}' (프로젝트 설정 → 입력)", Name));
		}
		FInputActionState Empty;
		Empty.Name = Action->Name;
		Empty.Type = Action->Type;
		return Empty;
	};
	InputTable["GetAction"] = [ResolveAction](const std::string& Name, sol::this_state State) {
		const FInputActionState Action = ResolveAction(Name);
		sol::variadic_results   Results;
		switch (Action.Type)
		{
		case EInputActionType::Button:
			Results.push_back(sol::make_object(State, Action.bActive));
			break;
		case EInputActionType::Axis1D:
			Results.push_back(sol::make_object(State, Action.Value.X));
			break;
		case EInputActionType::Axis2D:
			Results.push_back(sol::make_object(State, Action.Value.X));
			Results.push_back(sol::make_object(State, Action.Value.Y));
			break;
		}
		return Results;
	};
	InputTable["IsActionPressed"]   = [ResolveAction](const std::string& Name) { return ResolveAction(Name).bActive; };
	InputTable["WasActionPressed"]  = [ResolveAction](const std::string& Name) {
		const FInputActionState Action = ResolveAction(Name);
		return Action.bActive && !Action.bWasActive;
	};
	InputTable["WasActionReleased"] = [ResolveAction](const std::string& Name) {
		const FInputActionState Action = ResolveAction(Name);
		return !Action.bActive && Action.bWasActive;
	};
	// 재지정 (플레이어 설정, <Saved>/Config/InputBindings.json에 저장 — 자동 검증 실행은 저장 안 함)
	//   Input.Rebind("Jump", "Space", "K") → bool (이전 입력이 nil/""이면 새 바인딩 추가), Input.ResetBindings(["Jump"]),
	//   Input.GetBindings("Jump") → { "Space", "Gamepad_A" }
	InputTable["Rebind"] = [](const std::string& Action, sol::optional<std::string> OldSource, const std::string& NewSource) {
		const FInputSource Old = OldSource && !OldSource->empty() ? ParseInputSource(*OldSource) : FInputSource();
		return FProjectSettings::Get().Input.Rebind(Action, Old, ParseInputSource(NewSource));
	};
	InputTable["ResetBindings"] = [](sol::optional<std::string> Action) { FProjectSettings::Get().Input.ResetUserBindings(Action ? *Action : std::string()); };
	InputTable["GetBindings"]   = [this](const std::string& Name) {
		const FInputAction* Action = FProjectSettings::Get().Input.GetEffectiveMapping().Find(Name);
		if (Action == nullptr)
		{
			throw std::runtime_error(std::format("알 수 없는 입력 액션: '{}'", Name));
		}
		sol::table Result = Lua.create_table();
		for (size_t Index = 0; Index < Action->Bindings.size(); ++Index)
		{
			Result[Index + 1] = InputNames::ToString(Action->Bindings[Index].Source);
		}
		return Result;
	};
	InputTable["IsGamepadConnected"]  = [this]() { return Input && Input->IsGamepadConnected(); };
	InputTable["IsGamepadButtonDown"] = [this](const std::string& Button) {
		EGamepadButton Code;
		if (!InputNames::TryParseGamepadButton(Button, Code))
		{
			throw std::runtime_error(std::format("알 수 없는 게임패드 버튼: '{}' (A/B/X/Y/LeftShoulder/...)", Button));
		}
		return Input && Input->IsGamepadButtonDown(Code);
	};

	// ---- Time (Update마다 갱신)
	sol::table AudioTable     = Lua.create_named_table("Audio");
	AudioTable["PlayOneShot"] = [this](const std::string& ClipAsset) {
		if (AudioHooks && AudioHooks->PlayOneShot)
		{
			AudioHooks->PlayOneShot(ClipAsset);
		}
	};

	// ---- Physics.Raycast(origin, direction, maxDistance, layers?) → { entity, position, normal, distance } 또는 nil (cm)
	//   layers: 충돌 레이어 이름 표 {"Ground", "Prop"} 또는 이름 하나 (프로젝트 설정 → 충돌 레이어). 생략 = 모든 레이어. 트리거는 항상 제외.
	//   없는 레이어 이름은 Lua 오류
	sol::table PhysicsTable   = Lua.create_named_table("Physics");
	PhysicsTable["Raycast"]   = [this](const FVector3& Origin, const FVector3& Direction, float MaxDistance, sol::object Layers) -> sol::object {
		uint32 LayerMask = FCollisionLayerSettings::AllLayersMask;
		if (Layers.valid() && Layers.get_type() != sol::type::lua_nil && Layers.get_type() != sol::type::none)
		{
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
						throw std::runtime_error("Physics.Raycast: 레이어 표에는 이름 문자열만 넣습니다");
					}
					Names.push_back(Value.as<std::string>());
				}
			}
			else
			{
				throw std::runtime_error("Physics.Raycast: 네 번째 인자는 레이어 이름 표나 이름이어야 합니다");
			}
			std::string Unknown;
			if (!FProjectSettings::Get().Collision.MakeMask(Names, LayerMask, &Unknown))
			{
				throw std::runtime_error(std::format("Physics.Raycast: 없는 충돌 레이어 '{}' (프로젝트 설정 → 충돌 레이어)", Unknown));
			}
		}
		FScriptRayHit Hit;
		bool          bHit = false;
		if (PhysicsHooks != nullptr)
		{
			if (LayerMask != FCollisionLayerSettings::AllLayersMask && PhysicsHooks->RaycastLayers)
			{
				bHit = PhysicsHooks->RaycastLayers(Origin, Direction, MaxDistance, LayerMask, Hit);
			}
			else if (LayerMask == FCollisionLayerSettings::AllLayersMask && PhysicsHooks->Raycast)
			{
				bHit = PhysicsHooks->Raycast(Origin, Direction, MaxDistance, Hit);
			}
		}
		if (!bHit)
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

	sol::table TimeTable     = Lua.create_named_table("Time");
	TimeTable["DeltaTime"]   = 0.0;
	TimeTable["TotalTime"]   = 0.0;
	TimeTable["FrameCount"]  = 0;
}

// ---------------------------------------------------------------- 값 변환

FScriptValue FLuaRuntime::ToScriptValue(const sol::object& Object)
{
	switch (Object.get_type())
	{
	case sol::type::boolean: return FScriptValue::MakeBool(Object.as<bool>());
	case sol::type::number:
	{
		lua_State* L = Lua.lua_state();
		Object.push(L);
		const bool   bInteger = lua_isinteger(L, -1) != 0;
		const double Number   = lua_tonumber(L, -1);
		lua_pop(L, 1);
		return FScriptValue::MakeNumber(Number, bInteger);
	}
	case sol::type::string: return FScriptValue::MakeString(Object.as<std::string>());
	case sol::type::userdata:
		if (Object.is<FVector3>())
		{
			return FScriptValue::MakeVector3(Object.as<FVector3>());
		}
		if (Object.is<FScriptAssetRef>())
		{
			const FScriptAssetRef& Ref = Object.as<const FScriptAssetRef&>();
			return FScriptValue::MakeAsset(Ref.Path, Ref.Filter);
		}
		return FScriptValue{};
	default: return FScriptValue{};
	}
}

sol::object FLuaRuntime::FromScriptValue(const FScriptValue& Value)
{
	switch (Value.Type)
	{
	case EScriptValueType::Bool:    return sol::make_object(Lua, Value.bBool);
	case EScriptValueType::Number:
		return Value.bInteger ? sol::make_object(Lua, static_cast<lua_Integer>(Value.Number)) : sol::make_object(Lua, Value.Number);
	case EScriptValueType::String:  return sol::make_object(Lua, Value.String);
	case EScriptValueType::Vector3: return sol::make_object(Lua, Value.Vector);
	case EScriptValueType::Asset:   return sol::make_object(Lua, FScriptAssetRef{ Value.String, Value.AssetFilter });
	default:                        return sol::lua_nil;
	}
}

sol::object FLuaRuntime::CopyValue(const sol::object& Object)
{
	if (Object.get_type() == sol::type::userdata)
	{
		if (Object.is<FVector3>()) return sol::make_object(Lua, Object.as<FVector3>());
		if (Object.is<FQuat>()) return sol::make_object(Lua, Object.as<FQuat>());
		if (Object.is<FVector2>()) return sol::make_object(Lua, Object.as<FVector2>());
		if (Object.is<FVector4>()) return sol::make_object(Lua, Object.as<FVector4>());
	}
	else if (Object.get_type() == sol::type::table)
	{
		// 한 단계 복사 (인스턴스끼리 기본값 테이블을 공유하지 않도록)
		sol::table Copy = Lua.create_table();
		for (const auto& [Key, Value] : Object.as<sol::table>())
		{
			Copy[Key] = CopyValue(Value);
		}
		return Copy;
	}
	return Object;
}

// ---------------------------------------------------------------- 클래스 로드

std::string FLuaRuntime::MakeClassKey(const std::filesystem::path& AbsolutePath) const
{
	std::error_code       ErrorCode;
	std::filesystem::path Canonical = std::filesystem::weakly_canonical(AbsolutePath, ErrorCode);
	if (ErrorCode)
	{
		Canonical = AbsolutePath;
	}
	std::wstring Key = Canonical.lexically_normal().wstring();
	std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
	return FStringConv::ToUtf8(Key);
}

bool FLuaRuntime::ExecuteClassFile(FScriptClass& Class, const std::filesystem::path& AbsolutePath)
{
	bool              bRead  = false;
	const std::string Source = ReadScriptSource(AbsolutePath, bRead);
	if (!bRead)
	{
		Class.Error = "스크립트 파일을 열 수 없습니다: " + FStringConv::ToUtf8(AbsolutePath.wstring());
		return false;
	}

	sol::load_result Chunk = Lua.load(Source, "@" + Class.AssetName);
	if (!Chunk.valid())
	{
		const sol::error Error = Chunk;
		Class.Error            = Error.what();
		return false;
	}

	sol::protected_function Function(Chunk.get<sol::function>(), Traceback);
	ExecutingFiles.push_back(MakeClassKey(AbsolutePath)); // 파일 맨 위의 Script.Require가 이 클래스를 의존자로 기록한다
	sol::protected_function_result Result = Function();
	ExecutingFiles.pop_back();
	if (!Result.valid())
	{
		const sol::error Error = Result;
		Class.Error            = Error.what();
		return false;
	}
	sol::object Returned = Result;
	if (Returned.get_type() != sol::type::table)
	{
		Class.Error = "스크립트는 클래스 테이블을 반환해야 합니다 (파일 끝에 return MyScript)";
		return false;
	}

	Class.Class = Returned.as<sol::table>();
	if (!Class.Metatable.valid())
	{
		Class.Metatable = Lua.create_table();
	}
	Class.Metatable["__index"] = Class.Class;

	Class.Decls.clear();
	const sol::object PropertiesObject = Class.Class["Properties"];
	if (PropertiesObject.get_type() == sol::type::table)
	{
		for (const auto& [Key, Value] : PropertiesObject.as<sol::table>())
		{
			if (Key.get_type() != sol::type::string)
			{
				continue;
			}
			FScriptValue Default = ToScriptValue(Value);
			if (!Default.IsNil())
			{
				Class.Decls.push_back({ Key.as<std::string>(), std::move(Default) });
			}
		}
		std::sort(Class.Decls.begin(), Class.Decls.end(), [](const FScriptPropertyDecl& A, const FScriptPropertyDecl& B) { return A.Name < B.Name; });
	}

	Class.bValid = true;
	Class.Error.clear();
	return true;
}

FLuaRuntime::FScriptClass& FLuaRuntime::LoadClass(const std::string& ScriptAsset)
{
	const std::filesystem::path AbsolutePath = ContentDirectory / FStringConv::ToWide(ScriptAsset);
	const std::string           Key          = MakeClassKey(AbsolutePath);
	if (const auto Found = Classes.find(Key); Found != Classes.end())
	{
		return *Found->second;
	}

	std::unique_ptr<FScriptClass> Class = std::make_unique<FScriptClass>();
	Class->AssetName                    = ScriptAsset;
	if (!ExecuteClassFile(*Class, AbsolutePath))
	{
		ReportError(std::format("스크립트 로드 실패: {}\n{}", ScriptAsset, Class->Error));
	}
	FScriptClass& Result = *Class;
	Classes.emplace(Key, std::move(Class));
	return Result;
}

bool FLuaRuntime::ReloadClass(const std::filesystem::path& ScriptPath, bool& bOutSucceeded)
{
	bOutSucceeded         = false;
	const std::string Key = MakeClassKey(ScriptPath);
	if (Modules.contains(Key) && !Classes.contains(Key))
	{
		return ReloadModule(Key, ScriptPath, bOutSucceeded); // Script.Require로 읽은 파일 (클래스가 아님)
	}
	return ReloadClassByKey(Key, ScriptPath, bOutSucceeded);
}

bool FLuaRuntime::ReloadClassByKey(const std::string& Key, const std::filesystem::path& ScriptPath, bool& bOutSucceeded)
{
	bOutSucceeded    = false;
	const auto        Found = Classes.find(Key);
	if (Found == Classes.end())
	{
		return false;
	}

	// 새 코드는 임시 항목에 실행해 보고, 성공했을 때만 교체한다 (실패 시 기존 클래스 유지)
	FScriptClass& Class = *Found->second;
	FScriptClass  Candidate;
	Candidate.AssetName = Class.AssetName;
	Candidate.Metatable = Class.Metatable; // 공유 메타테이블을 그대로 쓰면 인스턴스가 즉시 새 함수를 본다
	if (!ExecuteClassFile(Candidate, ScriptPath))
	{
		// 실패: 메타테이블은 이전 클래스를 가리키도록 되돌린다
		if (Class.bValid && Class.Metatable.valid())
		{
			Class.Metatable["__index"] = Class.Class;
		}
		ReportError(std::format("스크립트 다시 로드 실패 (기존 코드 유지): {}\n{}", Class.AssetName, Candidate.Error));
		return true;
	}

	const bool bWasValid = Class.bValid;
	Class                = std::move(Candidate);
	bOutSucceeded        = true;
	E_LOG(LogScript, Display, "스크립트 다시 로드: {}", Class.AssetName);

	// 인스턴스 갱신: 새 Properties 항목의 기본값 추가, 오류로 멈춘 인스턴스 재개. 이전에 로드 실패였다면 처음부터 다시 만든다
	std::vector<uint64> Recreate;
	for (auto& [Id, Instance] : Instances)
	{
		if (Instance.ClassKey != Key)
		{
			continue;
		}
		ClearInstanceTasks(Instance); // 옛 코드 클로저를 가진 타이머/코루틴은 버린다 (Timer/Coroutine 규칙 — ScriptTimerBindings.cpp)
		if (!bWasValid || !Instance.Self.valid())
		{
			Recreate.push_back(Id);
			continue;
		}
		Instance.bFaulted       = false;
		sol::table Properties   = Instance.Self["Properties"].get_or_create<sol::table>();
		for (const FScriptPropertyDecl& Decl : Class.Decls)
		{
			const sol::object Existing = Properties[Decl.Name];
			if (!Existing.valid() || Existing.get_type() == sol::type::lua_nil)
			{
				Properties[Decl.Name] = FromScriptValue(Decl.Default);
			}
		}
	}
	for (uint64 Id : Recreate)
	{
		Instances.erase(Id); // 다음 Update에서 새로 만들고 OnStart 호출
	}
	// 스크립트 객체(Lua 트리 노드): 오류로 멈춘 것 재개. 메타테이블을 공유하므로 새 함수는 이미 보인다
	for (auto& [Id, Object] : Objects)
	{
		if (Object.Self.valid() && MakeClassKey(ContentDirectory / FStringConv::ToWide(Object.ScriptAsset)) == Key)
		{
			Object.bFaulted = false;
		}
	}
	return true;
}

// ---------------------------------------------------------------- 인스턴스

void FLuaRuntime::CreateInstance(FEntity Entity, const std::string& ScriptAsset, const std::string& Overrides)
{
	FScriptClass& Class = LoadClass(ScriptAsset);

	FScriptInstance Instance;
	Instance.Entity      = Entity;
	Instance.ScriptAsset = ScriptAsset;
	Instance.ClassKey    = MakeClassKey(ContentDirectory / FStringConv::ToWide(ScriptAsset));

	if (!Class.bValid)
	{
		Instance.bFaulted = true; // 로드 오류는 LoadClass에서 한 번만 보고. 핫 리로드 성공 시 다시 만든다
		Instances.emplace(Entity.ToId(), std::move(Instance));
		return;
	}

	sol::table Self            = Lua.create_table();
	Self["entity"]             = FScriptEntity{ Entity };
	Self["Properties"]         = MakeProperties(Class, ScriptAsset, Overrides);
	Self[sol::metatable_key]   = Class.Metatable;
	Instance.Self              = Self;
	Instances.emplace(Entity.ToId(), std::move(Instance));
}

sol::table FLuaRuntime::MakeProperties(const FScriptClass& Class, const std::string& ScriptAsset, const std::string& Overrides)
{
	sol::table        Properties = Lua.create_table();
	const sol::object Defaults   = Class.Class["Properties"];
	if (Defaults.get_type() == sol::type::table)
	{
		for (const auto& [Key, Value] : Defaults.as<sol::table>())
		{
			Properties[Key] = CopyValue(Value);
		}
	}
	for (const auto& [Name, Value] : FScriptProperties::ParseOverrides(Overrides))
	{
		const auto Decl = std::find_if(Class.Decls.begin(), Class.Decls.end(), [&](const FScriptPropertyDecl& Item) { return Item.Name == Name; });
		if (Decl == Class.Decls.end())
		{
			E_LOG(LogScript, Warning, "{}: 선언되지 않은 프로퍼티 오버라이드 '{}' 무시", ScriptAsset, Name);
			continue;
		}
		if (!FScriptProperties::IsCompatible(Decl->Default, Value))
		{
			E_LOG(LogScript, Warning, "{}: 프로퍼티 '{}' 오버라이드 타입이 선언과 달라 무시", ScriptAsset, Name);
			continue;
		}
		// 정수/실수 하위 타입은 선언을 따른다
		FScriptValue Typed = Value;
		if (Typed.Type == EScriptValueType::Number)
		{
			Typed.bInteger = Decl->Default.bInteger;
		}
		Typed.AssetFilter = Decl->Default.AssetFilter; // 에셋 확장자는 선언을 따른다
		Properties[Name] = FromScriptValue(Typed);
	}
	return Properties;
}

bool FLuaRuntime::CallMethod(FScriptInstance& Instance, const char* MethodName, float DeltaSeconds, bool bPassDelta)
{
	const sol::object Method = Instance.Self[MethodName];
	if (Method.get_type() != sol::type::function)
	{
		return true;
	}

	// 호출 중 Instances가 바뀔 수 있으므로(다른 스크립트의 GetScript 등은 읽기만 하지만) 필요한 값은 복사해 둔다
	const sol::table        Self = Instance.Self;
	sol::protected_function Function(Method.as<sol::function>(), Traceback);
	const FInstanceScope    Scope(*this, Instance.Entity);
	sol::protected_function_result Result = bPassDelta ? Function(Self, DeltaSeconds) : Function(Self);
	if (!Result.valid())
	{
		const sol::error Error = Result;
		FaultInstance(Instance, MethodName, Error.what());
		return false;
	}
	return true;
}

void FLuaRuntime::DestroyInstance(uint64 EntityId)
{
	const auto Found = Instances.find(EntityId);
	if (Found == Instances.end())
	{
		return;
	}
	FScriptInstance& Instance = Found->second;
	if (Instance.bStarted && !Instance.bFaulted && Scene != nullptr && Scene->GetRegistry().IsValid(Instance.Entity))
	{
		CallMethod(Instance, "OnDestroy");
	}
	ClearInstanceTasks(Instance); // OnDestroy가 만든 것까지
	Instances.erase(EntityId);
}

void FLuaRuntime::ApplyPendingDestroys()
{
	// OnDestroy가 또 파괴를 요청할 수 있으므로 몇 차례 반복 (무한 연쇄 방지)
	for (int32 Pass = 0; Pass < 8 && !PendingDestroy.empty(); ++Pass)
	{
		std::vector<FEntity> Batch;
		Batch.swap(PendingDestroy);
		for (FEntity Root : Batch)
		{
			if (!Scene->GetRegistry().IsValid(Root))
			{
				continue;
			}
			std::vector<FEntity> Stack{ Root };
			while (!Stack.empty())
			{
				const FEntity Entity = Stack.back();
				Stack.pop_back();
				DestroyInstance(Entity.ToId());
				for (FEntity Child : Scene->GetChildren(Entity))
				{
					Stack.push_back(Child);
				}
			}
			Scene->DestroyEntity(Root);
			bStructureChanged = true;
		}
	}
	PendingDestroy.clear();
}

void FLuaRuntime::Update(float DeltaSeconds, const FInput* InInput)
{
	if (Scene == nullptr)
	{
		return;
	}
	Input = InInput;
	TotalTime += DeltaSeconds;
	++FrameCount;
	sol::table TimeTable     = Lua["Time"];
	TimeTable["DeltaTime"]   = DeltaSeconds;
	TimeTable["TotalTime"]   = TotalTime;
	TimeTable["FrameCount"]  = FrameCount;

	FRegistry& Registry = Scene->GetRegistry();

	ApplyPendingSpawns(); // 갱신 밖(RunString 등)에서 요청된 생성

	UpdateOrder.clear();
	Registry.View<FScriptComponent>().Each([&](FEntity Entity, FScriptComponent&) { UpdateOrder.push_back(Entity); });

	// 1. 사라졌거나 스크립트가 바뀐 인스턴스 정리
	std::vector<uint64> Stale;
	for (const auto& [Id, Instance] : Instances)
	{
		const FScriptComponent* Component = Registry.IsValid(Instance.Entity) ? Registry.TryGet<FScriptComponent>(Instance.Entity) : nullptr;
		if (Component == nullptr || Component->ScriptAsset != Instance.ScriptAsset)
		{
			Stale.push_back(Id);
		}
	}
	for (uint64 Id : Stale)
	{
		DestroyInstance(Id);
	}

	// 2. 새 스크립트 컴포넌트 인스턴스화
	for (FEntity Entity : UpdateOrder)
	{
		if (!Instances.contains(Entity.ToId()))
		{
			const FScriptComponent& Component = Registry.Get<FScriptComponent>(Entity);
			if (!Component.ScriptAsset.empty() && ShouldRunHere(Component))
			{
				CreateInstance(Entity, Component.ScriptAsset, Component.PropertyOverrides);
			}
		}
	}

	// 3. OnStart(처음 한 번) → OnUpdate
	for (FEntity Entity : UpdateOrder)
	{
		const auto Found = Instances.find(Entity.ToId());
		if (Found == Instances.end() || Found->second.bFaulted || !Registry.IsValid(Entity) || !Registry.Has<FScriptComponent>(Entity))
		{
			continue;
		}
		FScriptInstance& Instance = Found->second;
		// 이 인스턴스가 보는 입력 (서버: 엔티티 소유 플레이어의 입력)
		Input = NetHooks != nullptr && NetHooks->ResolveInput ? NetHooks->ResolveInput(Entity, InInput) : InInput;
		if (!Instance.bStarted)
		{
			Instance.bStarted = true;
			if (!CallMethod(Instance, "OnStart"))
			{
				continue;
			}
		}
		CallMethod(Instance, "OnUpdate", DeltaSeconds, true);
	}
	Input = InInput;

	// 3.2 타이머 발사 / 코루틴 재개 (모든 OnUpdate 뒤, ScriptTimerBindings.cpp)
	UpdateTasks(DeltaSeconds, InInput);

	// 3.5 애니메이션 노티파이 (직전 프레임 애니메이션 갱신에서 발생) → 게임 UI 이벤트 (이번 프레임 FUISystem::Update에서 발생)
	DispatchAnimNotifies();
	DispatchUIEvents();
	DispatchSequenceEvents(); // 직전 게임플레이 틱 시퀀스 갱신에서 발생
	DispatchMontageEvents();

	// 4. 스크립트가 요청한 프리팹 생성 (갱신 순회·노티파이가 끝난 뒤) → 파괴
	ApplyPendingSpawns();
	ApplyPendingDestroys();
}

void FLuaRuntime::LateUpdate(float DeltaSeconds, const FInput* InInput)
{
	if (Scene == nullptr)
	{
		return;
	}
	FRegistry& Registry = Scene->GetRegistry();
	for (FEntity Entity : UpdateOrder) // 이번 프레임 Update 순서 그대로
	{
		const auto Found = Instances.find(Entity.ToId());
		if (Found == Instances.end() || Found->second.bFaulted || !Found->second.bStarted || !Registry.IsValid(Entity))
		{
			continue;
		}
		Input = NetHooks != nullptr && NetHooks->ResolveInput ? NetHooks->ResolveInput(Entity, InInput) : InInput;
		CallMethod(Found->second, "OnLateUpdate", DeltaSeconds, true);
	}
	Input = InInput;
	ApplyPendingDestroys();
}

void FLuaRuntime::DispatchAnimNotifies()
{
	FRegistry& Registry = Scene->GetRegistry();
	// 받는 쪽: 노티파이가 난 엔티티(모델 루트)의 스크립트, 없으면 가장 가까운 조상의 스크립트 (캐릭터 루트에 스크립트를 두는 경우)
	std::vector<FAnimNotifyEvent> Events;
	Registry.View<FAnimationComponent>().Each([&](FEntity, FAnimationComponent& Animation) {
		Events.insert(Events.end(), Animation.Runtime.PendingNotifies.begin(), Animation.Runtime.PendingNotifies.end());
	});
	for (const FAnimNotifyEvent& Event : Events)
	{
		FScriptInstance* Receiver = nullptr;
		for (FEntity Current = Event.Entity; Current.IsValid() && Registry.IsValid(Current); Current = Scene->GetParent(Current))
		{
			const auto Found = Instances.find(Current.ToId());
			if (Found != Instances.end())
			{
				Receiver = &Found->second;
				break;
			}
		}
		if (Receiver == nullptr || Receiver->bFaulted || !Receiver->bStarted)
		{
			continue;
		}
		switch (Event.Type)
		{
		case EAnimNotifyEventType::Notify:     CallMethod(*Receiver, ("OnAnimNotify_" + Event.Name).c_str()); break;
		case EAnimNotifyEventType::StateBegin: CallMethod(*Receiver, ("OnAnimNotifyBegin_" + Event.Name).c_str()); break;
		case EAnimNotifyEventType::StateTick:  CallMethod(*Receiver, ("OnAnimNotifyTick_" + Event.Name).c_str(), Event.DeltaSeconds, true); break;
		case EAnimNotifyEventType::StateEnd:   CallMethod(*Receiver, ("OnAnimNotifyEnd_" + Event.Name).c_str()); break;
		}
	}
}

void FLuaRuntime::DestroyAllInstances()
{
	std::vector<uint64> Ids;
	Ids.reserve(Instances.size());
	for (const auto& [Id, Instance] : Instances)
	{
		Ids.push_back(Id);
	}
	for (uint64 Id : Ids)
	{
		DestroyInstance(Id);
	}
	Instances.clear();
	Tasks.clear();
	PendingDestroy.clear();
	PendingSpawns.clear();
}

bool FLuaRuntime::RunString(std::string_view Code)
{
	sol::protected_function_result Result = Lua.safe_script(Code, sol::script_pass_on_error, "=RunString");
	if (!Result.valid())
	{
		const sol::error Error = Result;
		ReportError(std::format("Lua 실행 오류: {}", Error.what()));
		return false;
	}
	return true;
}

FScriptValue FLuaRuntime::GetInstanceProperty(FEntity Entity, const std::string& Name)
{
	const auto Found = Instances.find(Entity.ToId());
	if (Found == Instances.end() || !Found->second.Self.valid())
	{
		return FScriptValue{};
	}
	const sol::object Properties = Found->second.Self["Properties"];
	if (Properties.get_type() != sol::type::table)
	{
		return FScriptValue{};
	}
	return ToScriptValue(Properties.as<sol::table>()[Name]);
}

// ---------------------------------------------------------------- 프리팹 (에셋 값, Scene.SpawnPrefab)

void FLuaRuntime::RegisterPrefabBindings()
{
	// 에셋 참조 값: Properties 기본값으로 선언하면 인스펙터가 콘텐츠 브라우저 드롭 칸을 보여 준다
	Lua.new_usertype<FScriptAssetRef>(
		"AssetRef",
		sol::no_constructor,
		"Path", sol::readonly_property([](const FScriptAssetRef& Ref) { return Ref.Path; }),
		"Filter", sol::readonly_property([](const FScriptAssetRef& Ref) { return Ref.Filter; }),
		"IsEmpty", [](const FScriptAssetRef& Ref) { return Ref.Path.empty(); },
		sol::meta_function::to_string, [](const FScriptAssetRef& Ref) { return std::format("Asset({})", Ref.Path); },
		sol::meta_function::equal_to, [](const FScriptAssetRef& A, const FScriptAssetRef& B) { return A.Path == B.Path; });
	Lua["Asset"]  = [](sol::optional<std::string> Path, sol::optional<std::string> Filter) {
		return FScriptAssetRef{ Path.value_or(std::string()), Filter.value_or(std::string()) };
	};
	Lua["Prefab"] = [](sol::optional<std::string> Path) { return FScriptAssetRef{ Path.value_or(std::string()), ".eprefab" }; };

	// Scene.SpawnPrefab(prefab, position?, onSpawned?) — 프리팹 경로 문자열 또는 Prefab 값.
	// 생성은 지연된다: 이번 프레임 스크립트 갱신(OnStart/OnUpdate 순회)이 끝난 뒤 만들어지고, onSpawned(root)가 그때 호출된다.
	// 만든 엔티티의 스크립트는 다음 프레임부터 OnStart/OnUpdate가 돈다. 반환값 없음 (엔티티는 onSpawned에서 받는다)
	sol::table SceneTable     = Lua["Scene"];
	SceneTable["SpawnPrefab"] = [this](const sol::object& Prefab, sol::optional<FVector3> Position, sol::optional<sol::function> OnSpawned) {
		if (Scene == nullptr)
		{
			throw std::runtime_error("씬이 없습니다 (플레이 중에만 프리팹을 만들 수 있습니다)");
		}
		FPendingSpawn Spawn;
		if (Prefab.is<FScriptAssetRef>())
		{
			Spawn.Asset = Prefab.as<const FScriptAssetRef&>().Path;
		}
		else if (Prefab.get_type() == sol::type::string)
		{
			Spawn.Asset = Prefab.as<std::string>();
		}
		if (Spawn.Asset.empty())
		{
			throw std::runtime_error("Scene.SpawnPrefab: 프리팹 경로(문자열 또는 Prefab 값)가 필요합니다");
		}
		Spawn.bHasPosition = Position.has_value();
		Spawn.Position     = Position.value_or(FVector3::ZeroVector);
		if (OnSpawned)
		{
			Spawn.OnSpawned = sol::protected_function(*OnSpawned, Traceback);
			Spawn.Owner     = CurrentInstance; // 콜백도 요청한 인스턴스 코드로 (Timer/Coroutine 소유자)
		}
		PendingSpawns.push_back(std::move(Spawn));
	};
}

void FLuaRuntime::ApplyPendingSpawns()
{
	if (PendingSpawns.empty() || Scene == nullptr)
	{
		return;
	}
	// 콜백이 또 생성하면 다음 적용(다음 프레임)으로 넘어간다
	std::vector<FPendingSpawn> Batch;
	Batch.swap(PendingSpawns);
	for (FPendingSpawn& Spawn : Batch)
	{
		// 상대 경로는 이 런타임의 Content 기준 (라이브러리 기준 폴더와 달라도 같은 파일을 가리키도록 절대 경로로 넘긴다)
		const std::filesystem::path RelativeOrAbsolute = FStringConv::ToWide(Spawn.Asset);
		const std::filesystem::path File               = RelativeOrAbsolute.is_absolute() ? RelativeOrAbsolute : ContentDirectory / RelativeOrAbsolute;
		std::string                 Error;
		const FEntity               Root = FPrefabLibrary::Get().Instantiate(*Scene, FStringConv::ToUtf8(File.wstring()), NullEntity, &Error);
		if (!Root.IsValid())
		{
			ReportError(std::format("Scene.SpawnPrefab 실패: {} — {}", Spawn.Asset, Error));
			continue;
		}
		if (Spawn.bHasPosition)
		{
			Scene->GetTransform(Root).Position = Spawn.Position;
		}
		bStructureChanged = true; // 앱이 메시/머티리얼/모델 참조를 해석한다
		if (Spawn.OnSpawned.valid())
		{
			const FInstanceScope           Scope(*this, Spawn.Owner);
			sol::protected_function_result Result = Spawn.OnSpawned(FScriptEntity{ Root });
			if (!Result.valid())
			{
				const sol::error CallbackError = Result;
				ReportError(std::format("Scene.SpawnPrefab 콜백 오류 ({})\n{}", Spawn.Asset, CallbackError.what()));
			}
		}
	}
}

// ---------------------------------------------------------------- 네트워크 (Net 테이블)

void FLuaRuntime::RegisterNetBindings()
{
	// Net.IsServer(): 게임 로직 권한 (서버/Standalone), Net.IsClient(): 화면/로컬 플레이어가 있음 (클라이언트/리슨 호스트/Standalone)
	sol::table NetTable            = Lua.create_named_table("Net");
	NetTable["IsServer"]           = [this]() { return NetHooks == nullptr || NetHooks->bIsServer; };
	NetTable["IsClient"]           = [this]() { return NetHooks == nullptr || NetHooks->bIsClient; };
	NetTable["GetMode"]            = [this]() { return NetHooks != nullptr ? NetHooks->ModeName : std::string("Standalone"); };
	NetTable["GetLocalPlayerId"]   = [this]() { return GetLocalPlayerId(); };
	// 로컬 플레이어의 시점 방향 (yaw, pitch 도) — 매 틱 입력과 함께 서버로 간다 (서버 스크립트는 entity:GetControlRotation)
	NetTable["SetControlRotation"] = [this](float Yaw, float Pitch) {
		LocalControlRotation = FVector2(Yaw, Pitch);
		if (NetHooks != nullptr && NetHooks->SetLocalControlRotation)
		{
			NetHooks->SetLocalControlRotation(LocalControlRotation);
		}
	};

	// ---- 세션 (로비). Host/Connect/Disconnect는 요청만 하고 앱이 이번 프레임 끝에 전환한다 (Connect/Disconnect는 씬을 다시 연다)
	NetTable["FindSessions"] = [this]() {
		if (NetHooks != nullptr && NetHooks->FindSessions)
		{
			NetHooks->FindSessions();
		}
	};
	NetTable["GetSessions"] = [this]() {
		sol::table Result = Lua.create_table();
		if (NetHooks != nullptr && NetHooks->GetSessions)
		{
			int32 Index = 1;
			for (const FScriptLanSession& Session : NetHooks->GetSessions())
			{
				sol::table Entry     = Lua.create_table();
				Entry["name"]        = Session.Name;
				Entry["scene"]       = Session.SceneAsset;
				Entry["address"]     = Session.Address;
				Entry["players"]     = Session.Players;
				Entry["maxPlayers"]  = Session.MaxPlayers;
				Result[Index++]      = Entry;
			}
		}
		return Result;
	};
	NetTable["Host"] = [this](sol::optional<int32> Port) {
		if (NetHooks == nullptr || !NetHooks->Host)
		{
			throw std::runtime_error("Net.Host: 이 앱은 세션 전환을 지원하지 않습니다");
		}
		NetHooks->Host(Port.value_or(0));
	};
	NetTable["Connect"] = [this](const std::string& Address) {
		if (NetHooks == nullptr || !NetHooks->Connect)
		{
			throw std::runtime_error("Net.Connect: 이 앱은 세션 전환을 지원하지 않습니다");
		}
		NetHooks->Connect(Address);
	};
	NetTable["Disconnect"] = [this]() {
		if (NetHooks != nullptr && NetHooks->Disconnect)
		{
			NetHooks->Disconnect();
		}
	};
	NetTable["GetState"] = [this]() {
		return NetHooks != nullptr && NetHooks->GetState ? NetHooks->GetState() : std::string("Standalone");
	};
	NetTable["GetFailureReason"] = [this]() {
		return NetHooks != nullptr && NetHooks->GetFailureReason ? NetHooks->GetFailureReason() : std::string();
	};
}

bool FLuaRuntime::ShouldRunHere(const FScriptComponent& Component) const
{
	if (NetHooks == nullptr)
	{
		return true;
	}
	switch (static_cast<EScriptExecution>(Component.ExecutionLocation))
	{
	case EScriptExecution::ClientOnly: return NetHooks->bRunClientScripts;
	case EScriptExecution::Both:       return NetHooks->bRunServerScripts || NetHooks->bRunClientScripts;
	default:                           return NetHooks->bRunServerScripts;
	}
}

int32 FLuaRuntime::GetLocalPlayerId() const
{
	return NetHooks != nullptr && NetHooks->GetLocalPlayerId ? NetHooks->GetLocalPlayerId() : 0;
}

int32 FLuaRuntime::GetOwner(FEntity Entity) const
{
	return NetHooks != nullptr && NetHooks->GetOwner ? NetHooks->GetOwner(Entity) : -1;
}

FGameRpcValue FLuaRuntime::ToRpcValue(const sol::object& Object)
{
	if (Object.is<FScriptEntity>())
	{
		return FGameRpcValue::MakeEntity(Object.as<FScriptEntity>().Entity);
	}
	const FScriptValue Value = ToScriptValue(Object);
	switch (Value.Type)
	{
	case EScriptValueType::Bool:    return FGameRpcValue::MakeBool(Value.bBool);
	case EScriptValueType::Number:  return FGameRpcValue::MakeNumber(Value.Number, Value.bInteger);
	case EScriptValueType::String:  return FGameRpcValue::MakeString(Value.String);
	case EScriptValueType::Vector3: return FGameRpcValue::MakeVector3(Value.Vector);
	case EScriptValueType::Asset:   return FGameRpcValue::MakeAsset(Value.String, Value.AssetFilter);
	default:
		if (Object.get_type() != sol::type::lua_nil && Object.get_type() != sol::type::none)
		{
			throw std::runtime_error("RPC 인자는 nil/bool/숫자/문자열/Vector3/에셋/엔티티만 보낼 수 있습니다");
		}
		return FGameRpcValue{};
	}
}

sol::object FLuaRuntime::FromRpcValue(const FGameRpcValue& Value)
{
	switch (Value.Type)
	{
	case FGameRpcValue::EType::Bool:    return sol::make_object(Lua, Value.bBool);
	case FGameRpcValue::EType::Number:
		return Value.bInteger ? sol::make_object(Lua, static_cast<lua_Integer>(Value.Number)) : sol::make_object(Lua, Value.Number);
	case FGameRpcValue::EType::String:  return sol::make_object(Lua, Value.String);
	case FGameRpcValue::EType::Vector3: return sol::make_object(Lua, Value.Vector);
	case FGameRpcValue::EType::Asset:   return sol::make_object(Lua, FScriptAssetRef{ Value.String, Value.AssetFilter });
	case FGameRpcValue::EType::Entity:  return sol::make_object(Lua, FScriptEntity{ Value.Entity });
	default:                            return sol::lua_nil;
	}
}

void FLuaRuntime::CallRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const sol::variadic_args& Args)
{
	FGameRpcArgs Converted;
	Converted.reserve(Args.size());
	for (const sol::object Arg : Args)
	{
		Converted.push_back(ToRpcValue(Arg));
	}
	if (NetHooks != nullptr && NetHooks->SendRpc)
	{
		NetHooks->SendRpc(Target, Kind, Name, Converted);
		return;
	}
	InvokeMethod(Target, GetRpcMethodPrefix(Kind) + Name, Converted); // 네트워크 없음(Standalone): 바로 로컬 호출
}

void FLuaRuntime::BroadcastMethod(const std::string& MethodName, const FGameRpcArgs& Args)
{
	// 호출 중 인스턴스가 생기거나 없어질 수 있으므로 대상 목록을 먼저 복사한다
	std::vector<FEntity> Targets;
	Targets.reserve(Instances.size());
	for (const auto& [Id, Instance] : Instances)
	{
		Targets.push_back(Instance.Entity);
	}
	for (const FEntity Target : Targets)
	{
		InvokeMethod(Target, MethodName, Args);
	}
}

bool FLuaRuntime::InvokeMethod(FEntity Target, const std::string& MethodName, const FGameRpcArgs& Args)
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
	Values.reserve(Args.size());
	for (const FGameRpcValue& Arg : Args)
	{
		Values.push_back(FromRpcValue(Arg));
	}
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
