#include "Scene/Prefab.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Scene/EntityJson.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <cwctype>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using nlohmann::json;

	using FMemberMap = std::unordered_map<std::string, FEntity>; // 원본 공간 Id → 대상 엔티티
	using FMakeLink  = std::function<FPrefabLinkComponent(const std::string&)>;

	std::wstring Lower(std::wstring Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Text;
	}

	bool ReadTextFile(const std::filesystem::path& Path, std::string& Out)
	{
		std::ifstream File(Path, std::ios::binary);
		if (!File)
		{
			return false;
		}
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		Out = Buffer.str();
		return true;
	}

	bool WriteTextFile(const std::filesystem::path& Path, const std::string& Text)
	{
		std::error_code ErrorCode;
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		if (!File)
		{
			return false;
		}
		File << Text;
		return static_cast<bool>(File);
	}

	void SetError(std::string* OutError, std::string Message)
	{
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
	}

	const FTypeInfo* GetLinkType() { return FTypeRegistry::Get().Find<FPrefabLinkComponent>(); }
	const FTypeInfo* GetInstanceType() { return FTypeRegistry::Get().Find<FPrefabInstanceComponent>(); }

	// 동기화/차이 계산에서 다루지 않는 타입 (계층은 따로 맞추고, 연결은 인스턴스 소유)
	bool IsIgnoredType(const FTypeInfo& Type)
	{
		const FTypeRegistry& Registry = FTypeRegistry::Get();
		return &Type == Registry.Find<FHierarchyComponent>() || &Type == Registry.Find<FTransientComponent>() || &Type == GetLinkType();
	}

	// 루트의 위치/회전/이름은 인스턴스마다 다르다 (유니티와 같음)
	bool IsRootOwned(const FTypeInfo& Type, const FPropertyInfo& Property)
	{
		const FTypeRegistry& Registry = FTypeRegistry::Get();
		if (&Type == Registry.Find<FTransformComponent>())
		{
			return Property.Name == "Position" || Property.Name == "Rotation";
		}
		return &Type == Registry.Find<FNameComponent>();
	}

	bool IsTopLevelId(const std::string& Id) { return !Id.empty() && Id.find('/') == std::string::npos; }

	// 원본 엔티티 참조 → 대상 엔티티
	using FRemap = std::function<FEntity(FEntity)>;

	bool PropertyEquals(const FPropertyInfo& Property, const void* Target, const void* Source, const FRemap& Remap)
	{
		switch (Property.Type)
		{
		case EPropertyType::String: return Property.GetRef<std::string>(Target) == Property.GetRef<std::string>(Source);
		case EPropertyType::Entity: return Property.GetRef<FEntity>(Target) == Remap(Property.GetRef<FEntity>(Source));
		default:                    return std::memcmp(Property.GetPtr(Target), Property.GetPtr(Source), Property.Size) == 0;
		}
	}

	void CopyPropertyValue(const FPropertyInfo& Property, void* Target, const void* Source, const FRemap& Remap)
	{
		switch (Property.Type)
		{
		case EPropertyType::String: Property.GetRef<std::string>(Target) = Property.GetRef<std::string>(Source); break;
		case EPropertyType::Entity: Property.GetRef<FEntity>(Target) = Remap(Property.GetRef<FEntity>(Source)); break;
		default:                    std::memcpy(Property.GetPtr(Target), Property.GetPtr(Source), Property.Size); break;
		}
	}

	// 경로 문자열이 바뀐 컴포넌트는 리소스 핸들을 비워 다시 해석되게 한다 (FSceneAssetResolver)
	void ResetResourceHandles(const FTypeInfo& Type, void* Component)
	{
		constexpr uint32 InvalidHandle[2] = { ~0u, 0u };
		for (const FPropertyInfo& Property : Type.Properties)
		{
			if (Property.Type == EPropertyType::ResourceHandle && Property.Size == sizeof(InvalidHandle))
			{
				std::memcpy(Property.GetPtr(Component), InvalidHandle, sizeof(InvalidHandle));
			}
		}
	}

	std::string MakePropertyKey(const FTypeInfo& Type, const FPropertyInfo& Property) { return Type.Name + "." + Property.Name; }

	// 원본 노드 중 건너뛸 것: 오버라이드로 삭제됐거나 부모가 건너뛰어짐, Id 중복/없음
	std::vector<bool> ComputeSkipped(const FPrefabTemplate& Template, const FPrefabOverrides& Overrides)
	{
		std::vector<bool> Skipped(Template.Nodes.size(), false);
		for (size_t Index = 0; Index < Template.Nodes.size(); ++Index)
		{
			const FPrefabTemplate::FNode& Node = Template.Nodes[Index];
			const auto                    Found = Template.IndexById.find(Node.Id);
			const bool                    bCanonical = Found != Template.IndexById.end() && Found->second == Index;
			Skipped[Index] = !bCanonical || (Index > 0 && Overrides.RemovedEntities.contains(Node.Id)) ||
			                 (Node.Parent >= 0 && Skipped[static_cast<size_t>(Node.Parent)]);
		}
		return Skipped;
	}

	// 원본 내용으로 대상 엔티티들을 맞춘다 (오버라이드 항목 제외). Members는 생성/삭제에 맞춰 갱신된다
	bool SyncCore(const FPrefabTemplate& Template, FScene& Scene, FMemberMap& Members, const FPrefabOverrides& Overrides, const FMakeLink& MakeLink)
	{
		FRegistry&        Target = Scene.GetRegistry();
		FRegistry&        Source = Template.Scene->GetRegistry();
		const size_t      Count  = Template.Nodes.size();
		std::vector<bool> Skipped = ComputeSkipped(Template, Overrides);
		std::vector<FEntity> Mapped(Count);
		bool                 bChanged = false;

		// 1) 대응 엔티티 찾기 / 원본에 새로 생긴 엔티티 만들기
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (Skipped[Index])
			{
				continue;
			}
			const FPrefabTemplate::FNode& Node  = Template.Nodes[Index];
			const auto                    Found = Members.find(Node.Id);
			if (Found != Members.end() && Target.IsValid(Found->second))
			{
				Mapped[Index] = Found->second;
				continue;
			}
			if (Index == 0)
			{
				return false; // 루트가 없으면 맞출 수 없다
			}
			const FNameComponent* Name    = Source.TryGet<FNameComponent>(Node.Entity);
			const FEntity         Created = Scene.CreateEntity(Name ? Name->Name : std::string("Entity"));
			Scene.SetParent(Created, Mapped[static_cast<size_t>(Node.Parent)]);
			Target.Emplace<FPrefabLinkComponent>(Created, MakeLink(Node.Id));
			Mapped[Index]      = Created;
			Members[Node.Id]   = Created;
			bChanged           = true;
		}

		// 2) 원본에서 사라진 엔티티 삭제 (하위 트리 포함)
		std::vector<std::string> Stale;
		for (const auto& [Id, Entity] : Members)
		{
			const auto Found = Template.IndexById.find(Id);
			if (Found == Template.IndexById.end() || Skipped[Found->second] || Mapped[Found->second] != Entity)
			{
				Stale.push_back(Id);
			}
		}
		for (const std::string& Id : Stale)
		{
			const FEntity Entity = Members[Id];
			Members.erase(Id);
			const bool bStillMapped = std::find(Mapped.begin(), Mapped.end(), Entity) != Mapped.end();
			if (!bStillMapped && Target.IsValid(Entity))
			{
				Scene.DestroyEntity(Entity);
				bChanged = true;
			}
		}

		const FRemap Remap = [&](FEntity SourceEntity) {
			const FPrefabTemplate::FNode* Node = Template.FindByEntity(SourceEntity);
			if (Node == nullptr)
			{
				return NullEntity;
			}
			const size_t Index = static_cast<size_t>(Node - Template.Nodes.data());
			return Skipped[Index] ? NullEntity : Mapped[Index];
		};

		// 3) 계층 + 컴포넌트 값
		const FTypeInfo* InstanceType = GetInstanceType();
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (Skipped[Index] || !Target.IsValid(Mapped[Index]))
			{
				continue;
			}
			const FPrefabTemplate::FNode& Node  = Template.Nodes[Index];
			const FEntity                 Dest  = Mapped[Index];
			const bool                    bRoot = Index == 0;
			if (!bRoot)
			{
				const FEntity ExpectedParent = Mapped[static_cast<size_t>(Node.Parent)];
				if (Scene.GetParent(Dest) != ExpectedParent)
				{
					Scene.SetParent(Dest, ExpectedParent);
					bChanged = true;
				}
			}

			const auto                                Found     = Overrides.Entities.find(Node.Id);
			const FPrefabOverrides::FEntityOverrides* Entity    = Found != Overrides.Entities.end() ? &Found->second : nullptr;
			FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
				if (IsIgnoredType(Type) || (bRoot && &Type == InstanceType))
				{
					return;
				}
				const bool bInSource = Type.HasComponent(Source, Node.Entity);
				const bool bInTarget = Type.HasComponent(Target, Dest);
				if (bInSource)
				{
					if (Entity != nullptr && Entity->RemovedComponents.contains(Type.Name))
					{
						return;
					}
					const bool bFresh    = !bInTarget;
					void*      Component = bFresh ? Type.AddComponent(Target, Dest) : Type.GetComponent(Target, Dest);
					const void* Original = Type.GetComponent(Source, Node.Entity);
					bool        bPathChanged = false;
					for (const FPropertyInfo& Property : Type.Properties)
					{
						if (!FEntityJson::IsSerializable(Property) || (bRoot && IsRootOwned(Type, Property)))
						{
							continue;
						}
						if (!bFresh && Entity != nullptr && Entity->Properties.contains(MakePropertyKey(Type, Property)))
						{
							continue;
						}
						if (!PropertyEquals(Property, Component, Original, Remap))
						{
							CopyPropertyValue(Property, Component, Original, Remap);
							bPathChanged = bPathChanged || Property.Type == EPropertyType::String;
							bChanged     = true;
						}
					}
					bChanged = bChanged || bFresh;
					if (bPathChanged && !bFresh)
					{
						ResetResourceHandles(Type, Component);
					}
				}
				else if (bInTarget)
				{
					if ((Entity != nullptr && Entity->AddedComponents.contains(Type.Name)) || (!Type.bRemovable && &Type != InstanceType))
					{
						return;
					}
					Type.RemoveComponent(Target, Dest);
					bChanged = true;
				}
			});
		}
		return bChanged;
	}

	// 원본과 다른 항목을 Overrides에 더한다 (지우지는 않는다 — 되돌리기로만 지움). 반환: 바뀌었는가
	bool RecordCore(const FPrefabTemplate& Template, FScene& Scene, const FMemberMap& Members, FPrefabOverrides& Overrides)
	{
		const FPrefabOverrides Before = Overrides;
		FRegistry&             Target = Scene.GetRegistry();
		FRegistry&             Source = Template.Scene->GetRegistry();
		const size_t           Count  = Template.Nodes.size();

		// 대응 엔티티 (없어진 원본 엔티티는 삭제로 기록 — 가장 위 것만)
		std::vector<FEntity> Mapped(Count);
		std::vector<bool>    Skipped(Count, false);
		for (size_t Index = 0; Index < Count; ++Index)
		{
			const FPrefabTemplate::FNode& Node       = Template.Nodes[Index];
			const auto                    Canonical  = Template.IndexById.find(Node.Id);
			if (Canonical == Template.IndexById.end() || Canonical->second != Index || (Node.Parent >= 0 && Skipped[static_cast<size_t>(Node.Parent)]))
			{
				Skipped[Index] = true;
				continue;
			}
			const auto Found = Members.find(Node.Id);
			if (Found == Members.end() || !Target.IsValid(Found->second))
			{
				Skipped[Index] = true;
				if (Index > 0)
				{
					Overrides.RemovedEntities.insert(Node.Id);
				}
				continue;
			}
			Overrides.RemovedEntities.erase(Node.Id);
			Mapped[Index] = Found->second;
		}

		const FRemap Remap = [&](FEntity SourceEntity) {
			const FPrefabTemplate::FNode* Node = Template.FindByEntity(SourceEntity);
			if (Node == nullptr)
			{
				return NullEntity;
			}
			const size_t Index = static_cast<size_t>(Node - Template.Nodes.data());
			return Skipped[Index] ? NullEntity : Mapped[Index];
		};

		const FTypeInfo* InstanceType = GetInstanceType();
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (Skipped[Index])
			{
				continue;
			}
			const FPrefabTemplate::FNode&       Node   = Template.Nodes[Index];
			const FEntity                       Dest   = Mapped[Index];
			const bool                          bRoot  = Index == 0;
			FPrefabOverrides::FEntityOverrides& Entity = Overrides.Entities[Node.Id];
			FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
				if (IsIgnoredType(Type) || (bRoot && &Type == InstanceType))
				{
					return;
				}
				const bool bInSource = Type.HasComponent(Source, Node.Entity);
				const bool bInTarget = Type.HasComponent(Target, Dest);
				if (bInSource && bInTarget)
				{
					Entity.RemovedComponents.erase(Type.Name);
					Entity.AddedComponents.erase(Type.Name);
					const void* Component = Type.GetComponent(Target, Dest);
					const void* Original  = Type.GetComponent(Source, Node.Entity);
					for (const FPropertyInfo& Property : Type.Properties)
					{
						if (!FEntityJson::IsSerializable(Property) || (bRoot && IsRootOwned(Type, Property)))
						{
							continue;
						}
						if (!PropertyEquals(Property, Component, Original, Remap))
						{
							Entity.Properties.insert(MakePropertyKey(Type, Property));
						}
					}
				}
				else if (bInSource)
				{
					Entity.RemovedComponents.insert(Type.Name);
				}
				else if (bInTarget && (Type.bRemovable || &Type == InstanceType))
				{
					Entity.AddedComponents.insert(Type.Name);
				}
			});
		}

		// 빈 항목, 원본에 없는 Id 정리
		std::erase_if(Overrides.Entities, [&](const auto& Pair) { return Pair.second.IsEmpty() || !Template.IndexById.contains(Pair.first); });
		std::erase_if(Overrides.RemovedEntities, [&](const std::string& Id) { return !Template.IndexById.contains(Id); });
		return !(Overrides == Before);
	}

	// 씬 인스턴스 소속: Root 하위에서 링크 Root가 이 인스턴스인 엔티티 (Id 중복은 첫 번째만)
	FMemberMap CollectSceneMembers(FScene& Scene, FEntity Root)
	{
		FMemberMap           Members;
		FRegistry&           Registry = Scene.GetRegistry();
		std::vector<FEntity> Stack{ Root };
		while (!Stack.empty())
		{
			const FEntity Entity = Stack.back();
			Stack.pop_back();
			if (const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Entity); Link && Link->Root == Root && !Link->Id.empty())
			{
				Members.emplace(Link->Id, Entity);
			}
			const std::vector<FEntity>& Children = Scene.GetChildren(Entity);
			Stack.insert(Stack.end(), Children.rbegin(), Children.rend());
		}
		return Members;
	}

	// 프리팹 파일 안 중첩 인스턴스 소속: NestedRoot 자신(= 안쪽 원본 루트) + Id가 "Prefix/"로 시작하는 하위 엔티티 (키는 안쪽 Id)
	FMemberMap CollectNestedMembers(FScene& Scene, FEntity NestedRoot, const std::string& Prefix, const std::string& InnerRootId)
	{
		FMemberMap           Members;
		FRegistry&           Registry = Scene.GetRegistry();
		const std::string    Head     = Prefix + "/";
		Members.emplace(InnerRootId, NestedRoot);
		std::vector<FEntity> Stack(Scene.GetChildren(NestedRoot).begin(), Scene.GetChildren(NestedRoot).end());
		while (!Stack.empty())
		{
			const FEntity Entity = Stack.back();
			Stack.pop_back();
			if (const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Entity); Link && Link->Id.rfind(Head, 0) == 0)
			{
				Members.emplace(Link->Id.substr(Head.size()), Entity);
			}
			const std::vector<FEntity>& Children = Scene.GetChildren(Entity);
			Stack.insert(Stack.end(), Children.begin(), Children.end());
		}
		return Members;
	}

	struct FNestedRoot
	{
		FEntity     Entity;
		std::string Id;
		std::string Asset;
	};

	// 최상위(안쪽이 아닌) 중첩 인스턴스 루트 목록 (루트 제외, 부모 먼저)
	std::vector<FNestedRoot> CollectTopLevelNested(FScene& Scene, FEntity Root)
	{
		std::vector<FEntity> Entities;
		FEntityJson::CollectSubtree(Scene, Root, Entities);
		std::vector<FNestedRoot> Result;
		FRegistry&               Registry = Scene.GetRegistry();
		for (size_t Index = 1; Index < Entities.size(); ++Index)
		{
			const FPrefabInstanceComponent* Instance = Registry.TryGet<FPrefabInstanceComponent>(Entities[Index]);
			const FPrefabLinkComponent*     Link     = Registry.TryGet<FPrefabLinkComponent>(Entities[Index]);
			if (Instance != nullptr && Link != nullptr && IsTopLevelId(Link->Id))
			{
				Result.push_back({ Entities[Index], Link->Id, Instance->Asset });
			}
		}
		return Result;
	}

	uint32 ParseLeadingNumber(const std::string& Id)
	{
		uint32 Value = 0;
		std::from_chars(Id.data(), Id.data() + Id.size(), Value);
		return Value;
	}
} // namespace

