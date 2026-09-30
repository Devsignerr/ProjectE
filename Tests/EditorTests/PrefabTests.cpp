#include "Core/Testing/TestFramework.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/AssetReferenceUpdater.h"
#include "Editor/SceneEditOps.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scene/SceneCloner.h"
#include "Scene/SceneSerializer.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
	namespace fs = std::filesystem;

	// 프로세스당 한 번 비우는 임시 Content (테스트마다 다른 파일 이름을 쓴다)
	fs::path GetContent()
	{
		static const fs::path Directory = [] {
			const fs::path  Path = fs::temp_directory_path() / L"ProjectEPrefabTests" / L"Content";
			std::error_code ErrorCode;
			fs::remove_all(Path.parent_path(), ErrorCode);
			fs::create_directories(Path / L"Prefabs");
			return Path;
		}();
		FPrefabLibrary::Get().SetContentDirectory(Directory);
		return Directory;
	}

	std::string ReadText(const fs::path& Path)
	{
		std::ifstream     File(Path, std::ios::binary);
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		return Buffer.str();
	}

	void WriteText(const fs::path& Path, const std::string& Text)
	{
		fs::create_directories(Path.parent_path());
		std::ofstream(Path, std::ios::binary | std::ios::trunc) << Text;
	}

	bool Contains(const std::string& Text, const std::string& Needle) { return Text.find(Needle) != std::string::npos; }

	FEntity MakeCube(FScene& Scene, const char* Name, FEntity Parent, const FVector3& Position)
	{
		const FEntity Entity = Scene.CreateEntity(Name);
		Scene.SetParent(Entity, Parent);
		Scene.GetTransform(Entity).Position                             = Position;
		Scene.GetRegistry().Emplace<FStaticMeshComponent>(Entity).MeshAsset = "primitive:cube";
		return Entity;
	}

	// Root 하위(자신 포함)에서 이름으로 찾기 (첫 번째)
	FEntity FindByName(FScene& Scene, FEntity Root, const std::string& Name)
	{
		std::vector<FEntity> Stack{ Root };
		while (!Stack.empty())
		{
			const FEntity Entity = Stack.back();
			Stack.pop_back();
			if (Scene.GetRegistry().Get<FNameComponent>(Entity).Name == Name)
			{
				return Entity;
			}
			const std::vector<FEntity>& Children = Scene.GetChildren(Entity);
			Stack.insert(Stack.end(), Children.rbegin(), Children.rend());
		}
		return NullEntity;
	}

	size_t CountSubtree(FScene& Scene, FEntity Root)
	{
		size_t Count = 1;
		for (FEntity Child : Scene.GetChildren(Root))
		{
			Count += CountSubtree(Scene, Child);
		}
		return Count;
	}

	// "Crate" 프리팹: 루트(Body 큐브) + Lid 큐브 자식. 반환: Content 기준 경로
	std::string MakeCratePrefab(const char* FileName)
	{
		const fs::path File = GetContent() / L"Prefabs" / FStringConv::ToWide(FileName);
		FScene         Scene;
		const FEntity  Body = MakeCube(Scene, "Crate", NullEntity, FVector3(100.0f, 0.0f, 0.0f));
		MakeCube(Scene, "Lid", Body, FVector3(0.0f, 0.0f, 50.0f));
		std::string Error;
		E_EXPECT_TRUE(FPrefabLibrary::Get().CreatePrefab(Scene, Body, File, &Error));
		return FPrefabLibrary::Get().MakeAssetPath(File);
	}

	// 원본 파일을 편집 창처럼 불러와 Edit로 고치고 저장한다 (저장 전 열린 씬 기록 → 캐시 무효화는 호출자가)
	void EditPrefabFile(const std::string& Asset, const std::function<void(FScene&, FEntity)>& Edit)
	{
		FScene        Scene;
		uint32        NextId = 0;
		const FEntity Root   = FPrefabLibrary::Get().LoadAssetInto(Scene, Asset, NullEntity, NextId);
		E_EXPECT_TRUE(Root.IsValid());
		Edit(Scene, Root);
		std::string Error;
		E_EXPECT_TRUE(FPrefabLibrary::Get().SaveAsset(Scene, Root, FPrefabLibrary::Get().ResolveAssetPath(Asset), NextId, &Error));
	}
} // namespace

