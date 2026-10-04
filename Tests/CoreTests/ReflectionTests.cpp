#include "Core/Reflection/TypeInfo.h"
#include "Core/Testing/TestFramework.h"

#include <cstddef>

namespace
{
	struct FTestTag {};
	using FTestHandle = THandle<FTestTag>;

	struct FReflectedThing
	{
		bool        bEnabled  = true;
		int32       Count     = 3;
		uint32      Mask      = 7;
		float       Weight    = 1.5f;
		std::string Label     = "thing";
		FVector3    Direction = FVector3::UpVector;
		FVector4    Tint      = FVector4::OneVector;
		FQuat       Rotation;
		FEntity     Target;
		FTestHandle Resource;
		float       Hidden = 0.0f;
	};

	struct FReflectedComponent
	{
		float Value = 42.0f;
	};

	// 테스트 프로세스에서 한 번만 등록
	void EnsureRegistered()
	{
		static bool bDone = false;
		if (bDone)
		{
			return;
		}
		bDone = true;

		FTypeRegistry::Get().RegisterType<FReflectedThing>("ReflectedThing", "테스트 타입")
			.Property(&FReflectedThing::bEnabled, "Enabled", "활성")
			.Property(&FReflectedThing::Count, "Count", "개수").Range(0.0f, 10.0f)
			.Property(&FReflectedThing::Mask, "Mask", "마스크")
			.Property(&FReflectedThing::Weight, "Weight", "무게").Range(0.0f, 100.0f, 0.5f)
			.Property(&FReflectedThing::Label, "Label", "라벨")
			.Property(&FReflectedThing::Direction, "Direction", "방향")
			.Property(&FReflectedThing::Tint, "Tint", "틴트", PF_Color)
			.Property(&FReflectedThing::Rotation, "Rotation", "회전")
			.Property(&FReflectedThing::Target, "Target", "대상")
			.Property(&FReflectedThing::Resource, "Resource", "리소스", PF_ReadOnly)
			.Property(&FReflectedThing::Hidden, "Hidden", "숨김", PF_Hidden | PF_Transient);

		FTypeRegistry::Get().RegisterType<FReflectedComponent>("ReflectedComponent", "테스트 컴포넌트")
			.Property(&FReflectedComponent::Value, "Value", "값")
			.AsComponent();
	}
} // namespace

E_TEST(Reflection_TypeAndPropertyMetadata)
{
	EnsureRegistered();
	const FTypeRegistry& Registry = FTypeRegistry::Get();

	const FTypeInfo* Type = Registry.Find<FReflectedThing>();
	E_EXPECT_TRUE(Type != nullptr);
	if (Type == nullptr)
	{
		return;
	}
	E_EXPECT_TRUE(Registry.Find("ReflectedThing") == Type);
	E_EXPECT_TRUE(Registry.IsRegistered<FReflectedThing>());
	E_EXPECT_TRUE(Type->DisplayName == "테스트 타입");
	E_EXPECT_EQ(Type->Size, sizeof(FReflectedThing));
	E_EXPECT_EQ(Type->Properties.size(), static_cast<size_t>(11));
	E_EXPECT_FALSE(Type->bIsComponent);

	// 타입/오프셋/크기
	const FPropertyInfo* Weight = Type->FindProperty("Weight");
	E_EXPECT_TRUE(Weight != nullptr && Weight->Type == EPropertyType::Float);
	E_EXPECT_EQ(Weight->Offset, offsetof(FReflectedThing, Weight));
	E_EXPECT_EQ(Weight->Size, sizeof(float));
	E_EXPECT_TRUE(Weight->HasRange());
	E_EXPECT_NEAR(Weight->MaxValue, 100.0f, 0.0f);
	E_EXPECT_NEAR(Weight->Step, 0.5f, 0.0f);

	E_EXPECT_TRUE(Type->FindProperty("Label")->Type == EPropertyType::String);
	E_EXPECT_EQ(Type->FindProperty("Label")->Offset, offsetof(FReflectedThing, Label));
	E_EXPECT_TRUE(Type->FindProperty("Direction")->Type == EPropertyType::Vector3);
	E_EXPECT_TRUE(Type->FindProperty("Rotation")->Type == EPropertyType::Quat);
	E_EXPECT_TRUE(Type->FindProperty("Target")->Type == EPropertyType::Entity);
	E_EXPECT_TRUE(Type->FindProperty("Mask")->Type == EPropertyType::UInt32);
	E_EXPECT_FALSE(Type->FindProperty("Mask")->HasRange());

	// 플래그
	E_EXPECT_TRUE(Type->FindProperty("Tint")->HasFlag(PF_Color));
	E_EXPECT_TRUE(Type->FindProperty("Resource")->HasFlag(PF_ReadOnly));
	E_EXPECT_TRUE(Type->FindProperty("Hidden")->HasFlag(PF_Hidden));
	E_EXPECT_TRUE(Type->FindProperty("Hidden")->HasFlag(PF_Transient));
	E_EXPECT_FALSE(Type->FindProperty("Weight")->HasFlag(PF_Hidden));

	// 리소스 핸들 태그 이름
	const FPropertyInfo* Resource = Type->FindProperty("Resource");
	E_EXPECT_TRUE(Resource->Type == EPropertyType::ResourceHandle);
	E_EXPECT_TRUE(Resource->HandleTypeName.find("FTestTag") != std::string::npos);

	E_EXPECT_TRUE(Type->FindProperty("NoSuchProperty") == nullptr);
}