// ---------------------------------------------------------------- FPrefabOverrides

FPrefabOverrides FPrefabOverrides::Parse(std::string_view Json)
{
	FPrefabOverrides Result;
	if (Json.empty())
	{
		return Result;
	}
	const json Document = json::parse(Json, nullptr, false);
	if (Document.is_discarded() || !Document.is_object())
	{
		E_LOG(LogScene, Warning, "프리팹 오버라이드 JSON 형식 오류 — 무시합니다");
		return Result;
	}
	const auto ReadSet = [](const json& Node, const char* Key, std::set<std::string>& Out) {
		if (const auto It = Node.find(Key); It != Node.end() && It->is_array())
		{
			for (const json& Item : *It)
			{
				if (Item.is_string())
				{
					Out.insert(Item.get<std::string>());
				}
			}
		}
	};
	if (const auto It = Document.find("Entities"); It != Document.end() && It->is_object())
	{
		for (const auto& [Id, Node] : It->items())
		{
			if (!Node.is_object())
			{
				continue;
			}
			FEntityOverrides& Entity = Result.Entities[Id];
			ReadSet(Node, "Properties", Entity.Properties);
			ReadSet(Node, "Added", Entity.AddedComponents);
			ReadSet(Node, "Removed", Entity.RemovedComponents);
		}
	}
	ReadSet(Document, "RemovedEntities", Result.RemovedEntities);
	std::erase_if(Result.Entities, [](const auto& Pair) { return Pair.second.IsEmpty(); });
	return Result;
}

