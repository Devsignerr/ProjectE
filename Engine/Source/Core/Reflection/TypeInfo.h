#pragma once

#include "Core/Assert.h"
#include "Core/ECS/Registry.h"
#include "Core/Reflection/PropertyType.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

// 프로퍼티 메타데이터: 오브젝트 시작 주소 + Offset 위치에 Type 값이 있다
struct FPropertyInfo
{
	std::string   Name;        // 식별자 (직렬화 키, 스크립트 이름)
	std::string   DisplayName; // 인스펙터 표시 이름
	EPropertyType Type   = EPropertyType::Bool;
	uint32        Flags  = PF_None;
	size_t        Offset = 0;
	size_t        Size   = 0;

	// 숫자 타입의 편집 범위/증분 (Min == Max면 제한 없음, Step 0이면 기본값)
	float MinValue = 0.0f;
	float MaxValue = 0.0f;
	float Step     = 0.0f;

	std::string HandleTypeName; // ResourceHandle일 때 태그 이름
	std::string AssetFilter;    // 에셋 경로 문자열이면 허용 확장자 (";" 구분, 예 ".emat") — 에디터 드래그 앤 드롭 대상

	bool HasFlag(EPropertyFlags Flag) const { return (Flags & Flag) != 0; }
	bool HasRange() const { return MinValue < MaxValue; }

	void*       GetPtr(void* Object) const { return static_cast<uint8*>(Object) + Offset; }
	const void* GetPtr(const void* Object) const { return static_cast<const uint8*>(Object) + Offset; }

	template <typename T>
	T& GetRef(void* Object) const
	{
		E_CHECKF(TPropertyTypeOf<T>::Value == Type, "프로퍼티 '{}' 타입 불일치", Name);
		return *static_cast<T*>(GetPtr(Object));
	}

	template <typename T>
	const T& GetRef(const void* Object) const
	{
		E_CHECKF(TPropertyTypeOf<T>::Value == Type, "프로퍼티 '{}' 타입 불일치", Name);
		return *static_cast<const T*>(GetPtr(Object));
	}
};

enum ETypeFlags : uint32
{
	TF_None              = 0,
	TF_HiddenInInspector = 1 << 0, // 인스펙터 컴포넌트 목록에 표시하지 않음
	TF_NoReplicate       = 1 << 1, // 네트워크 복제 제외 (씬 구조/편집 전용 컴포넌트)
};

// 등록된 타입 하나의 메타데이터. ECS 컴포넌트면 레지스트리 조작 훅을 갖는다.
struct FTypeInfo
{
	std::string Name;
	std::string DisplayName;
	uint32      Flags  = TF_None;
	uint32      TypeId = 0; // 레지스트리 내 순번
	std::string Owner;      // 등록한 모듈 ("Engine" 또는 게임 모듈 이름) — 모듈 언로드 시 함께 제거
	size_t      Size   = 0;
	size_t      Alignment = 0;

	std::vector<FPropertyInfo> Properties;

	// ---- ECS 컴포넌트 훅 (bIsComponent일 때만 유효)
	bool bIsComponent = false;
	bool bRemovable   = true; // 인스펙터에서 제거 허용

	std::function<bool(const FRegistry&, FEntity)> HasComponent;
	std::function<void*(FRegistry&, FEntity)>      GetComponent; // 없으면 nullptr
	std::function<void*(FRegistry&, FEntity)>      AddComponent; // 기본 생성 후 포인터
	std::function<void(FRegistry&, FEntity)>       RemoveComponent;
	std::function<void(FRegistry&, FEntity, const FRegistry&, FEntity)> CopyComponent; // (대상, 대상 엔티티, 원본, 원본 엔티티) 값 복사

	bool HasFlag(ETypeFlags Flag) const { return (Flags & Flag) != 0; }

	const FPropertyInfo* FindProperty(std::string_view PropertyName) const
	{
		for (const FPropertyInfo& Property : Properties)
		{
			if (Property.Name == PropertyName)
			{
				return &Property;
			}
		}
		return nullptr;
	}
};

// 멤버 포인터 → 바이트 오프셋 (생성자 호출 없이 계산)
template <typename T, typename TMember>
size_t GetMemberOffset(TMember T::*Member)
{
	alignas(T) static unsigned char Storage[sizeof(T)];
	const T* Object = reinterpret_cast<const T*>(Storage);
	return static_cast<size_t>(reinterpret_cast<const unsigned char*>(&(Object->*Member)) - Storage);
}

template <typename T>
class TTypeBuilder;

// 프로세스 전역 타입 레지스트리
class FTypeRegistry
{
public:
	static FTypeRegistry& Get();

	// 새 타입 등록. 이미 등록된 타입이면 Fatal (등록 함수는 한 번만 호출할 것)
	template <typename T>
	TTypeBuilder<T> RegisterType(std::string Name, std::string DisplayName);

	template <typename T>
	bool IsRegistered() const
	{
		return ByType.contains(std::type_index(typeid(T)));
	}

	template <typename T>
	const FTypeInfo* Find() const
	{
		const auto Found = ByType.find(std::type_index(typeid(T)));
		return Found != ByType.end() ? Found->second : nullptr;
	}

	const FTypeInfo* Find(std::string_view Name) const;

	const std::vector<std::unique_ptr<FTypeInfo>>& GetTypes() const { return Types; }

	// 이후 등록되는 타입의 소유자 (게임 모듈 로드 중에는 모듈 이름, 기본 "Engine")
	void               SetRegistrationOwner(std::string Owner) { CurrentOwner = std::move(Owner); }
	const std::string& GetRegistrationOwner() const { return CurrentOwner; }
	// 소유자가 같은 타입 제거 (게임 모듈 언로드 전: 등록 정보의 함수 포인터가 모듈 코드를 가리키므로). 제거한 수 반환
	size_t RemoveTypesByOwner(std::string_view Owner);