// ---------------------------------------------------------------- 형식

E_TEST(Prefab_OverridesRoundTrip)
{
	FPrefabOverrides Overrides;
	Overrides.Entities["2"].Properties.insert("TransformComponent.Position");
	Overrides.Entities["3/1"].AddedComponents.insert("ScriptComponent");
	Overrides.Entities["3/1"].RemovedComponents.insert("StaticMeshComponent");
	Overrides.RemovedEntities.insert("4");
	const std::string Json = Overrides.Serialize();
	E_EXPECT_TRUE(FPrefabOverrides::Parse(Json) == Overrides);
	E_EXPECT_EQ(Overrides.Count(), static_cast<size_t>(4));
	E_EXPECT_TRUE(Overrides.IsPropertyOverridden("2", "TransformComponent.Position"));
	E_EXPECT_TRUE(FPrefabOverrides{}.Serialize().empty());
	E_EXPECT_TRUE(FPrefabOverrides::Parse("not json").IsEmpty());
}

E_TEST(Prefab_CreateWritesFileAndConvertsSource)
{
	const fs::path File = GetContent() / L"Prefabs/Create.eprefab";
	FScene         Scene;
	const FEntity  Body = MakeCube(Scene, "Crate", NullEntity, FVector3(100.0f, 20.0f, 0.0f));
	const FEntity  Lid  = MakeCube(Scene, "Lid", Body, FVector3(0.0f, 0.0f, 50.0f));
	std::string    Error;
	E_EXPECT_TRUE(FPrefabLibrary::Get().CreatePrefab(Scene, Body, File, &Error));

	const std::string Text = ReadText(File);
	E_EXPECT_TRUE(Contains(Text, "\"NextId\": 3"));
	E_EXPECT_TRUE(Contains(Text, "\"PrefabLinkComponent\""));
	E_EXPECT_FALSE(Contains(Text, "\"PrefabInstanceComponent\"")); // 루트는 인스턴스 표식을 저장하지 않는다

	// 원래 엔티티는 인스턴스가 되고 위치는 그대로
	E_EXPECT_TRUE(FPrefabLibrary::IsInstanceRoot(Scene, Body));
	E_EXPECT_TRUE(FPrefabLibrary::FindInstanceRoot(Scene, Lid) == Body);
	E_EXPECT_EQ(Scene.GetRegistry().Get<FPrefabInstanceComponent>(Body).Asset, std::string("Prefabs/Create.eprefab"));
	E_EXPECT_NEAR(Scene.GetTransform(Body).Position.Y, 20.0f, 1.0e-4f);
	E_EXPECT_FALSE(FPrefabLibrary::Get().RecordAllOverrides(Scene)); // 방금 만든 인스턴스는 원본과 같다

	// 인스턴스 안이나 인스턴스 루트로는 다시 만들 수 없다
	E_EXPECT_FALSE(FPrefabLibrary::Get().CreatePrefab(Scene, Lid, GetContent() / L"Prefabs/Nope.eprefab", &Error));
	E_EXPECT_FALSE(FPrefabLibrary::Get().CreatePrefab(Scene, Body, GetContent() / L"Prefabs/Nope.eprefab", &Error));

	// 두 번째 인스턴스: 같은 구조, 루트 위치는 원점(인스턴스 값)
	const FEntity Second = FPrefabLibrary::Get().Instantiate(Scene, "Prefabs/Create.eprefab", NullEntity);
	E_EXPECT_TRUE(Second.IsValid());
	E_EXPECT_EQ(CountSubtree(Scene, Second), static_cast<size_t>(2));
	E_EXPECT_TRUE(FindByName(Scene, Second, "Lid").IsValid());
	E_EXPECT_NEAR(Scene.GetTransform(FindByName(Scene, Second, "Lid")).Position.Z, 50.0f, 1.0e-4f);
	E_EXPECT_TRUE(FPrefabLibrary::FindInstanceRoot(Scene, FindByName(Scene, Second, "Lid")) == Second);
}

// ---------------------------------------------------------------- 씬 저장 / 실행 취소 / 플레이 복제