std::string FPrefabOverrides::Serialize() const
{
	if (IsEmpty())
	{
		return std::string();
	}
	json Document = json::object();
	if (!Entities.empty())
	{
		json EntitiesJson = json::object();
		for (const auto& [Id, Entity] : Entities)
		{
			if (Entity.IsEmpty())
			{
				continue;
			}
			json Node = json::object();
			if (!Entity.Properties.empty())
			{
				Node["Properties"] = Entity.Properties;
			}
			if (!Entity.AddedComponents.empty())
			{
				Node["Added"] = Entity.AddedComponents;
			}
			if (!Entity.RemovedComponents.empty())
			{
				Node["Removed"] = Entity.RemovedComponents;
			}
			EntitiesJson[Id] = std::move(Node);
		}
		Document["Entities"] = std::move(EntitiesJson);
	}
	if (!RemovedEntities.empty())
	{
		Document["RemovedEntities"] = RemovedEntities;
	}
	return Document.dump();
}

bool FPrefabOverrides::IsEmpty() const
{
	if (!RemovedEntities.empty())
	{
		return false;
	}
	return std::all_of(Entities.begin(), Entities.end(), [](const auto& Pair) { return Pair.second.IsEmpty(); });
}

bool FPrefabOverrides::IsPropertyOverridden(const std::string& Id, std::string_view Key) const
{
	const auto Found = Entities.find(Id);
	return Found != Entities.end() && Found->second.Properties.contains(std::string(Key));
}

