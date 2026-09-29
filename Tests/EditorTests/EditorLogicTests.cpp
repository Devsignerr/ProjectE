#include "Core/Testing/TestFramework.h"
#include "Editor/EditorCameraState.h"
#include "Editor/EntitySelection.h"
#include "Editor/ModelTemplateCache.h"
#include "Editor/SceneEditOps.h"
#include "Editor/SnapSettings.h"
#include "Editor/UndoHistory.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <set>

namespace
{
	constexpr float Tol = 1.0e-3f;

	FEntity MakeEntity(uint32 Index) { return FEntity{ Index, 0 }; }

	const std::string& NameOf(FScene& Scene, FEntity Entity) { return Scene.GetRegistry().Get<FNameComponent>(Entity).Name; }
} // namespace

// ---------------------------------------------------------------- 선택 집합

E_TEST(Selection_SetAddToggleKeepsPrimaryLast)
{
	FEntitySelection Selection;
	Selection.Set(MakeEntity(1));
	E_EXPECT_EQ(Selection.Num(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Selection.GetPrimary() == MakeEntity(1));

	Selection.Add(MakeEntity(2));
	Selection.Add(MakeEntity(3));
	E_EXPECT_TRUE(Selection.GetPrimary() == MakeEntity(3));

	// 이미 있는 요소 추가 → 중복 없이 주 선택으로
	Selection.Add(MakeEntity(1));
	E_EXPECT_EQ(Selection.Num(), static_cast<size_t>(3));
	E_EXPECT_TRUE(Selection.GetPrimary() == MakeEntity(1));

	// 토글 제거 → 주 선택은 남은 마지막
	Selection.Toggle(MakeEntity(1));
	E_EXPECT_FALSE(Selection.Contains(MakeEntity(1)));
	E_EXPECT_TRUE(Selection.GetPrimary() == MakeEntity(3));
	Selection.Toggle(MakeEntity(4));
	E_EXPECT_TRUE(Selection.GetPrimary() == MakeEntity(4));

	Selection.Set(NullEntity);
	E_EXPECT_TRUE(Selection.IsEmpty());
	E_EXPECT_FALSE(Selection.GetPrimary().IsValid());
}

E_TEST(Selection_RangeAndRemoveIf)
{
	const std::vector<FEntity> Order = { MakeEntity(5), MakeEntity(6), MakeEntity(7), MakeEntity(8) };

	const std::vector<FEntity> Forward = FEntitySelection::GetRange(Order, MakeEntity(6), MakeEntity(8));
	E_EXPECT_EQ(Forward.size(), static_cast<size_t>(3));
	E_EXPECT_TRUE(Forward.front() == MakeEntity(6));
	const std::vector<FEntity> Backward = FEntitySelection::GetRange(Order, MakeEntity(8), MakeEntity(5));
	E_EXPECT_EQ(Backward.size(), static_cast<size_t>(4));
	// 앵커가 없으면 대상만, 대상이 없으면 빈 범위
	E_EXPECT_EQ(FEntitySelection::GetRange(Order, MakeEntity(99), MakeEntity(7)).size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(FEntitySelection::GetRange(Order, MakeEntity(5), MakeEntity(99)).empty());

	FEntitySelection Selection;
	Selection.SetMany(Forward, MakeEntity(7));
	E_EXPECT_TRUE(Selection.GetPrimary() == MakeEntity(7));
	Selection.RemoveIf([](FEntity Entity) { return Entity.Index % 2 == 0; });
	E_EXPECT_EQ(Selection.Num(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Selection.GetPrimary() == MakeEntity(7));
}

// ---------------------------------------------------------------- Undo 기록

E_TEST(Undo_CommitUndoRedo)
{
	FUndoHistory History;
	History.Reset("A");
	E_EXPECT_FALSE(History.CanUndo());
	E_EXPECT_FALSE(History.IsDirty());

	E_EXPECT_TRUE(History.Commit("이동", "B"));
	E_EXPECT_TRUE(History.Commit("회전", "C"));
	E_EXPECT_TRUE(History.IsDirty());
	E_EXPECT_TRUE(History.GetUndoLabel() == "회전");

	const std::string* State = History.Undo();
	E_EXPECT_TRUE(State != nullptr && *State == "B");
	E_EXPECT_TRUE(History.GetRedoLabel() == "회전");
	E_EXPECT_TRUE(History.GetUndoLabel() == "이동");
	State = History.Redo();
	E_EXPECT_TRUE(State != nullptr && *State == "C");
	E_EXPECT_TRUE(History.Redo() == nullptr);
}

E_TEST(Undo_NoChangeIsIgnoredAndCommitTruncatesRedo)
{
	FUndoHistory History;
	History.Reset("A");
	E_EXPECT_FALSE(History.Commit("편집", "A")); // 변화 없음
	E_EXPECT_EQ(History.GetUndoCount(), static_cast<size_t>(0));

	History.Commit("1", "B");
	History.Commit("2", "C");
	History.Undo();
	History.Undo();
	E_EXPECT_EQ(History.GetRedoCount(), static_cast<size_t>(2));
	History.Commit("3", "D");
	E_EXPECT_EQ(History.GetRedoCount(), static_cast<size_t>(0));
	E_EXPECT_EQ(History.GetUndoCount(), static_cast<size_t>(1));
	E_EXPECT_TRUE(History.GetCurrentState() == "D");
}

E_TEST(Undo_LimitDropsOldest)
{
	FUndoHistory History(3);
	History.Reset("S0");
	for (int32 Index = 1; Index <= 5; ++Index)
	{
		History.Commit(std::to_string(Index), "S" + std::to_string(Index));
	}
	E_EXPECT_EQ(History.GetUndoCount(), static_cast<size_t>(3));
	History.Undo();
	History.Undo();
	const std::string* Oldest = History.Undo();
	E_EXPECT_TRUE(Oldest != nullptr && *Oldest == "S2");
	E_EXPECT_FALSE(History.CanUndo());
}

E_TEST(Undo_DirtyTracksSavedPoint)
{
	FUndoHistory History;
	History.Reset("A");
	History.Commit("1", "B");
	History.MarkSaved();
	E_EXPECT_FALSE(History.IsDirty());
	History.Undo();
	E_EXPECT_TRUE(History.IsDirty());
	History.Redo();
	E_EXPECT_FALSE(History.IsDirty());

	// 저장 지점이 Redo 꼬리에 있을 때 새 커밋 → 다시는 저장 상태가 아니다
	History.Undo();
	History.Commit("2", "C");
	E_EXPECT_TRUE(History.IsDirty());
	History.Undo();
	E_EXPECT_TRUE(History.IsDirty());
}

E_TEST(Undo_PendingEditMergesUntilInteractionEnds)
{
	FPendingEdit Pending;
	std::string  Label;
	E_EXPECT_FALSE(Pending.TryTake(false, Label));

	// 드래그 중 여러 번 알림 → 조작 중에는 커밋하지 않고, 끝나면 첫 라벨로 한 번
	Pending.Mark("위치 편집");
	Pending.Mark("회전 편집");
	E_EXPECT_FALSE(Pending.TryTake(true, Label));
	E_EXPECT_TRUE(Pending.TryTake(false, Label));
	E_EXPECT_TRUE(Label == "위치 편집");
	E_EXPECT_FALSE(Pending.IsPending());
	E_EXPECT_FALSE(Pending.TryTake(false, Label));
}

// ---------------------------------------------------------------- 복제 / 이름

E_TEST(SceneEditOps_MakeUniqueName)
{
	std::set<std::string> Used = { "Cube", "Cube1", "Rock_07", "Rock_08" };
	const auto IsUsed = [&Used](const std::string& Name) { return Used.contains(Name); };

	E_EXPECT_TRUE(FSceneEditOps::MakeUniqueName("Cube", IsUsed) == "Cube2");
	E_EXPECT_TRUE(FSceneEditOps::MakeUniqueName("Cube1", IsUsed) == "Cube2");
	E_EXPECT_TRUE(FSceneEditOps::MakeUniqueName("Rock_07", IsUsed) == "Rock_09");
	E_EXPECT_TRUE(FSceneEditOps::MakeUniqueName("Light", IsUsed) == "Light1");
	E_EXPECT_TRUE(FSceneEditOps::MakeUniqueName("", IsUsed) == "1");
}

E_TEST(SceneEditOps_DuplicateKeepsHierarchyAndComponents)
{
	FScene        Scene;
	const FEntity Root  = Scene.CreateEntity("Parent");
	const FEntity Item  = Scene.CreateEntity("Item");
	const FEntity Child = Scene.CreateEntity("Child");
	Scene.SetParent(Item, Root);
	Scene.SetParent(Child, Item);
	Scene.GetTransform(Item).Position = FVector3(100.0f, 0.0f, 0.0f);
	Scene.GetTransform(Child).Scale   = FVector3(2.0f);
	FStaticMeshComponent& Mesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Child);
	Mesh.MeshAsset = "primitive:cube";
	Mesh.bVisible  = false;
	Scene.UpdateTransforms();

	// 자식이 함께 선택돼도 최상위만 복제된다
	const std::vector<FEntity> Clones = FSceneEditOps::Duplicate(Scene, { Child, Item });
	E_EXPECT_EQ(Clones.size(), static_cast<size_t>(1));
	const FEntity Clone = Clones[0];
	E_EXPECT_TRUE(Clone != Item);
	E_EXPECT_TRUE(NameOf(Scene, Clone) == "Item1");
	E_EXPECT_TRUE(Scene.GetParent(Clone) == Root);
	E_EXPECT_EQ(Scene.GetChildren(Root).size(), static_cast<size_t>(2));
	E_EXPECT_EQUALS(Scene.GetTransform(Clone).Position, FVector3(100.0f, 0.0f, 0.0f), Tol);

	const std::vector<FEntity>& CloneChildren = Scene.GetChildren(Clone);
	E_EXPECT_EQ(CloneChildren.size(), static_cast<size_t>(1));
	const FEntity CloneChild = CloneChildren[0];
	E_EXPECT_TRUE(CloneChild != Child);
	E_EXPECT_TRUE(NameOf(Scene, CloneChild) == "Child");
	E_EXPECT_EQUALS(Scene.GetTransform(CloneChild).Scale, FVector3(2.0f), Tol);
	const FStaticMeshComponent* CloneMesh = Scene.GetRegistry().TryGet<FStaticMeshComponent>(CloneChild);
	E_EXPECT_TRUE(CloneMesh != nullptr && CloneMesh->MeshAsset == "primitive:cube" && !CloneMesh->bVisible);
	// 원본 계층은 그대로
	E_EXPECT_TRUE(Scene.GetParent(Child) == Item);
	E_EXPECT_EQ(Scene.GetChildren(Item).size(), static_cast<size_t>(1));
}

E_TEST(SceneEditOps_DeleteTopLevel)
{
	FScene        Scene;
	const FEntity A = Scene.CreateEntity("A");
	const FEntity B = Scene.CreateEntity("B");
	const FEntity C = Scene.CreateEntity("C");
	Scene.SetParent(B, A);

	const std::vector<FEntity> TopLevel = FSceneEditOps::GetTopLevel(Scene, { B, A });
	E_EXPECT_EQ(TopLevel.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(TopLevel[0] == A);

	FSceneEditOps::Delete(Scene, { B, A });
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(A));
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(B));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(C));
}