E_TEST(Prefab_SceneSaveLoadSaveIsByteIdentical)
{
	const std::string Asset = MakeCratePrefab("Identical.eprefab");
	FScene            Scene;
	const FEntity     Root = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	Scene.GetTransform(Root).Position                       = FVector3(5.0f, 6.0f, 7.0f);
	Scene.GetTransform(FindByName(Scene, Root, "Lid")).Scale = FVector3(2.0f, 2.0f, 2.0f);
	E_EXPECT_TRUE(FPrefabLibrary::Get().RecordAllOverrides(Scene));
	E_EXPECT_TRUE(Contains(Scene.GetRegistry().Get<FPrefabInstanceComponent>(Root).Overrides, "TransformComponent.Scale"));
	E_EXPECT_FALSE(Contains(Scene.GetRegistry().Get<FPrefabInstanceComponent>(Root).Overrides, "Position")); // 루트 위치는 기록 안 함

	const std::string First = FSceneSerializer::ToJsonString(Scene);
	FScene            Loaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded, First));
	const std::string Second = FSceneSerializer::ToJsonString(Loaded);
	E_EXPECT_TRUE(First == Second);
	FScene Reloaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Reloaded, Second));
	E_EXPECT_TRUE(FSceneSerializer::ToJsonString(Reloaded) == First);
}

E_TEST(Prefab_EditThenUndoRestoresExactly)
{
	// 에디터 커밋 순서: 오버라이드 기록 → 스냅샷. 되돌리기: 이전 스냅샷 복원(로드 시 원본에 맞춤)
	const std::string Asset = MakeCratePrefab("Undo.eprefab");
	FScene            Scene;
	FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	FPrefabLibrary::Get().RecordAllOverrides(Scene);
	const std::string Before = FSceneSerializer::ToJsonString(Scene);

	const FEntity Root = Scene.GetRootEntities().front();
	FEntity       Lid  = FindByName(Scene, Root, "Lid");
	Scene.GetTransform(Lid).Position                       = FVector3(1.0f, 2.0f, 3.0f);
	Scene.GetRegistry().Get<FStaticMeshComponent>(Lid).bVisible = false;
	E_EXPECT_TRUE(FPrefabLibrary::Get().RecordAllOverrides(Scene));
	const std::string After = FSceneSerializer::ToJsonString(Scene);
	E_EXPECT_FALSE(Before == After);

	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Scene, Before));
	FPrefabLibrary::Get().RecordAllOverrides(Scene);
	E_EXPECT_TRUE(FSceneSerializer::ToJsonString(Scene) == Before);
	Lid = FindByName(Scene, Scene.GetRootEntities().front(), "Lid");
	E_EXPECT_NEAR(Scene.GetTransform(Lid).Position.Z, 50.0f, 1.0e-4f);

	// 다시 실행
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Scene, After));
	E_EXPECT_TRUE(FSceneSerializer::ToJsonString(Scene) == After);
}

E_TEST(Prefab_PlayModeCloneKeepsInstanceAndRemapsLinks)
{
	const std::string Asset = MakeCratePrefab("Play.eprefab");
	FScene            Scene;
	const FEntity     Root = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	Scene.CreateEntity("Other"); // 엔티티 인덱스를 어긋나게

	FScene                   Play;
	FSceneCloner::FEntityMap Map;
	FSceneCloner::Clone(Scene, Play, &Map);
	const FEntity PlayRoot = Map.at(Root.ToId());
	const FEntity PlayLid  = FindByName(Play, PlayRoot, "Lid");
	E_EXPECT_TRUE(PlayLid.IsValid());
	E_EXPECT_TRUE(FPrefabLibrary::IsInstanceRoot(Play, PlayRoot));
	E_EXPECT_TRUE(Play.GetRegistry().Get<FPrefabLinkComponent>(PlayLid).Root == PlayRoot);
	E_EXPECT_TRUE(Play.GetRegistry().Get<FPrefabLinkComponent>(PlayRoot).Root == PlayRoot);
	E_EXPECT_EQ(Play.GetRegistry().Get<FPrefabInstanceComponent>(PlayRoot).Asset, Asset);
}