size_t FPrefabOverrides::Count() const
{
	size_t Result = RemovedEntities.size();
	for (const auto& [Id, Entity] : Entities)
	{
		Result += Entity.Properties.size() + Entity.AddedComponents.size() + Entity.RemovedComponents.size();
	}
	return Result;
}

// ---------------------------------------------------------------- FPrefabTemplate

FPrefabTemplate::FPrefabTemplate()
	: Scene(std::make_unique<FScene>())
{
}

FPrefabTemplate::~FPrefabTemplate() = default;

FEntity FPrefabTemplate::GetRoot() const
{
	return Nodes.empty() ? NullEntity : Nodes.front().Entity;
}

const std::string& FPrefabTemplate::GetRootId() const
{
	static const std::string Empty;
	return Nodes.empty() ? Empty : Nodes.front().Id;
}

const FPrefabTemplate::FNode* FPrefabTemplate::FindById(const std::string& Id) const
{
	const auto Found = IndexById.find(Id);
	return Found != IndexById.end() ? &Nodes[Found->second] : nullptr;
}

const FPrefabTemplate::FNode* FPrefabTemplate::FindByEntity(FEntity Entity) const
{
	const auto Found = IndexByEntity.find(Entity.ToId());
	return Found != IndexByEntity.end() ? &Nodes[Found->second] : nullptr;
}

void FPrefabTemplate::RebuildIndex(FEntity Root)
{
	Nodes.clear();
	IndexById.clear();
	IndexByEntity.clear();
	std::vector<FEntity> Entities;
	FEntityJson::CollectSubtree(*Scene, Root, Entities);
	FRegistry& Registry = Scene->GetRegistry();
	for (FEntity Entity : Entities)
	{
		FNode Node;
		Node.Entity = Entity;
		if (const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Entity))
		{
			Node.Id = Link->Id;
		}
		const auto Parent = IndexByEntity.find(Scene->GetParent(Entity).ToId());
		Node.Parent       = Parent != IndexByEntity.end() && Entity != Root ? static_cast<int32>(Parent->second) : -1;
		IndexByEntity[Entity.ToId()] = Nodes.size();
		if (!Node.Id.empty())
		{
			IndexById.emplace(Node.Id, Nodes.size()); // 중복 Id는 첫 번째만
		}
		Nodes.push_back(std::move(Node));
	}
}

// ---------------------------------------------------------------- FPrefabLibrary: 경로/캐시

FPrefabLibrary& FPrefabLibrary::Get()
{
	static FPrefabLibrary Instance;
	return Instance;
}

void FPrefabLibrary::SetContentDirectory(const std::filesystem::path& Directory)
{
	if (bContentDirectorySet && Directory == ContentDirectory)
	{
		return;
	}
	ContentDirectory     = Directory;
	bContentDirectorySet = true;
	Invalidate();
}

const std::filesystem::path& FPrefabLibrary::GetContentDirectory() const
{
	if (!bContentDirectorySet && FPaths::IsInitialized() && FPaths::HasProject())
	{
		// 앱이 지정하지 않았으면 프로젝트 Content (런타임)
		FPrefabLibrary& Self      = const_cast<FPrefabLibrary&>(*this);
		Self.ContentDirectory     = FPaths::GetProjectContentDirectory();
		Self.bContentDirectorySet = true;
	}
	return ContentDirectory;
}

std::filesystem::path FPrefabLibrary::ResolveAssetPath(const std::string& Asset) const
{
	const std::filesystem::path Path = FStringConv::ToWide(Asset);
	return (Path.is_absolute() ? Path : GetContentDirectory() / Path).lexically_normal();
}

std::string FPrefabLibrary::MakeAssetPath(const std::filesystem::path& Path) const
{
	const std::filesystem::path Normal = Path.lexically_normal();
	if (!GetContentDirectory().empty())
	{
		const std::filesystem::path Relative = Normal.lexically_relative(GetContentDirectory().lexically_normal());
		if (!Relative.empty() && Relative.native().rfind(L"..", 0) != 0)
		{
			return FStringConv::ToUtf8(Relative.generic_wstring());
		}
	}
	return FStringConv::ToUtf8(Normal.generic_wstring());
}

std::wstring FPrefabLibrary::MakeKey(const std::string& Asset) const
{
	return Lower(ResolveAssetPath(Asset).generic_wstring());
}

void FPrefabLibrary::Invalidate()
{
	Cache.clear();
	Failed.clear();
}

const FPrefabTemplate* FPrefabLibrary::Load(const std::string& Asset, std::string* OutError)
{
	std::vector<std::wstring> Stack;
	return LoadInternal(Asset, Stack, OutError);
}

