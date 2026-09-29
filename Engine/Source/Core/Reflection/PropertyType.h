#pragma once

#include "Core/Containers/Handle.h"
#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <string>
#include <typeinfo>

// 리플렉션이 이해하는 프로퍼티 값 타입.
// 인스펙터 위젯 선택, JSON 직렬화, 스크립트 바인딩이 모두 이 열거형으로 분기한다.
enum class EPropertyType : uint8
{
	Bool,
	Int32,
	UInt32,
	Float,
	String,         // std::string (UTF-8)
	Vector2,
	Vector3,
	Vector4,
	Quat,
	Entity,         // FEntity
	ResourceHandle, // THandle<Tag> (메시/텍스처/머티리얼 등)
	Count
};

const char* PropertyTypeToString(EPropertyType Type);

// 프로퍼티 표시/직렬화 힌트
enum EPropertyFlags : uint32
{
	PF_None      = 0,
	PF_Hidden    = 1 << 0, // 인스펙터에 표시하지 않음
	PF_ReadOnly  = 1 << 1, // 표시만 하고 편집 불가
	PF_Color     = 1 << 2, // Vector3/Vector4를 색으로 편집
	PF_Transient = 1 << 3, // 직렬화 제외 (캐시 등)
};

// C++ 타입 → EPropertyType 매핑. 지원하지 않는 타입은 컴파일 오류가 난다.
template <typename T>
struct TPropertyTypeOf;

#define E_DEFINE_PROPERTY_TYPE(CppType, EnumValue)                                       \
	template <>                                                                          \
	struct TPropertyTypeOf<CppType>                                                      \
	{                                                                                    \
		static constexpr EPropertyType Value = EnumValue;                                \
	};

E_DEFINE_PROPERTY_TYPE(bool, EPropertyType::Bool)
E_DEFINE_PROPERTY_TYPE(int32, EPropertyType::Int32)
E_DEFINE_PROPERTY_TYPE(uint32, EPropertyType::UInt32)
E_DEFINE_PROPERTY_TYPE(float, EPropertyType::Float)
E_DEFINE_PROPERTY_TYPE(std::string, EPropertyType::String)
E_DEFINE_PROPERTY_TYPE(FVector2, EPropertyType::Vector2)
E_DEFINE_PROPERTY_TYPE(FVector3, EPropertyType::Vector3)
E_DEFINE_PROPERTY_TYPE(FVector4, EPropertyType::Vector4)
E_DEFINE_PROPERTY_TYPE(FQuat, EPropertyType::Quat)
E_DEFINE_PROPERTY_TYPE(FEntity, EPropertyType::Entity)

#undef E_DEFINE_PROPERTY_TYPE

template <typename TTag>
struct TPropertyTypeOf<THandle<TTag>>
{
	static constexpr EPropertyType Value = EPropertyType::ResourceHandle;
};

// 리소스 핸들의 태그 이름 (표시/디버그용). MSVC의 typeid 이름에서 "struct " 접두사를 제거한다.
template <typename TTag>
inline std::string GetHandleTypeName()
{
	std::string Name = typeid(TTag).name();
	if (Name.rfind("struct ", 0) == 0)
	{
		Name.erase(0, 7);
	}
	return Name;
}