E_TEST(EntityPath_SurvivesSnapshotRestore)
{
	FScene        Scene;
	const FEntity RootA  = Scene.CreateEntity("Group");
	const FEntity RootB  = Scene.CreateEntity("Group"); // 같은 이름 루트
	const FEntity Target = Scene.CreateEntity("Leaf");
	Scene.CreateEntity("Leaf"); // 다른 루트에 있는 같은 이름
	Scene.SetParent(Target, RootB);

	const FEntityPath Path = FEntityPath::Build(Scene, Target);
	E_EXPECT_EQ(Path.Segments.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Path.Segments[0].Occurrence, 1u);

	// 직렬화 → 복원하면 핸들은 바뀌지만 경로로 같은 엔티티를 찾는다
	const std::string Json = FSceneSerializer::ToJsonString(Scene);
	FScene            Restored;
	Restored.CreateEntity("Dummy"); // 기존 내용은 복원 시 비워진다
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Restored, Json));
	const FEntity Found = Path.Resolve(Restored);
	E_EXPECT_TRUE(Found.IsValid());
	E_EXPECT_TRUE(FEntityPath::Build(Restored, Found) == Path);
	E_EXPECT_TRUE(NameOf(Restored, Restored.GetParent(Found)) == "Group");
	const FEntityPath MissingPath{ { { "Missing", 0 } } };
	E_EXPECT_FALSE(MissingPath.Resolve(Restored).IsValid());
	(void)RootA;
}

