#include "Scripting/LuaRuntime.h"

#include "Core/Input.h"
#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Scene/Scene.h"

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
	// EKey 순서와 1:1 (Lua: Input.IsKeyDown("W"), "Space", "LeftShift", "F1", "0" ...)
	constexpr std::array<const char*, static_cast<size_t>(EKey::Count)> GKeyNames = {
		"None",
		"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
		"N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
		"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
		"Escape", "Tab", "CapsLock", "Space", "Enter", "Backspace",
		"LeftShift", "RightShift", "LeftControl", "RightControl", "LeftAlt", "RightAlt",
		"Insert", "Delete", "Home", "End", "PageUp", "PageDown",
		"Left", "Right", "Up", "Down",
		"Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9",
		"NumpadAdd", "NumpadSubtract", "NumpadMultiply", "NumpadDivide", "NumpadDecimal", "NumpadEnter",
		"Minus", "Equals", "LeftBracket", "RightBracket", "Backslash",
		"Semicolon", "Apostrophe", "Comma", "Period", "Slash", "Grave",
	};

	EKey ParseKey(std::string_view Name)
	{
		static const std::unordered_map<std::string_view, EKey> Map = [] {
			std::unordered_map<std::string_view, EKey> Result;
			for (size_t Index = 1; Index < GKeyNames.size(); ++Index)
			{
				Result.emplace(GKeyNames[Index], static_cast<EKey>(Index));
			}
			return Result;
		}();
		const auto Found = Map.find(Name);
		if (Found == Map.end())
		{
			throw std::runtime_error(std::format("알 수 없는 키 이름: '{}'", Name));
		}
		return Found->second;
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

	std::string ReadTextFile(const std::filesystem::path& Path, bool& bOutOk)
	{
		std::ifstream File(Path, std::ios::binary);
		bOutOk = static_cast<bool>(File);
		if (!bOutOk)
		{
			return std::string();
		}
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		std::string Text = Buffer.str();
		// UTF-8 BOM 제거 (Lua 파서는 BOM을 모른다)
		if (Text.size() >= 3 && static_cast<unsigned char>(Text[0]) == 0xEF && static_cast<unsigned char>(Text[1]) == 0xBB &&
		    static_cast<unsigned char>(Text[2]) == 0xBF)
		{
			Text.erase(0, 3);
		}
		return Text;
	}
} // namespace

FLuaRuntime::FLuaRuntime(std::filesystem::path InContentDirectory, uint32& InErrorCounter)
	: ContentDirectory(std::move(InContentDirectory))
	, ErrorCounter(InErrorCounter)
{
	// io/os/package는 열지 않는다 (콘텐츠 스크립트에 파일 시스템/프로세스 접근을 주지 않음)
	Lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::coroutine, sol::lib::utf8,
	                   sol::lib::debug);
	Lua.set_exception_handler(&HandleBindingException);
	Traceback = Lua["debug"]["traceback"];
	Lua["dofile"]   = sol::lua_nil;
	Lua["loadfile"] = sol::lua_nil;
	RegisterBindings();
}

FLuaRuntime::~FLuaRuntime()
{
	// sol 참조(테이블/함수)는 상태보다 먼저 해제되어야 한다
	Instances.clear();
	Classes.clear();
	Traceback = sol::lua_nil;
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
		if (Property.HasFlag(PF_ReadOnly) || Property.Type == EPropertyType::ResourceHandle)
		{
			throw std::runtime_error(std::format("프로퍼티 '{}'는 읽기 전용입니다", Property.Name));
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

	Lua.new_usertype<FScriptEntity>(
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

	// ---- Time (Update마다 갱신)
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
	const std::string Source = ReadTextFile(AbsolutePath, bRead);
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

	sol::protected_function        Function(Chunk.get<sol::function>(), Traceback);
	sol::protected_function_result Result = Function();
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
	bOutSucceeded    = false;
	const std::string Key   = MakeClassKey(ScriptPath);
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

	sol::table Self       = Lua.create_table();
	sol::table Properties = Lua.create_table();
	const sol::object Defaults = Class.Class["Properties"];
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
		Properties[Name] = FromScriptValue(Typed);
	}

	Self["entity"]             = FScriptEntity{ Entity };
	Self["Properties"]         = Properties;
	Self[sol::metatable_key]   = Class.Metatable;
	Instance.Self              = Self;
	Instances.emplace(Entity.ToId(), std::move(Instance));
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
	sol::protected_function_result Result = bPassDelta ? Function(Self, DeltaSeconds) : Function(Self);
	if (!Result.valid())
	{
		const sol::error Error = Result;
		Instance.bFaulted      = true;
		ReportError(std::format("스크립트 오류 ({}:{}) — 이 인스턴스는 멈춥니다 (스크립트 저장 시 재개)\n{}", Instance.ScriptAsset, MethodName, Error.what()));
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
			if (!Component.ScriptAsset.empty())
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

	// 4. 스크립트가 요청한 파괴
	ApplyPendingDestroys();
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
	PendingDestroy.clear();
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