const FPrefabTemplate* FPrefabLibrary::LoadInternal(const std::string& Asset, std::vector<std::wstring>& Stack, std::string* OutError)
{
	if (Asset.empty())
	{
		SetError(OutError, "프리팹 경로가 비어 있습니다");
		return nullptr;
	}
	const std::wstring Key = MakeKey(Asset);
	if (const auto Found = Cache.find(Key); Found != Cache.end())
	{
		return Found->second.get();
	}
	if (std::find(Stack.begin(), Stack.end(), Key) != Stack.end())
	{
		SetError(OutError, "프리팹 순환 참조: " + Asset);
		return nullptr;
	}
	if (const auto Found = Failed.find(Key); Found != Failed.end())
	{
		SetError(OutError, Found->second);
		return nullptr;
	}
	const auto Fail = [&](std::string Message) -> const FPrefabTemplate* {
		E_LOG(LogScene, Error, "{}", Message);
		Failed[Key] = Message;
		SetError(OutError, std::move(Message));
		return nullptr;
	};

	std::string Text;
	if (!ReadTextFile(ResolveAssetPath(Asset), Text))
	{
		return Fail("프리팹 파일을 열 수 없습니다: " + Asset);
	}
	const json Document = json::parse(Text, nullptr, false, true);
	if (Document.is_discarded() || !Document.is_object())
	{
		return Fail("프리팹 JSON 파싱 실패: " + Asset);
	}
	const auto EntitiesIt = Document.find("Entities");
	if (EntitiesIt == Document.end() || !EntitiesIt->is_array() || EntitiesIt->empty())
	{
		return Fail("프리팹에 엔티티가 없습니다: " + Asset);
	}
	if (Document.value("Version", 0) > Version)
	{
		E_LOG(LogScene, Warning, "프리팹 버전 {}이(가) 지원 버전 {}보다 높습니다: {}", Document.value("Version", 0), Version, Asset);
	}

	std::unique_ptr<FPrefabTemplate> Template = std::make_unique<FPrefabTemplate>();
	Template->Asset                           = MakeAssetPath(ResolveAssetPath(Asset));
	Template->NextId                          = Document.value("NextId", 1u);
	const std::vector<FEntity> Entities       = FEntityJson::Read(*Template->Scene, *EntitiesIt, NullEntity, Asset);
	FRegistry&                 Registry       = Template->Scene->GetRegistry();
	const FEntity              Root           = Entities.front();

	// 손으로 만든 파일 등 링크가 없는 엔티티에 ID 부여 (파일 순서대로 → 같은 파일이면 같은 결과)
	for (FEntity Entity : Entities)
	{
		FPrefabLinkComponent& Link = Registry.GetOrEmplace<FPrefabLinkComponent>(Entity);
		Link.Root                  = NullEntity;
		if (Link.Id.empty())
		{
			Link.Id = std::to_string(Template->NextId++);
		}
	}
	Registry.Remove<FPrefabInstanceComponent>(Root); // 루트는 다른 프리팹의 인스턴스가 될 수 없다 (변형 프리팹 미지원)
	Template->RebuildIndex(Root);

	// 중첩 인스턴스: 각 원본의 현재 내용 + 이 파일이 기록한 오버라이드로 다시 맞춘다
	Stack.push_back(Key);
	for (const FNestedRoot& Nested : CollectTopLevelNested(*Template->Scene, Root))
	{
		if (!Registry.IsValid(Nested.Entity))
		{
			continue; // 앞선 중첩 동기화에서 삭제됨
		}
		std::string            Error;
		const FPrefabTemplate* Inner = LoadInternal(Nested.Asset, Stack, &Error);
		if (Inner == nullptr)
		{
			E_LOG(LogScene, Warning, "{}: 중첩 프리팹을 맞추지 못해 저장된 내용을 씁니다 — {}", Asset, Error);
			continue;
		}
		FMemberMap             Members   = CollectNestedMembers(*Template->Scene, Nested.Entity, Nested.Id, Inner->GetRootId());
		const FPrefabOverrides Overrides = FPrefabOverrides::Parse(Registry.Get<FPrefabInstanceComponent>(Nested.Entity).Overrides);
		const std::string      Prefix    = Nested.Id;
		SyncCore(*Inner, *Template->Scene, Members, Overrides, [&Prefix](const std::string& InnerId) {
			return FPrefabLinkComponent{ Prefix + "/" + InnerId, NullEntity };
		});
	}
	Stack.pop_back();
	Template->RebuildIndex(Root);
	Template->Scene->UpdateTransforms();

	const FPrefabTemplate* Result = Template.get();
	Cache.emplace(Key, std::move(Template));
	return Result;
}

bool FPrefabLibrary::DependsOn(const std::string& Asset, const std::string& Target)
{
	const std::wstring        TargetKey = MakeKey(Target);
	std::vector<std::wstring> Visited;
	const std::function<bool(const std::string&)> Visit = [&](const std::string& Current) {
		const std::wstring Key = MakeKey(Current);
		if (Key == TargetKey)
		{
			return true;
		}
		if (std::find(Visited.begin(), Visited.end(), Key) != Visited.end())
		{
			return false;
		}
		Visited.push_back(Key);
		const FPrefabTemplate* Template = Load(Current);
		if (Template == nullptr)
		{
			return false;
		}
		// 원본 안의 모든 중첩 인스턴스 (안쪽의 안쪽 포함)
		std::vector<std::string> Assets;
		for (const FPrefabTemplate::FNode& Node : Template->Nodes)
		{
			if (const FPrefabInstanceComponent* Instance = Template->Scene->GetRegistry().TryGet<FPrefabInstanceComponent>(Node.Entity))
			{
				Assets.push_back(Instance->Asset);
			}
		}
		return std::any_of(Assets.begin(), Assets.end(), Visit);
	};
	return Visit(Asset);
}

// ---------------------------------------------------------------- 씬 인스턴스

FEntity FPrefabLibrary::Instantiate(FScene& Scene, const std::string& Asset, FEntity Parent, std::string* OutError)
{
	const FPrefabTemplate* Template = Load(Asset, OutError);
	if (Template == nullptr)
	{
		return NullEntity;
	}
	std::vector<FEntity> Sources;
	Sources.reserve(Template->Nodes.size());
	for (const FPrefabTemplate::FNode& Node : Template->Nodes)
	{
		Sources.push_back(Node.Entity);
	}
	const json                 Array   = FEntityJson::Write(*Template->Scene, Sources);
	const std::vector<FEntity> Created = FEntityJson::Read(Scene, Array, Parent, Template->Asset);
	FRegistry&                 Registry = Scene.GetRegistry();
	const FEntity              Root     = Created.front();
	for (FEntity Entity : Created)
	{
		Registry.GetOrEmplace<FPrefabLinkComponent>(Entity).Root = Root;
	}
	FPrefabInstanceComponent& Instance = Registry.GetOrEmplace<FPrefabInstanceComponent>(Root);
	Instance.Asset                     = Template->Asset;
	Instance.Overrides.clear();
	return Root;
}

bool FPrefabLibrary::SyncInstance(FScene& Scene, FEntity InstanceRoot)
{
	FRegistry&                      Registry = Scene.GetRegistry();
	const FPrefabInstanceComponent* Instance = Registry.TryGet<FPrefabInstanceComponent>(InstanceRoot);
	if (Instance == nullptr)
	{
		return false;
	}
	const std::string      Asset     = Instance->Asset;
	const FPrefabOverrides Overrides = FPrefabOverrides::Parse(Instance->Overrides);
	const FPrefabTemplate* Template  = Load(Asset);
	if (Template == nullptr)
	{
		return false; // 원본이 없으면 저장된 내용을 그대로 둔다
	}
	FMemberMap Members = CollectSceneMembers(Scene, InstanceRoot);
	if (!Members.contains(Template->GetRootId()))
	{
		// 연결 없는 루트(손으로 만든 씬): 루트를 원본 루트에 연결
		Registry.GetOrEmplace<FPrefabLinkComponent>(InstanceRoot) = { Template->GetRootId(), InstanceRoot };
		Members[Template->GetRootId()] = InstanceRoot;
	}
	return SyncCore(*Template, Scene, Members, Overrides, [InstanceRoot](const std::string& Id) { return FPrefabLinkComponent{ Id, InstanceRoot }; });
}