E_TEST(Prefab_DuplicateInstanceMakesNewInstance)
{
	const std::string Asset = MakeCratePrefab("Duplicate.eprefab");
	FScene            Scene;
	const FEntity     Root = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	const FEntity     Copy = FSceneEditOps::Duplicate(Scene, { Root }).front();
	E_EXPECT_TRUE(FPrefabLibrary::IsInstanceRoot(Scene, Copy));
	E_EXPECT_TRUE(FPrefabLibrary::FindInstanceRoot(Scene, FindByName(Scene, Copy, "Lid")) == Copy);

	// 인스턴스 일부만 복제하면 연결 없는 (인스턴스에 추가한) 엔티티
	const FEntity LidCopy = FSceneEditOps::Duplicate(Scene, { FindByName(Scene, Root, "Lid") }).front();
	E_EXPECT_FALSE(Scene.GetRegistry().Has<FPrefabLinkComponent>(LidCopy));
	E_EXPECT_FALSE(FPrefabLibrary::Get().SyncInstance(Scene, Root)); // 추가 엔티티는 동기화가 건드리지 않는다
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(LidCopy));
}

// ---------------------------------------------------------------- 원본 변경 반영

E_TEST(Prefab_ChangePropagatesExceptOverrides)
{
	const std::string Asset = MakeCratePrefab("Propagate.eprefab");
	FScene            Scene;
	const FEntity     A = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	const FEntity     B = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	Scene.GetTransform(FindByName(Scene, A, "Lid")).Position = FVector3(0.0f, 0.0f, 999.0f); // A만 오버라이드
	FPrefabLibrary::Get().RecordAllOverrides(Scene);

	EditPrefabFile(Asset, [](FScene& Prefab, FEntity Root) {
		const FEntity Lid                = FindByName(Prefab, Root, "Lid");
		Prefab.GetTransform(Lid).Position = FVector3(0.0f, 0.0f, 80.0f);
		Prefab.GetTransform(Lid).Scale    = FVector3(3.0f, 3.0f, 3.0f);
		MakeCube(Prefab, "Handle", Root, FVector3(0.0f, 60.0f, 0.0f)); // 새 엔티티
	});
	E_EXPECT_TRUE(FPrefabLibrary::Get().SyncAllInstances(Scene));

	const FEntity LidA = FindByName(Scene, A, "Lid");
	const FEntity LidB = FindByName(Scene, B, "Lid");
	E_EXPECT_NEAR(Scene.GetTransform(LidA).Position.Z, 999.0f, 1.0e-4f); // 오버라이드 유지
	E_EXPECT_NEAR(Scene.GetTransform(LidA).Scale.X, 3.0f, 1.0e-4f);      // 나머지는 반영
	E_EXPECT_NEAR(Scene.GetTransform(LidB).Position.Z, 80.0f, 1.0e-4f);
	E_EXPECT_TRUE(FindByName(Scene, A, "Handle").IsValid());
	E_EXPECT_TRUE(FindByName(Scene, B, "Handle").IsValid());
	E_EXPECT_FALSE(FPrefabLibrary::Get().RecordAllOverrides(Scene)); // 반영 후에는 새 차이가 없다

	// 프로퍼티 되돌리기 → 원본 값
	E_EXPECT_TRUE(FPrefabLibrary::Get().RevertProperty(Scene, LidA, "TransformComponent.Position"));
	E_EXPECT_NEAR(Scene.GetTransform(LidA).Position.Z, 80.0f, 1.0e-4f);
}

E_TEST(Prefab_OverridePersistsWhenPrefabChangedWhileSceneClosed)
{
	const std::string Asset = MakeCratePrefab("Closed.eprefab");
	std::string       SavedScene;
	{
		FScene        Scene;
		const FEntity Root = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
		Scene.GetTransform(FindByName(Scene, Root, "Lid")).Position = FVector3(0.0f, 0.0f, 7.0f);
		FPrefabLibrary::Get().RecordAllOverrides(Scene);
		SavedScene = FSceneSerializer::ToJsonString(Scene);
	}
	// 씬이 닫힌 동안 원본의 Position(오버라이드된 값)과 Scale(아닌 값)이 바뀐다
	EditPrefabFile(Asset, [](FScene& Prefab, FEntity Root) {
		const FEntity Lid                = FindByName(Prefab, Root, "Lid");
		Prefab.GetTransform(Lid).Position = FVector3(0.0f, 0.0f, 123.0f);
		Prefab.GetTransform(Lid).Scale    = FVector3(4.0f, 4.0f, 4.0f);
	});

	FScene Scene;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Scene, SavedScene));
	const FEntity Lid = FindByName(Scene, Scene.GetRootEntities().front(), "Lid");
	E_EXPECT_NEAR(Scene.GetTransform(Lid).Position.Z, 7.0f, 1.0e-4f); // 사용자 오버라이드
	E_EXPECT_NEAR(Scene.GetTransform(Lid).Scale.X, 4.0f, 1.0e-4f);    // 원본 변경
}