E_TEST(ModelTemplateCache_RestoresTransientChildren)
{
	FScene        Scene;
	const FEntity Model = Scene.CreateEntity("Helmet");
	Scene.GetRegistry().Emplace<FModelComponent>(Model).AssetPath = "Models/Helmet.glb";
	const FEntity Node = Scene.CreateEntity("Node");
	Scene.SetParent(Node, Model);
	Scene.GetRegistry().Emplace<FTransientComponent>(Node);
	FStaticMeshComponent& Mesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Node);
	Mesh.Mesh = FMeshHandle{ 7, 3 };

	FModelTemplateCache Cache;
	Cache.Capture(Scene);
	E_EXPECT_EQ(Cache.Num(), static_cast<size_t>(1));

	// 스냅샷 복원: 생성된 자식은 직렬화되지 않으므로 모델 루트만 돌아온다
	const std::string Json = FSceneSerializer::ToJsonString(Scene);
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Scene, Json));
	const FEntityPath ModelPath{ { { "Helmet", 0 } } };
	const FEntity     RestoredModel = ModelPath.Resolve(Scene);
	E_EXPECT_TRUE(RestoredModel.IsValid());
	E_EXPECT_TRUE(Scene.GetChildren(RestoredModel).empty());

	E_EXPECT_EQ(Cache.Instantiate(Scene), 1u);
	const std::vector<FEntity>& Children = Scene.GetChildren(RestoredModel);
	E_EXPECT_EQ(Children.size(), static_cast<size_t>(1));
	const FEntity RestoredNode = Children[0];
	E_EXPECT_TRUE(Scene.GetRegistry().Has<FTransientComponent>(RestoredNode));
	const FStaticMeshComponent* RestoredMesh = Scene.GetRegistry().TryGet<FStaticMeshComponent>(RestoredNode);
	const FMeshHandle ExpectedMesh{ 7, 3 };
	E_EXPECT_TRUE(RestoredMesh != nullptr && RestoredMesh->Mesh == ExpectedMesh); // 같은 GPU 메시 공유
	// 이미 자식이 있으면 다시 채우지 않는다
	E_EXPECT_EQ(Cache.Instantiate(Scene), 0u);
}