namespace
{
	std::vector<FEntity> CollectOutermostInstances(FScene& Scene)
	{
		std::vector<FEntity> Roots;
		FRegistry&           Registry = Scene.GetRegistry();
		Registry.View<FPrefabInstanceComponent>().Each([&](FEntity Entity, FPrefabInstanceComponent&) {
			if (FPrefabLibrary::IsInstanceRoot(Scene, Entity))
			{
				Roots.push_back(Entity);
			}
		});
		return Roots;
	}
} // namespace

bool FPrefabLibrary::SyncAllInstances(FScene& Scene)
{
	bool bChanged = false;
	for (FEntity Root : CollectOutermostInstances(Scene))
	{
		if (Scene.GetRegistry().IsValid(Root))
		{
			bChanged = SyncInstance(Scene, Root) || bChanged;
		}
	}
	return bChanged;
}

bool FPrefabLibrary::RecordOverrides(FScene& Scene, FEntity InstanceRoot)
{
	FRegistry&                Registry = Scene.GetRegistry();
	FPrefabInstanceComponent* Instance = Registry.TryGet<FPrefabInstanceComponent>(InstanceRoot);
	if (Instance == nullptr)
	{
		return false;
	}
	const FPrefabTemplate* Template = Load(Instance->Asset);
	if (Template == nullptr)
	{
		return false;
	}
	FPrefabOverrides Overrides = FPrefabOverrides::Parse(Instance->Overrides);
	if (!RecordCore(*Template, Scene, CollectSceneMembers(Scene, InstanceRoot), Overrides))
	{
		return false;
	}
	Registry.Get<FPrefabInstanceComponent>(InstanceRoot).Overrides = Overrides.Serialize();
	return true;
}

bool FPrefabLibrary::RecordAllOverrides(FScene& Scene)
{
	bool bChanged = false;
	for (FEntity Root : CollectOutermostInstances(Scene))
	{
		bChanged = RecordOverrides(Scene, Root) || bChanged;
	}
	return bChanged;
}

namespace
{
	// 인스턴스 루트의 오버라이드를 Edit로 고친 뒤 저장. Edit가 false면 저장하지 않음
	bool EditOverrides(FScene& Scene, FEntity Member, const std::function<bool(FPrefabOverrides&, const std::string& Id)>& Edit, FEntity& OutRoot)
	{
		FRegistry&                  Registry = Scene.GetRegistry();
		const FPrefabLinkComponent* Link     = Registry.TryGet<FPrefabLinkComponent>(Member);
		OutRoot                              = FPrefabLibrary::FindInstanceRoot(Scene, Member);
		if (Link == nullptr || !OutRoot.IsValid())
		{
			return false;
		}
		const std::string         Id       = Link->Id;
		FPrefabInstanceComponent& Instance = Registry.Get<FPrefabInstanceComponent>(OutRoot);
		FPrefabOverrides          Overrides = FPrefabOverrides::Parse(Instance.Overrides);
		if (!Edit(Overrides, Id))
		{
			return false;
		}
		std::erase_if(Overrides.Entities, [](const auto& Pair) { return Pair.second.IsEmpty(); });
		Instance.Overrides = Overrides.Serialize();
		return true;
	}
} // namespace

bool FPrefabLibrary::RevertProperty(FScene& Scene, FEntity Member, const std::string& Key)
{
	FEntity Root;
	const bool bEdited = EditOverrides(Scene, Member, [&Key](FPrefabOverrides& Overrides, const std::string& Id) {
		const auto Found = Overrides.Entities.find(Id);
		return Found != Overrides.Entities.end() && Found->second.Properties.erase(Key) > 0;
	}, Root);
	if (bEdited)
	{
		SyncInstance(Scene, Root);
	}
	return bEdited;
}

bool FPrefabLibrary::RevertComponent(FScene& Scene, FEntity Member, const std::string& ComponentName)
{
	FEntity Root;
	const bool bEdited = EditOverrides(Scene, Member, [&ComponentName](FPrefabOverrides& Overrides, const std::string& Id) {
		const auto Found = Overrides.Entities.find(Id);
		if (Found == Overrides.Entities.end())
		{
			return false;
		}
		const size_t Erased = Found->second.AddedComponents.erase(ComponentName) + Found->second.RemovedComponents.erase(ComponentName);
		// 다시 추가된 컴포넌트는 원본 값으로 시작한다
		std::erase_if(Found->second.Properties, [&](const std::string& Key) { return Key.rfind(ComponentName + ".", 0) == 0; });
		return Erased > 0;
	}, Root);
	if (bEdited)
	{
		SyncInstance(Scene, Root);
	}
	return bEdited;
}

bool FPrefabLibrary::RevertAll(FScene& Scene, FEntity InstanceRoot)
{
	FRegistry&                Registry = Scene.GetRegistry();
	FPrefabInstanceComponent* Instance = Registry.TryGet<FPrefabInstanceComponent>(InstanceRoot);
	if (Instance == nullptr)
	{
		return false;
	}
	Instance->Overrides.clear();

	// 인스턴스에서 추가한 자식 엔티티 삭제 (모델이 만든 생성 노드는 모델이 관리하므로 둔다)
	std::vector<FEntity> Added;
	std::vector<FEntity> Stack{ InstanceRoot };
	while (!Stack.empty())
	{
		const FEntity Entity = Stack.back();
		Stack.pop_back();
		for (FEntity Child : Scene.GetChildren(Entity))
		{
			const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Child);
			if (Link != nullptr && Link->Root == InstanceRoot)
			{
				Stack.push_back(Child);
			}
			else if (!Registry.Has<FTransientComponent>(Child))
			{
				Added.push_back(Child);
			}
		}
	}
	for (FEntity Entity : Added)
	{
		Scene.DestroyEntity(Entity);
	}
	SyncInstance(Scene, InstanceRoot);
	return true;
}

void FPrefabLibrary::Unpack(FScene& Scene, FEntity InstanceRoot)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (!IsInstanceRoot(Scene, InstanceRoot))
	{
		return;
	}
	const FMemberMap Members = CollectSceneMembers(Scene, InstanceRoot);

	// 최상위 중첩 인스턴스는 독립 인스턴스로 승격: 안쪽 엔티티 Id에서 바깥 접두사를 뗀다
	std::unordered_map<uint64, bool> Promoted;
	for (const auto& [Id, Entity] : Members)
	{
		const FPrefabInstanceComponent* Instance = Registry.TryGet<FPrefabInstanceComponent>(Entity);
		if (Entity == InstanceRoot || Instance == nullptr || !IsTopLevelId(Id))
		{
			continue;
		}
		const FPrefabTemplate* Inner = Load(Instance->Asset);
		const std::string      Head  = Id + "/";
		for (const auto& [OtherId, Other] : Members)
		{
			if (OtherId.rfind(Head, 0) == 0)
			{
				Registry.Get<FPrefabLinkComponent>(Other) = { OtherId.substr(Head.size()), Entity };
				Promoted[Other.ToId()] = true;
			}
		}
		Registry.Get<FPrefabLinkComponent>(Entity) = { Inner ? Inner->GetRootId() : std::string("1"), Entity };
		Promoted[Entity.ToId()]                   = true;
	}
	for (const auto& [Id, Entity] : Members)
	{
		if (!Promoted.contains(Entity.ToId()))
		{
			Registry.Remove<FPrefabLinkComponent>(Entity);
		}
	}
	Registry.Remove<FPrefabInstanceComponent>(InstanceRoot);
}