	// 컴포넌트 타입만 등록 순서대로 순회: Func(const FTypeInfo&)
	template <typename TFunc>
	void ForEachComponentType(TFunc&& Func) const
	{
		for (const std::unique_ptr<FTypeInfo>& Type : Types)
		{
			if (Type->bIsComponent)
			{
				Func(*Type);
			}
		}
	}

private:
	FTypeInfo& AddType(std::type_index Index, std::string Name, std::string DisplayName, size_t Size, size_t Alignment);

	std::vector<std::unique_ptr<FTypeInfo>>       Types;
	std::string                                   CurrentOwner = "Engine";
	std::unordered_map<std::string, FTypeInfo*>   ByName;
	std::unordered_map<std::type_index, FTypeInfo*> ByType;
};

// 타입 등록용 유창한 빌더. 사용 예:
//   FTypeRegistry::Get().RegisterType<FLight>("Light", "조명")
//       .Property(&FLight::Color, "Color", "색", PF_Color)
//       .Property(&FLight::Intensity, "Intensity", "강도").Range(0.0f, 50.0f, 0.05f)
//       .AsComponent();
template <typename T>
class TTypeBuilder
{
public:
	explicit TTypeBuilder(FTypeInfo& InInfo)
		: Info(InInfo)
	{
	}

	template <typename TMember>
	TTypeBuilder& Property(TMember T::*Member, std::string Name, std::string DisplayName, uint32 Flags = PF_None)
	{
		E_CHECKF(Info.FindProperty(Name) == nullptr, "타입 '{}'에 프로퍼티 '{}'가 이미 있습니다", Info.Name, Name);

		FPropertyInfo& PropertyInfo = Info.Properties.emplace_back();
		PropertyInfo.Name        = std::move(Name);
		PropertyInfo.DisplayName = std::move(DisplayName);
		PropertyInfo.Type        = TPropertyTypeOf<TMember>::Value;
		PropertyInfo.Flags       = Flags;
		PropertyInfo.Offset      = GetMemberOffset(Member);
		PropertyInfo.Size        = sizeof(TMember);
		if constexpr (TPropertyTypeOf<TMember>::Value == EPropertyType::ResourceHandle)
		{
			PropertyInfo.HandleTypeName = GetHandleTypeName<typename THandleTag<TMember>::Type>();
		}
		return *this;
	}

	// 직전에 추가한 숫자 프로퍼티의 범위/증분
	TTypeBuilder& Range(float MinValue, float MaxValue, float Step = 0.0f)
	{
		E_CHECKF(!Info.Properties.empty(), "Range는 Property 다음에 호출해야 합니다");
		FPropertyInfo& Last = Info.Properties.back();
		Last.MinValue       = MinValue;
		Last.MaxValue       = MaxValue;
		Last.Step           = Step;
		return *this;
	}

	// 직전에 추가한 문자열 프로퍼티가 받는 에셋 확장자 (";" 구분). 인스펙터가 콘텐츠 브라우저 드롭을 받는다
	TTypeBuilder& AssetFilter(const char* Extensions)
	{
		E_CHECKF(!Info.Properties.empty(), "AssetFilter는 Property 다음에 호출해야 합니다");
		Info.Properties.back().AssetFilter = Extensions;
		return *this;
	}

	// ECS 컴포넌트로 표시하고 레지스트리 훅을 연결
	TTypeBuilder& AsComponent(bool bRemovable = true)
	{
		Info.bIsComponent    = true;
		Info.bRemovable      = bRemovable;
		Info.HasComponent    = [](const FRegistry& Registry, FEntity Entity) { return Registry.Has<T>(Entity); };
		Info.GetComponent    = [](FRegistry& Registry, FEntity Entity) -> void* { return Registry.TryGet<T>(Entity); };
		Info.AddComponent    = [](FRegistry& Registry, FEntity Entity) -> void* { return &Registry.GetOrEmplace<T>(Entity); };
		Info.RemoveComponent = [](FRegistry& Registry, FEntity Entity) { Registry.Remove<T>(Entity); };
		Info.CopyComponent   = [](FRegistry& Dest, FEntity DestEntity, const FRegistry& Source, FEntity SourceEntity) {
			Dest.GetOrEmplace<T>(DestEntity) = Source.Get<T>(SourceEntity);
		};
		return *this;
	}

	TTypeBuilder& Hide()
	{
		Info.Flags |= TF_HiddenInInspector;
		return *this;
	}

	TTypeBuilder& NoReplicate()
	{
		Info.Flags |= TF_NoReplicate;
		return *this;
	}

	const FTypeInfo& Get() const { return Info; }

private:
	// THandle<Tag>에서 Tag 추출
	template <typename THandleType>
	struct THandleTag
	{
		using Type = void;
	};
	template <typename TTag>
	struct THandleTag<THandle<TTag>>
	{
		using Type = TTag;
	};

	FTypeInfo& Info;
};

template <typename T>
TTypeBuilder<T> FTypeRegistry::RegisterType(std::string Name, std::string DisplayName)
{
	static_assert(std::is_default_constructible_v<T>, "리플렉션 타입은 기본 생성 가능해야 합니다");
	E_CHECKF(!IsRegistered<T>(), "타입 '{}'가 이미 등록되어 있습니다", Name);
	return TTypeBuilder<T>(AddType(std::type_index(typeid(T)), std::move(Name), std::move(DisplayName), sizeof(T), alignof(T)));
}