E_TEST(Prefab_AddedComponentAndRemovedEntitySurviveSyncAndRevertAll)
{
	const std::string Asset = MakeCratePrefab("AddRemove.eprefab");
	FScene            Scene;
	const FEntity     Root = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	Scene.GetRegistry().Emplace<FCameraComponent>(Root);
	Scene.DestroyEntity(FindByName(Scene, Root, "Lid"));
	const FEntity Extra = MakeCube(Scene, "Extra", Root, FVector3::ZeroVector);
	E_EXPECT_TRUE(FPrefabLibrary::Get().RecordAllOverrides(Scene));
	const FPrefabOverrides Overrides = FPrefabOverrides::Parse(Scene.GetRegistry().Get<FPrefabInstanceComponent>(Root).Overrides);
	E_EXPECT_EQ(Overrides.RemovedEntities.size(), static_cast<size_t>(1));

	FPrefabLibrary::Get().Invalidate();
	FPrefabLibrary::Get().SyncAllInstances(Scene);
	E_EXPECT_TRUE(Scene.GetRegistry().Has<FCameraComponent>(Root));
	E_EXPECT_FALSE(FindByName(Scene, Root, "Lid").IsValid());
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(Extra));

	E_EXPECT_TRUE(FPrefabLibrary::Get().RevertAll(Scene, Root));
	E_EXPECT_FALSE(Scene.GetRegistry().Has<FCameraComponent>(Root));
	E_EXPECT_TRUE(FindByName(Scene, Root, "Lid").IsValid());
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(Extra));
	E_EXPECT_FALSE(FPrefabLibrary::Get().RecordAllOverrides(Scene));
}

E_TEST(Prefab_ApplyToPrefabUpdatesOtherInstances)
{
	const std::string Asset = MakeCratePrefab("Apply.eprefab");
	FScene            Scene;
	const FEntity     A = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	const FEntity     B = FPrefabLibrary::Get().Instantiate(Scene, Asset, NullEntity);
	Scene.GetTransform(A).Position                                     = FVector3(500.0f, 0.0f, 0.0f);
	Scene.GetRegistry().Get<FStaticMeshComponent>(FindByName(Scene, A, "Lid")).MaterialAsset = "Materials/Red.emat";
	MakeCube(Scene, "Added", A, FVector3::ZeroVector);
	std::string Error;
	E_EXPECT_TRUE(FPrefabLibrary::Get().ApplyToPrefab(Scene, A, &Error));

	E_EXPECT_TRUE(Scene.GetRegistry().Get<FPrefabInstanceComponent>(A).Overrides.empty());
	E_EXPECT_EQ(Scene.GetRegistry().Get<FStaticMeshComponent>(FindByName(Scene, B, "Lid")).MaterialAsset, std::string("Materials/Red.emat"));
	E_EXPECT_TRUE(FindByName(Scene, B, "Added").IsValid());
	E_EXPECT_NEAR(Scene.GetTransform(B).Position.X, 0.0f, 1.0e-4f); // 루트 위치는 적용하지 않는다
	E_EXPECT_TRUE(FPrefabLibrary::FindInstanceRoot(Scene, FindByName(Scene, A, "Added")) == A); // 추가한 엔티티도 이제 원본 소속
	E_EXPECT_FALSE(FPrefabLibrary::Get().RecordAllOverrides(Scene));
}

// ---------------------------------------------------------------- 중첩