// ---------------------------------------------------------------- 저장 (원본 파일 만들기)

bool FPrefabLibrary::BuildAssetDocument(FScene& Scene, FEntity Root, bool bKeepSelfIds, FEntity SelfRoot, uint32& InOutNextId,
                                        const std::string& SavingAsset, std::string& OutJson, std::vector<std::string>& OutIds,
                                        std::string* OutError)
{
	std::vector<FEntity> Entities;
	FEntityJson::CollectSubtree(Scene, Root, Entities);
	if (Entities.empty())
	{
		SetError(OutError, "저장할 엔티티가 없습니다");
		return false;
	}

	// 임시 씬에 복사해 가공한다 (엔티티 참조는 목록 인덱스로 옮겨진다)
	FScene                     Temp;
	FRegistry&                 Registry = Temp.GetRegistry();
	const std::vector<FEntity> Copies   = FEntityJson::Read(Temp, FEntityJson::Write(Scene, Entities), NullEntity, "프리팹 저장");
	FEntity                    SelfCopy;
	if (bKeepSelfIds && SelfRoot.IsValid())
	{
		const auto Found = std::find(Entities.begin(), Entities.end(), SelfRoot);
		SelfCopy         = Found != Entities.end() ? Copies[static_cast<size_t>(Found - Entities.begin())] : NullEntity;
	}

	// ID: 자기 원본 소속은 유지, 안에 놓인 다른 인스턴스는 "새 ID/안쪽 ID", 나머지는 새 ID
	for (FEntity Copy : Copies)
	{
		const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Copy);
		if (bKeepSelfIds && Link != nullptr && Link->Root == SelfCopy && !Link->Id.empty())
		{
			InOutNextId = std::max(InOutNextId, ParseLeadingNumber(Link->Id) + 1);
		}
	}
	std::vector<std::string>                     Ids(Copies.size());
	std::unordered_map<uint64, std::string>      NestedBase; // 인스턴스 루트 → 새 ID
	for (size_t Index = 0; Index < Copies.size(); ++Index)
	{
		const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Copies[Index]);
		if (bKeepSelfIds && Link != nullptr && Link->Root == SelfCopy && !Link->Id.empty())
		{
			Ids[Index] = Link->Id;
		}
		else if (Link != nullptr && Link->Root.IsValid() && Link->Root != SelfCopy)
		{
			if (Link->Root == Copies[Index])
			{
				Ids[Index]                         = std::to_string(InOutNextId++);
				NestedBase[Copies[Index].ToId()] = Ids[Index];
			}
			else if (const auto Base = NestedBase.find(Link->Root.ToId()); Base != NestedBase.end() && !Link->Id.empty())
			{
				Ids[Index] = Base->second + "/" + Link->Id;
			}
			else
			{
				Ids[Index] = std::to_string(InOutNextId++);
			}
		}
		else
		{
			Ids[Index] = std::to_string(InOutNextId++);
		}
	}
	for (size_t Index = 0; Index < Copies.size(); ++Index)
	{
		Registry.GetOrEmplace<FPrefabLinkComponent>(Copies[Index]) = { Ids[Index], NullEntity };
	}

	// 원본 루트: 인스턴스 표식 제거, 위치/회전은 원점 (인스턴스가 정한다)
	const FEntity RootCopy = Copies.front();
	Registry.Remove<FPrefabInstanceComponent>(RootCopy);
	Temp.GetTransform(RootCopy).Position = FVector3::ZeroVector;
	Temp.GetTransform(RootCopy).Rotation = FQuat::Identity;

	// 중첩 인스턴스: 순환 검사 후 안쪽 원본과의 차이를 그 인스턴스의 오버라이드로 기록
	for (const FNestedRoot& Nested : CollectTopLevelNested(Temp, RootCopy))
	{
		if (!SavingAsset.empty() && DependsOn(Nested.Asset, SavingAsset))
		{
			SetError(OutError, std::format("프리팹 '{}'이(가) 자기 자신을 포함하게 되어 저장하지 않았습니다 (중첩 '{}')", SavingAsset, Nested.Asset));
			return false;
		}
		const FPrefabTemplate* Inner = Load(Nested.Asset);
		if (Inner == nullptr)
		{
			continue; // 원본이 없으면 저장된 오버라이드 유지
		}
		FPrefabOverrides Overrides = FPrefabOverrides::Parse(Registry.Get<FPrefabInstanceComponent>(Nested.Entity).Overrides);
		RecordCore(*Inner, Temp, CollectNestedMembers(Temp, Nested.Entity, Nested.Id, Inner->GetRootId()), Overrides);
		Registry.Get<FPrefabInstanceComponent>(Nested.Entity).Overrides = Overrides.Serialize();
	}

	json Document;
	Document["Version"]  = Version;
	Document["NextId"]   = InOutNextId;
	Document["Entities"] = FEntityJson::Write(Temp, Copies);
	OutJson              = Document.dump(2);
	OutIds               = std::move(Ids);
	return true;
}

bool FPrefabLibrary::CreatePrefab(FScene& Scene, FEntity Entity, const std::filesystem::path& File, std::string* OutError)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Entity) || Registry.Has<FTransientComponent>(Entity))
	{
		SetError(OutError, "프리팹으로 만들 수 없는 엔티티입니다 (모델이 만든 노드 등)");
		return false;
	}
	if (IsInstanceRoot(Scene, Entity))
	{
		SetError(OutError, "이미 프리팹 인스턴스입니다 (원본에 적용을 쓰세요)");
		return false;
	}
	if (FindInstanceRoot(Scene, Entity).IsValid())
	{
		SetError(OutError, "프리팹 인스턴스의 일부로는 새 프리팹을 만들 수 없습니다 (먼저 연결 해제)");
		return false;
	}
	std::error_code ErrorCode;
	if (std::filesystem::exists(File, ErrorCode))
	{
		SetError(OutError, "같은 이름의 파일이 이미 있습니다: " + FStringConv::ToUtf8(File.filename().wstring()));
		return false;
	}

	const std::string        Asset  = MakeAssetPath(File);
	uint32                   NextId = 1;
	std::string              Json;
	std::vector<std::string> Ids;
	if (!BuildAssetDocument(Scene, Entity, false, NullEntity, NextId, Asset, Json, Ids, OutError))
	{
		return false;
	}
	if (!WriteTextFile(File, Json))
	{
		SetError(OutError, "프리팹 파일을 쓸 수 없습니다: " + FStringConv::ToUtf8(File.wstring()));
		return false;
	}

	// 원래 엔티티를 새 프리팹의 인스턴스로 (안에 있던 인스턴스는 중첩 인스턴스가 된다)
	std::vector<FEntity> Entities;
	FEntityJson::CollectSubtree(Scene, Entity, Entities);
	for (size_t Index = 0; Index < Entities.size(); ++Index)
	{
		Registry.GetOrEmplace<FPrefabLinkComponent>(Entities[Index]) = { Ids[Index], Entity };
	}
	FPrefabInstanceComponent& Instance = Registry.GetOrEmplace<FPrefabInstanceComponent>(Entity);
	Instance.Asset                     = Asset;
	Instance.Overrides.clear();
	Invalidate();
	E_LOG(LogScene, Display, "프리팹 생성: {} (엔티티 {}개)", Asset, Entities.size());
	return true;
}