E_TEST(Reflection_GetSetThroughProperty)
{
	EnsureRegistered();
	const FTypeInfo* Type = FTypeRegistry::Get().Find<FReflectedThing>();
	if (Type == nullptr)
	{
		return;
	}

	FReflectedThing Thing;
	const FPropertyInfo& Weight = *Type->FindProperty("Weight");
	const FPropertyInfo& Label  = *Type->FindProperty("Label");
	const FPropertyInfo& Dir    = *Type->FindProperty("Direction");

	E_EXPECT_NEAR(Weight.GetRef<float>(&Thing), 1.5f, 0.0f);
	Weight.GetRef<float>(&Thing) = 9.0f;
	E_EXPECT_NEAR(Thing.Weight, 9.0f, 0.0f);

	Label.GetRef<std::string>(&Thing) = "바뀜";
	E_EXPECT_TRUE(Thing.Label == "바뀜");

	Dir.GetRef<FVector3>(&Thing) = FVector3::ForwardVector;
	E_EXPECT_EQUALS(Thing.Direction, FVector3::ForwardVector, 0.0f);

	// const 접근
	const FReflectedThing& ConstThing = Thing;
	E_EXPECT_NEAR(Weight.GetRef<float>(&ConstThing), 9.0f, 0.0f);
}

E_TEST(Reflection_ComponentHooks)
{
	EnsureRegistered();
	const FTypeInfo* Type = FTypeRegistry::Get().Find<FReflectedComponent>();
	E_EXPECT_TRUE(Type != nullptr && Type->bIsComponent && Type->bRemovable);
	if (Type == nullptr)
	{
		return;
	}

	FRegistry     Registry;
	const FEntity Entity = Registry.Create();

	E_EXPECT_FALSE(Type->HasComponent(Registry, Entity));
	E_EXPECT_TRUE(Type->GetComponent(Registry, Entity) == nullptr);

	void* Added = Type->AddComponent(Registry, Entity);
	E_EXPECT_TRUE(Added != nullptr);
	E_EXPECT_TRUE(Type->HasComponent(Registry, Entity));
	E_EXPECT_TRUE(Type->GetComponent(Registry, Entity) == Registry.TryGet<FReflectedComponent>(Entity));

	// 프로퍼티를 통해 실제 컴포넌트 값 변경
	Type->FindProperty("Value")->GetRef<float>(Added) = 7.0f;
	E_EXPECT_NEAR(Registry.Get<FReflectedComponent>(Entity).Value, 7.0f, 0.0f);

	// 중복 추가는 기존 컴포넌트 반환
	E_EXPECT_TRUE(Type->AddComponent(Registry, Entity) == Added);

	Type->RemoveComponent(Registry, Entity);
	E_EXPECT_FALSE(Registry.Has<FReflectedComponent>(Entity));

	// 컴포넌트 타입 순회에 포함되고, 비컴포넌트 타입은 제외
	bool bFoundComponent = false;
	bool bFoundThing     = false;
	FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Info) {
		bFoundComponent |= (&Info == Type);
		bFoundThing |= (Info.Name == "ReflectedThing");
	});
	E_EXPECT_TRUE(bFoundComponent);
	E_EXPECT_FALSE(bFoundThing);
}

// 문자열 선택지 공급자: StringOptions(인자 없음)와 StringOptionsFor(그 오브젝트를 받음 — 같은 오브젝트의 다른 값에 따라 목록이 바뀜)
namespace
{
	struct FOptionsThing
	{
		std::string Kind = "Fruit";
		std::string Choice;
		std::string Fixed;
		std::string Plain;
	};
} // namespace

E_TEST(Reflection_StringOptionsProviders)
{
	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		FTypeRegistry::Get().RegisterType<FOptionsThing>("OptionsThing", "선택지 테스트")
			.Property(&FOptionsThing::Kind, "Kind", "종류")
			.Property(&FOptionsThing::Choice, "Choice", "선택")
			.StringOptionsFor([](const FOptionsThing& Thing) {
				return Thing.Kind == "Fruit" ? std::vector<std::string>{ "Apple", "Pear" } : std::vector<std::string>{ "Carrot" };
			})
			.Property(&FOptionsThing::Fixed, "Fixed", "고정")
			.StringOptions([]() { return std::vector<std::string>{ "A", "B", "C" }; })
			.Property(&FOptionsThing::Plain, "Plain", "일반");
	}
	const FTypeInfo* Type = FTypeRegistry::Get().Find<FOptionsThing>();
	E_EXPECT_TRUE(Type != nullptr);
	if (Type == nullptr)
	{
		return;
	}
	FOptionsThing        Thing;
	const FPropertyInfo* Choice = Type->FindProperty("Choice");
	E_EXPECT_TRUE(Choice->HasStringOptions() && Choice->StringOptionsFor != nullptr && Choice->StringOptions == nullptr);
	E_EXPECT_TRUE(Choice->GetStringOptions(&Thing) == (std::vector<std::string>{ "Apple", "Pear" }));
	Thing.Kind = "Vegetable";
	E_EXPECT_TRUE(Choice->GetStringOptions(&Thing) == (std::vector<std::string>{ "Carrot" }));
	E_EXPECT_TRUE(Choice->GetStringOptions(nullptr).empty());

	const FPropertyInfo* Fixed = Type->FindProperty("Fixed");
	E_EXPECT_TRUE(Fixed->HasStringOptions() && Fixed->StringOptionsFor == nullptr);
	E_EXPECT_EQ(Fixed->GetStringOptions(&Thing).size(), static_cast<size_t>(3));
	E_EXPECT_FALSE(Type->FindProperty("Plain")->HasStringOptions());
	E_EXPECT_TRUE(Type->FindProperty("Plain")->GetStringOptions(&Thing).empty());
}