// ---------------------------------------------------------------- 스냅 / 카메라

E_TEST(Snap_ValuesPerToolAndInvert)
{
	FSnapSettings Snap;
	float         Values[3] = {};
	E_EXPECT_TRUE(Snap.GetSnapValues(ETransformTool::Translate, false, Values) == nullptr);

	// Ctrl을 누르면 꺼진 스냅이 일시적으로 켜진다
	const float* Translate = Snap.GetSnapValues(ETransformTool::Translate, true, Values);
	E_EXPECT_TRUE(Translate != nullptr);
	E_EXPECT_NEAR(Translate[0], 10.0f, Tol);
	E_EXPECT_NEAR(Translate[2], 10.0f, Tol);

	Snap.bEnabled = true;
	E_EXPECT_NEAR(Snap.GetSnapValues(ETransformTool::Rotate, false, Values)[0], 15.0f, Tol);
	E_EXPECT_NEAR(Snap.GetSnapValues(ETransformTool::Scale, false, Values)[0], 0.1f, Tol);
	E_EXPECT_TRUE(Snap.GetSnapValues(ETransformTool::Rotate, true, Values) == nullptr);

	Snap.TranslateStep = 0.0f; // 잘못된 값은 스냅 안 함
	E_EXPECT_TRUE(Snap.GetSnapValues(ETransformTool::Translate, false, Values) == nullptr);
}