bool FPrefabLibrary::ApplyToPrefab(FScene& Scene, FEntity InstanceRoot, std::string* OutError)
{
	if (!IsInstanceRoot(Scene, InstanceRoot))
	{
		SetError(OutError, "프리팹 인스턴스 루트가 아닙니다");
		return false;
	}
	// 다른 인스턴스의 아직 기록 안 된 편집이 원본 반영에 덮이지 않게 먼저 기록
	RecordAllOverrides(Scene);

	FRegistry&             Registry = Scene.GetRegistry();
	const std::string      Asset    = Registry.Get<FPrefabInstanceComponent>(InstanceRoot).Asset;
	const FPrefabTemplate* Template = Load(Asset, OutError);
	if (Template == nullptr)
	{
		return false;
	}
	uint32                   NextId = Template->NextId;
	std::string              Json;
	std::vector<std::string> Ids;
	if (!BuildAssetDocument(Scene, InstanceRoot, true, InstanceRoot, NextId, Asset, Json, Ids, OutError))
	{
		return false;
	}
	if (!WriteTextFile(ResolveAssetPath(Asset), Json))
	{
		SetError(OutError, "프리팹 파일을 쓸 수 없습니다: " + Asset);
		return false;
	}

	std::vector<FEntity> Entities;
	FEntityJson::CollectSubtree(Scene, InstanceRoot, Entities);
	for (size_t Index = 0; Index < Entities.size(); ++Index)
	{
		Registry.GetOrEmplace<FPrefabLinkComponent>(Entities[Index]) = { Ids[Index], InstanceRoot };
	}
	Registry.Get<FPrefabInstanceComponent>(InstanceRoot).Overrides.clear();
	Invalidate();
	SyncAllInstances(Scene);
	E_LOG(LogScene, Display, "프리팹 원본에 적용: {}", Asset);
	return true;
}

// ---------------------------------------------------------------- 조회

FEntity FPrefabLibrary::FindInstanceRoot(const FScene& Scene, FEntity Entity)
{
	const FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Entity))
	{
		return NullEntity;
	}
	if (const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Entity); Link && Link->Root.IsValid())
	{
		return Registry.IsValid(Link->Root) && Registry.Has<FPrefabInstanceComponent>(Link->Root) ? Link->Root : NullEntity;
	}
	return IsInstanceRoot(Scene, Entity) ? Entity : NullEntity;
}

bool FPrefabLibrary::IsInstanceRoot(const FScene& Scene, FEntity Entity)
{
	const FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Entity) || !Registry.Has<FPrefabInstanceComponent>(Entity))
	{
		return false;
	}
	const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Entity);
	return Link == nullptr || Link->Root == Entity;
}

// ---------------------------------------------------------------- 프리팹 편집 창

FEntity FPrefabLibrary::LoadAssetInto(FScene& Scene, const std::string& Asset, FEntity Parent, uint32& OutNextId, std::string* OutError)
{
	const FPrefabTemplate* Template = Load(Asset, OutError);
	if (Template == nullptr)
	{
		return NullEntity;
	}
	std::vector<FEntity> Sources;
	for (const FPrefabTemplate::FNode& Node : Template->Nodes)
	{
		Sources.push_back(Node.Entity);
	}
	OutNextId = Template->NextId;
	return FEntityJson::Read(Scene, FEntityJson::Write(*Template->Scene, Sources), Parent, Template->Asset).front();
}

std::string FPrefabLibrary::AssetStateToJson(FScene& Scene, FEntity Root, uint32 NextId) const
{
	std::vector<FEntity> Entities;
	FEntityJson::CollectSubtree(Scene, Root, Entities);
	json Document;
	Document["Version"]  = Version;
	Document["NextId"]   = NextId;
	Document["Entities"] = FEntityJson::Write(Scene, Entities);
	return Document.dump(2);
}

FEntity FPrefabLibrary::AssetStateFromJson(FScene& Scene, const std::string& Json, FEntity Parent, uint32& OutNextId) const
{
	const json Document = json::parse(Json, nullptr, false, true);
	if (Document.is_discarded() || !Document.is_object())
	{
		return NullEntity;
	}
	const auto EntitiesIt = Document.find("Entities");
	if (EntitiesIt == Document.end() || !EntitiesIt->is_array() || EntitiesIt->empty())
	{
		return NullEntity;
	}
	OutNextId = Document.value("NextId", 1u);
	return FEntityJson::Read(Scene, *EntitiesIt, Parent, "프리팹 편집").front();
}

bool FPrefabLibrary::SaveAsset(FScene& Scene, FEntity Root, const std::filesystem::path& File, uint32& InOutNextId, std::string* OutError)
{
	std::string              Json;
	std::vector<std::string> Ids;
	uint32                   NextId = InOutNextId;
	if (!BuildAssetDocument(Scene, Root, true, NullEntity, NextId, MakeAssetPath(File), Json, Ids, OutError))
	{
		return false;
	}
	if (!WriteTextFile(File, Json))
	{
		SetError(OutError, "프리팹 파일을 쓸 수 없습니다: " + FStringConv::ToUtf8(File.wstring()));
		return false;
	}
	// 편집 씬도 저장된 ID를 갖게 한다 (다음 저장에서 같은 ID 유지)
	std::vector<FEntity> Entities;
	FEntityJson::CollectSubtree(Scene, Root, Entities);
	for (size_t Index = 0; Index < Entities.size(); ++Index)
	{
		Scene.GetRegistry().GetOrEmplace<FPrefabLinkComponent>(Entities[Index]) = { Ids[Index], NullEntity };
	}
	InOutNextId = NextId;
	Invalidate();
	return true;
}