E_TEST(Prefab_NestedInstantiatePropagatesAndRefusesCycles)
{
	const std::string Inner = MakeCratePrefab("Inner.eprefab");

	// Outer = 루트 + Inner 인스턴스
	const fs::path OuterFile = GetContent() / L"Prefabs/Outer.eprefab";
	{
		FScene        Scene;
		const FEntity Root   = Scene.CreateEntity("Cart");
		const FEntity Nested = FPrefabLibrary::Get().Instantiate(Scene, Inner, Root);
		Scene.GetTransform(Nested).Position = FVector3(0.0f, 30.0f, 0.0f);
		std::string Error;
		E_EXPECT_TRUE(FPrefabLibrary::Get().CreatePrefab(Scene, Root, OuterFile, &Error));
		E_EXPECT_FALSE(FPrefabLibrary::IsInstanceRoot(Scene, Nested)); // 이제 바깥 인스턴스의 중첩
	}
	const std::string Outer = FPrefabLibrary::Get().MakeAssetPath(OuterFile);
	E_EXPECT_TRUE(Contains(ReadText(OuterFile), "\"2/2\"")); // 안쪽 Lid = "중첩 루트 ID/안쪽 ID"

	FScene        Scene;
	const FEntity Root   = FPrefabLibrary::Get().Instantiate(Scene, Outer, NullEntity);
	const FEntity Nested = FindByName(Scene, Root, "Crate");
	E_EXPECT_EQ(CountSubtree(Scene, Root), static_cast<size_t>(3));
	E_EXPECT_NEAR(Scene.GetTransform(Nested).Position.Y, 30.0f, 1.0e-4f);
	E_EXPECT_TRUE(FPrefabLibrary::FindInstanceRoot(Scene, FindByName(Scene, Root, "Lid")) == Root);

	// 바깥 인스턴스에서 안쪽 엔티티 오버라이드 (바깥 Id 공간 "2/2")
	Scene.GetTransform(FindByName(Scene, Root, "Lid")).Scale = FVector3(9.0f, 9.0f, 9.0f);
	FPrefabLibrary::Get().RecordAllOverrides(Scene);
	E_EXPECT_TRUE(FPrefabOverrides::Parse(Scene.GetRegistry().Get<FPrefabInstanceComponent>(Root).Overrides).IsPropertyOverridden("2/2", "TransformComponent.Scale"));

	// 안쪽 원본 변경 → 바깥 원본을 거쳐 인스턴스까지 (오버라이드 제외)
	EditPrefabFile(Inner, [](FScene& Prefab, FEntity PrefabRoot) {
		const FEntity Lid                = FindByName(Prefab, PrefabRoot, "Lid");
		Prefab.GetTransform(Lid).Position = FVector3(0.0f, 0.0f, 77.0f);
		Prefab.GetTransform(Lid).Scale    = FVector3(5.0f, 5.0f, 5.0f);
		MakeCube(Prefab, "Bolt", PrefabRoot, FVector3::ZeroVector);
	});
	FPrefabLibrary::Get().SyncAllInstances(Scene);
	const FEntity Lid = FindByName(Scene, Root, "Lid");
	E_EXPECT_NEAR(Scene.GetTransform(Lid).Position.Z, 77.0f, 1.0e-4f);
	E_EXPECT_NEAR(Scene.GetTransform(Lid).Scale.X, 9.0f, 1.0e-4f);
	E_EXPECT_TRUE(FindByName(Scene, Root, "Bolt").IsValid());

	// 순환: Inner 안에 Outer를 넣고 저장하면 거부
	E_EXPECT_TRUE(FPrefabLibrary::Get().DependsOn(Outer, Inner));
	E_EXPECT_FALSE(FPrefabLibrary::Get().DependsOn(Inner, Outer));
	{
		FScene        Prefab;
		uint32        NextId = 0;
		const FEntity PrefabRoot = FPrefabLibrary::Get().LoadAssetInto(Prefab, Inner, NullEntity, NextId);
		FPrefabLibrary::Get().Instantiate(Prefab, Outer, PrefabRoot);
		const std::string Before = ReadText(FPrefabLibrary::Get().ResolveAssetPath(Inner));
		std::string       Error;
		E_EXPECT_FALSE(FPrefabLibrary::Get().SaveAsset(Prefab, PrefabRoot, FPrefabLibrary::Get().ResolveAssetPath(Inner), NextId, &Error));
		E_EXPECT_FALSE(Error.empty());
		E_EXPECT_TRUE(ReadText(FPrefabLibrary::Get().ResolveAssetPath(Inner)) == Before);
	}

	// 손으로 만든 순환 파일도 로드는 멈추지 않는다
	WriteText(GetContent() / L"Prefabs/LoopA.eprefab",
	          R"({ "Version": 1, "NextId": 3, "Entities": [ { "Name": "A", "Parent": -1, "Components": {} },
	             { "Name": "B", "Parent": 0, "Components": { "PrefabInstanceComponent": { "Asset": "Prefabs/LoopA.eprefab", "Overrides": "" } } } ] })");
	FScene Loop;
	E_EXPECT_TRUE(FPrefabLibrary::Get().Instantiate(Loop, "Prefabs/LoopA.eprefab", NullEntity).IsValid());
}