E_TEST(EditorCamera_JsonRoundTrip)
{
	FEditorCameraState State;
	State.Position  = FVector3(12.0f, -34.0f, 56.0f);
	State.Rotation  = FQuat::FromEuler(-20.0f, 45.0f, 0.0f);
	State.MoveSpeed = 1234.0f;

	FEditorCameraState Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(State.ToJsonString()));
	E_EXPECT_EQUALS(Loaded.Position, State.Position, Tol);
	E_EXPECT_TRUE(Loaded.Rotation.Equals(State.Rotation, 1.0e-4f));
	E_EXPECT_NEAR(Loaded.MoveSpeed, 1234.0f, Tol);

	// 잘못된 입력은 거부하고 값을 유지한다
	E_EXPECT_FALSE(Loaded.FromJsonString("not json"));
	E_EXPECT_FALSE(Loaded.FromJsonString(R"({"Position":[1,2],"Rotation":[0,0,0,1]})"));
	E_EXPECT_FALSE(Loaded.FromJsonString(R"({"Position":[1,2,3],"Rotation":[0,0,0,0]})"));
	E_EXPECT_EQUALS(Loaded.Position, State.Position, Tol);
}

E_TEST(EditorCamera_FramingKeepsDirectionAndFitsBounds)
{
	const FBox     Bounds(FVector3(-100.0f), FVector3(100.0f));
	const FVector3 Forward  = FVector3(1.0f, 1.0f, -1.0f).GetNormalized();
	const FVector3 Position = FEditorCameraState::ComputeFramingPosition(Bounds, Forward, 60.0f, 16.0f / 9.0f);

	// 중심을 정확히 바라보는 방향
	const FVector3 ToCenter = (Bounds.GetCenter() - Position).GetNormalized();
	E_EXPECT_EQUALS(ToCenter, Forward, Tol);
	// 경계 구(반지름 173.2)가 세로 시야 반각 30°에 들어가는 거리 = r / sin(30°)
	const float Distance = FVector3::Distance(Position, Bounds.GetCenter());
	E_EXPECT_NEAR(Distance, FVector3(100.0f).Length() / 0.5f, 0.5f);
}

E_TEST(SceneEditOps_DuplicateRemapsSkinAndAnimationRuntime)
{
	// 모델 루트(애니메이션) → 관절 → 스킨 메시. 복제본의 런타임 참조는 복제본 엔티티를 가리켜야 한다
	FScene        Scene;
	const FEntity Root  = Scene.CreateEntity("Model");
	const FEntity Joint = Scene.CreateEntity("Joint");
	const FEntity Skin  = Scene.CreateEntity("SkinMesh");
	Scene.SetParent(Joint, Root);
	Scene.SetParent(Skin, Root);
	FAnimationComponent& Animation   = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
	Animation.Runtime.NodeEntities   = { Root, Joint, Skin };
	Animation.Runtime.CurrentTime    = 0.5f;
	FSkinComponent& Binding          = Scene.GetRegistry().Emplace<FSkinComponent>(Skin);
	Binding.Joints                   = { Joint };
	Binding.InverseBindMatrices      = { FMatrix4x4::Identity };

	const std::vector<FEntity> Clones = FSceneEditOps::Duplicate(Scene, { Root });
	E_EXPECT_EQ(Clones.size(), static_cast<size_t>(1));
	const FEntity CloneRoot = Clones[0];
	const FEntity CloneJoint = Scene.GetChildren(CloneRoot)[0];
	const FEntity CloneSkin  = Scene.GetChildren(CloneRoot)[1];

	const FAnimationComponent& CloneAnimation = Scene.GetRegistry().Get<FAnimationComponent>(CloneRoot);
	E_EXPECT_TRUE(CloneAnimation.Runtime.NodeEntities == (std::vector<FEntity>{ CloneRoot, CloneJoint, CloneSkin }));
	E_EXPECT_NEAR(CloneAnimation.Runtime.CurrentTime, 0.5f, 0.0f);
	const FSkinComponent* CloneBinding = Scene.GetRegistry().TryGet<FSkinComponent>(CloneSkin);
	E_EXPECT_TRUE(CloneBinding != nullptr && CloneBinding->Joints == std::vector<FEntity>{ CloneJoint });
	E_EXPECT_EQ(CloneBinding ? CloneBinding->InverseBindMatrices.size() : 0, static_cast<size_t>(1));
	// 원본은 그대로
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FSkinComponent>(Skin).Joints == std::vector<FEntity>{ Joint });
}