// ---------------------------------------------------------------- 참조 갱신

E_TEST(Prefab_ReferenceUpdateOnMove)
{
	const fs::path Content = GetContent();
	WriteText(Content / L"RefTest/Mat.emat", R"({ "BaseColorTexture": "" })");
	WriteText(Content / L"RefTest/Prop.eprefab",
	          R"({ "Version": 1, "NextId": 2, "Entities": [ { "Name": "P", "Parent": -1, "Components": { "PrefabLinkComponent": { "Id": "1", "Root": -1 },
	             "StaticMeshComponent": { "MeshAsset": "primitive:cube", "MaterialAsset": "RefTest/Mat.emat", "Visible": true } } } ] })");
	WriteText(Content / L"RefTest/Level.escene",
	          R"({ "Version": 1, "Entities": [ { "Name": "P", "Parent": -1, "Components": {
	             "PrefabInstanceComponent": { "Asset": "RefTest/Prop.eprefab", "Overrides": "" },
	             "ScriptComponent": { "ScriptAsset": "Spawner.lua", "PropertyOverrides": "{\"Bullet\":{\"Asset\":\"RefTest/Prop.eprefab\"}}" } } } ] })");

	// 프리팹 안에서 참조하는 머티리얼 이동 → 프리팹 파일 갱신 (Content 기준)
	fs::create_directories(Content / L"RefTest/Moved");
	fs::rename(Content / L"RefTest/Mat.emat", Content / L"RefTest/Moved/Mat.emat");
	FAssetReferenceUpdater::UpdateAfterMove(Content, {}, { { Content / L"RefTest/Mat.emat", Content / L"RefTest/Moved/Mat.emat" } });
	E_EXPECT_TRUE(Contains(ReadText(Content / L"RefTest/Prop.eprefab"), "\"RefTest/Moved/Mat.emat\""));

	// 프리팹 이름 변경 → 씬의 인스턴스 경로와 스크립트 오버라이드 안(JSON 안의 JSON) 경로 갱신
	fs::rename(Content / L"RefTest/Prop.eprefab", Content / L"RefTest/Crate.eprefab");
	const std::vector<FAssetMove> Moves = { { Content / L"RefTest/Prop.eprefab", Content / L"RefTest/Crate.eprefab" } };
	FAssetReferenceUpdater::UpdateAfterMove(Content, {}, Moves);
	const std::string Level = ReadText(Content / L"RefTest/Level.escene");
	E_EXPECT_TRUE(Contains(Level, "\"Asset\": \"RefTest/Crate.eprefab\""));
	E_EXPECT_TRUE(Contains(Level, "\\\"RefTest/Crate.eprefab\\\""));
	E_EXPECT_FALSE(Contains(Level, "Prop.eprefab"));

	// 열린 씬 문자열/스냅샷 갱신 경로도 JSON 안 JSON을 고친다
	const auto Remapped = FAssetReferenceUpdater::RemapContentPath("{\"Bullet\":{\"Asset\":\"RefTest/Prop.eprefab\"}}", Content, Moves);
	E_EXPECT_TRUE(Remapped.has_value() && Contains(*Remapped, "RefTest/Crate.eprefab"));
	std::string Snapshot = R"({ "Entities": [ { "Components": { "ScriptComponent": { "PropertyOverrides": "{\"Bullet\":{\"Asset\":\"RefTest/Prop.eprefab\"}}" } } } ] })";
	E_EXPECT_EQ(FAssetReferenceUpdater::RemapSceneJson(Snapshot, Content, Moves), 1u);
	E_EXPECT_TRUE(Contains(Snapshot, "RefTest/Crate.eprefab"));
}
