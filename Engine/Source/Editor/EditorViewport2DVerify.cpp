// FEditorApplication 자동 검증 중 2D 뷰포트 부분 (Phase 56 2D 에디터 후속 — EditorApplication.cpp 밖에 두어 다른 트랙과 겹치지 않게)
//   --verify-2d-create  : 콘텐츠 브라우저 드롭(.esprite/.eflipbook/.etileset) + 박스 선택 + 엔티티 메뉴 "2D" 만들기 → 컴포넌트·Y = 0 평면·Undo 한 단계
//   --verify-gizmo-2d   : ImGui 마우스 입력으로 2D 회전 기즈모 고리를 반시계로 60도 끌어 엔티티 2D 각 증가 + 화면에서 반시계 회전 확인
//   --verify-slice-rename : 임시 복사본(Content/__VerifySliceRename)으로 슬라이스 이름 변경 전파 (플립북·씬·프리팹 파일 + 열린 씬 메모리 Undo)
// 모두 씬을 저장하지 않는다. 실패는 Error 로그 (Verify.ps1 종료 코드)
#include <imgui.h>

#include "Editor/EditorApplication.h"

#include "Core/CommandLine.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/Sprite2DEditing.h"
#include "Editor/AssetEditors/SpriteSliceRename.h"
#include "Editor/Editor2D/Editor2DScene.h"
#include "Editor/SceneEditOps.h"
#include "Physics/Physics2DMath.h"
#include "Scene/Components.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <cfloat>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	struct FChecks
	{
		std::vector<std::string> Failures;
		void Check(bool bOk, const std::string& What)
		{
			if (!bOk)
			{
				Failures.push_back(What);
			}
		}
		void Report(const char* Name, const std::string& Summary) const
		{
			if (Failures.empty())
			{
				E_LOG(LogEditor, Display, "{} 검증 통과: {}", Name, Summary);
				return;
			}
			std::string Joined;
			for (const std::string& Failure : Failures)
			{
				Joined += (Joined.empty() ? "" : ", ") + Failure;
			}
			E_LOG(LogEditor, Error, "{} 검증 실패: {} ({})", Name, Joined, Summary);
		}
	};

	bool ProjectToPixel(const FMatrix4x4& ViewProjection, const FVector2& ImageSize, const FVector3& Point, FVector2& Out)
	{
		const FVector4 Clip = ViewProjection.TransformVector4(FVector4(Point, 1.0f));
		if (Clip.W <= 1.0e-4f)
		{
			return false;
		}
		Out = FVector2((Clip.X / Clip.W * 0.5f + 0.5f) * ImageSize.X, (0.5f - Clip.Y / Clip.W * 0.5f) * ImageSize.Y);
		return true;
	}

	// 엔티티 로컬 +X 축의 화면 각 (도, 화면 반시계 + — 픽셀 y는 아래로 +이므로 뒤집는다)
	float ScreenAngleOfLocalX(const FMatrix4x4& World, const FMatrix4x4& ViewProjection, const FVector2& ImageSize)
	{
		const FVector3 Origin = World.GetOrigin();
		const FVector3 AxisX  = FVector3(World.M[0][0], World.M[0][1], World.M[0][2]).GetNormalized();
		FVector2       A;
		FVector2       B;
		ProjectToPixel(ViewProjection, ImageSize, Origin, A);
		ProjectToPixel(ViewProjection, ImageSize, Origin + AxisX * 100.0f, B);
		return FMath::RadiansToDegrees(std::atan2(-(B.Y - A.Y), B.X - A.X));
	}

	float WrapDegrees(float Degrees)
	{
		while (Degrees > 180.0f)
		{
			Degrees -= 360.0f;
		}
		while (Degrees < -180.0f)
		{
			Degrees += 360.0f;
		}
		return Degrees;
	}

	bool WriteText(const std::filesystem::path& Path, const std::string& Text)
	{
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File << Text;
		return static_cast<bool>(File);
	}

	std::string ReadText(const std::filesystem::path& Path)
	{
		std::ifstream      File(Path, std::ios::binary);
		std::ostringstream Stream;
		Stream << File.rdbuf();
		return Stream.str();
	}
} // namespace

// ---------------------------------------------------------------- --verify-2d-create

void FEditorApplication::VerifyCreate2D()
{
	FChecks Checks;
	if (!ViewportPanel.Is2DMode())
	{
		ViewportPanel.Set2DMode(Context, true);
	}
	const FVector2 ImageSize = ViewportPanel.GetImageSize();
	if (ImageSize.X <= 0.0f || Context.Camera != &Camera)
	{
		E_LOG(LogEditor, Error, "2D 만들기 검증 실패: 뷰포트 렌더 타깃/편집 카메라가 없습니다");
		return;
	}
	// 2D 카메라를 원점 근처로 (Y·회전은 2D 모드가 고정)
	Camera.SetPosition(FVector3(0.0f, Camera.GetPosition().Y, 0.0f));
	const FVector3 CameraPosition = Camera.GetPosition();
	FRegistry&     Registry       = Scene.GetRegistry();

	// 1) 콘텐츠 브라우저 드롭 3개 (이미지 가운데) = 엔티티 3개, 오른쪽(+X)으로 150cm씩, Y = 0
	const size_t                       UndoBefore = UndoHistory.GetUndoCount();
	const std::vector<std::filesystem::path> Paths = {
		Context.ContentDirectory / L"Sprites/Samples/SampleAtlas.esprite",
		Context.ContentDirectory / L"Sprites/Samples/SampleGems.eflipbook",
		Context.ContentDirectory / L"Sprites/Samples/SampleTiles.etileset",
	};
	ViewportPanel.DropAssets(Context, Paths, ImageSize * 0.5f);
	CommitPendingEdit();
	const std::vector<FEntity> Dropped = Context.Selection.GetEntities();
	Checks.Check(Dropped.size() == 3, std::format("드롭 엔티티 {}개 (기대 3)", Dropped.size()));
	Checks.Check(UndoHistory.GetUndoCount() == UndoBefore + 1, "드롭 = Undo 한 단계");
	std::vector<FEntityPath> DroppedPaths;
	for (size_t Index = 0; Index < Dropped.size() && Index < 3; ++Index)
	{
		const FEntity         Entity    = Dropped[Index];
		const FVector3        Position  = Scene.GetTransform(Entity).GetWorldPosition();
		const FVector3        Expected  = FVector3(CameraPosition.X + 150.0f * static_cast<float>(Index), 0.0f, CameraPosition.Z);
		Checks.Check((Position - Expected).Length() < 0.5f, std::format("드롭 {} 위치 ({:.1f}, {:.1f}, {:.1f})", Index, Position.X, Position.Y, Position.Z));
		DroppedPaths.push_back(FEntityPath::Build(Scene, Entity));
	}
	if (Dropped.size() == 3)
	{
		const FSpriteComponent* Sprite = Registry.TryGet<FSpriteComponent>(Dropped[0]);
		Checks.Check(Sprite != nullptr && Sprite->Sprite == "Sprites/Samples/SampleAtlas.esprite" && Sprite->Slice.empty(), ".esprite → SpriteComponent(첫 슬라이스)");
		const FSpriteComponent*   FlipbookSprite = Registry.TryGet<FSpriteComponent>(Dropped[1]);
		const FFlipbookComponent* Flipbook       = Registry.TryGet<FFlipbookComponent>(Dropped[1]);
		Checks.Check(FlipbookSprite != nullptr && FlipbookSprite->Sprite == "Sprites/Samples/SampleAtlas.esprite" && Flipbook != nullptr &&
		                 Flipbook->Flipbook == "Sprites/Samples/SampleGems.eflipbook",
		             ".eflipbook → Sprite(플립북 아틀라스) + Flipbook");
		const FTilemapComponent* Tilemap = Registry.TryGet<FTilemapComponent>(Dropped[2]);
		Checks.Check(Tilemap != nullptr && Tilemap->Tileset == "Sprites/Samples/SampleTiles.etileset" && Tilemap->TileData.empty(),
		             ".etileset → 빈 Tilemap");

		// 2) 박스 선택: 드롭한 스프라이트·플립북 경계를 감싸는 사각형 → 둘 다 잡히고 빈 타일맵(경계 없음)은 아님. Ctrl(토글)이면 다시 빠짐
		Editor2DMath::FScreenRect Rect;
		bool                      bRectValid = false;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			FBox                      Bounds;
			Editor2DMath::FScreenRect Screen;
			if (Editor2DScene::AddBounds(Scene, Dropped[static_cast<size_t>(Index)], Bounds) &&
			    Editor2DMath::ProjectBoundsToScreen(Bounds, Camera.GetViewProjectionMatrix(), ImageSize, Screen))
			{
				Editor2DMath::UnionScreenRect(Rect, bRectValid, Screen);
			}
		}
		Checks.Check(bRectValid, "드롭 스프라이트 화면 경계");
		Rect.Min = Rect.Min - FVector2(4.0f, 4.0f);
		Rect.Max = Rect.Max + FVector2(4.0f, 4.0f);
		Context.ClearSelection();
		ViewportPanel.BoxSelect(Context, Rect, ImageSize, Editor2DMath::EBoxSelectMode::Replace);
		Checks.Check(Context.IsSelected(Dropped[0]) && Context.IsSelected(Dropped[1]) && !Context.IsSelected(Dropped[2]), "박스 선택 (교체)");
		ViewportPanel.BoxSelect(Context, Rect, ImageSize, Editor2DMath::EBoxSelectMode::Toggle);
		Checks.Check(!Context.IsSelected(Dropped[0]) && !Context.IsSelected(Dropped[1]), "박스 선택 (Ctrl 토글로 빠짐)");
		Checks.Check(!Context.PendingEdit.IsPending(), "선택은 Undo 단계가 아님");
	}

	// 3) Undo = 드롭 엔티티가 모두 사라짐, Redo = 돌아옴
	UndoEdit();
	bool bUndoOk = true;
	for (const FEntityPath& Path : DroppedPaths)
	{
		bUndoOk = bUndoOk && !Path.Resolve(Scene).IsValid();
	}
	Checks.Check(bUndoOk, "드롭 Undo");
	RedoEdit();
	bool bRedoOk = DroppedPaths.size() == 3;
	for (const FEntityPath& Path : DroppedPaths)
	{
		bRedoOk = bRedoOk && Path.Resolve(Scene).IsValid();
	}
	Checks.Check(bRedoOk, "드롭 Redo");

	// 4) 엔티티 메뉴 "2D" 만들기 3종 = 각각 Undo 한 단계, 스프라이트·타일맵은 편집 카메라 시선이 Y = 0에 닿는 곳, 카메라는 Y = 2000 직교
	const size_t UndoBeforeMenu = UndoHistory.GetUndoCount();
	std::vector<FEntityPath> MenuPaths;
	const Editor2DScene::ECreate2D Kinds[] = { Editor2DScene::ECreate2D::Sprite, Editor2DScene::ECreate2D::Tilemap, Editor2DScene::ECreate2D::Camera };
	for (const Editor2DScene::ECreate2D Kind : Kinds)
	{
		const FEntity Created = Editor2DScene::CreateFromMenu(Context, Kind, NullEntity);
		CommitPendingEdit();
		const FVector3 Position = Scene.GetTransform(Created).GetWorldPosition();
		switch (Kind)
		{
		case Editor2DScene::ECreate2D::Sprite:
			Checks.Check(Registry.Has<FSpriteComponent>(Created) && std::abs(Position.Y) < 0.01f, "메뉴 2D 스프라이트 (Y = 0)");
			break;
		case Editor2DScene::ECreate2D::Tilemap:
			Checks.Check(Registry.Has<FTilemapComponent>(Created) && std::abs(Position.Y) < 0.01f, "메뉴 2D 타일맵 (Y = 0)");
			break;
		case Editor2DScene::ECreate2D::Camera:
		{
			const FCameraComponent* CameraComponent = Registry.TryGet<FCameraComponent>(Created);
			Checks.Check(CameraComponent != nullptr && CameraComponent->bOrthographic && std::abs(Position.Y - 2000.0f) < 0.01f, "메뉴 2D 카메라 (직교, Y = 2000)");
			break;
		}
		}
		Checks.Check(std::abs(Position.X - CameraPosition.X) < 0.5f && std::abs(Position.Z - CameraPosition.Z) < 0.5f, "메뉴 만들기 위치 = 화면 가운데");
		Checks.Check(Context.SelectedEntity == Created, "메뉴 만들기 선택");
		MenuPaths.push_back(FEntityPath::Build(Scene, Created));
	}
	Checks.Check(UndoHistory.GetUndoCount() == UndoBeforeMenu + 3, std::format("메뉴 만들기 Undo {}단계 (기대 3)", UndoHistory.GetUndoCount() - UndoBeforeMenu));
	for (int32 Step = 0; Step < 3; ++Step)
	{
		UndoEdit();
	}
	bool bMenuUndoOk = true;
	for (const FEntityPath& Path : MenuPaths)
	{
		bMenuUndoOk = bMenuUndoOk && !Path.Resolve(Scene).IsValid();
	}
	Checks.Check(bMenuUndoOk, "메뉴 만들기 Undo 3번");

	// 화면 확인용: 드롭한 것 선택 (씬은 저장하지 않는다)
	std::vector<FEntity> Visible;
	for (const FEntityPath& Path : DroppedPaths)
	{
		if (const FEntity Entity = Path.Resolve(Scene); Entity.IsValid())
		{
			Visible.push_back(Entity);
		}
	}
	if (!Visible.empty())
	{
		Context.SelectMany(Visible, Visible.front());
	}
	Checks.Report("2D 만들기", std::format("드롭 {}개, 메뉴 3종, Undo 기록 {}", Dropped.size(), UndoHistory.GetUndoCount()));
}

// ---------------------------------------------------------------- --verify-gizmo-2d

void FEditorApplication::UpdateVerifyGizmo2D()
{
	static const bool bEnabled = FCommandLine::FromProcess().HasFlag(L"--verify-gizmo-2d");
	if (!bEnabled || VerifyGizmo.Step < 0 || GetFrameIndex() < VerifyGizmo.NextFrame)
	{
		return;
	}
	constexpr uint64 StartFrame   = 30;
	constexpr int32  MoveSteps    = 12;
	constexpr float  TargetDegree = 60.0f;
	ImGuiIO&         IO           = ImGui::GetIO();
	const FVector2   ImageSize    = ViewportPanel.GetImageSize();
	const auto       MoveMouse    = [&](const FVector2& Pixel) {
        const FVector2 Screen = ViewportPanel.GetImageMin() + Pixel;
        IO.AddMousePosEvent(Screen.X, Screen.Y);
	};
	const auto Fail = [&](const std::string& Message) {
		E_LOG(LogEditor, Error, "2D 기즈모 회전 검증 실패: {}", Message);
		VerifyGizmo.Step = -1;
	};

	switch (VerifyGizmo.Step)
	{
	case 0: // 대상 고르기 + 2D 모드 + 회전 도구 + 대상 맞춤
	{
		if (GetFrameIndex() < StartFrame)
		{
			return;
		}
		FEntity Target = Context.SelectedEntity;
		if (!Scene.GetRegistry().IsValid(Target))
		{
			Scene.GetRegistry().View<FSpriteComponent>().Each([&](FEntity Entity, FSpriteComponent&) {
				if (!Target.IsValid() && !Scene.GetParent(Entity).IsValid())
				{
					Target = Entity;
				}
			});
		}
		if (!Target.IsValid())
		{
			Fail("스프라이트 엔티티가 없습니다");
			return;
		}
		if (!ViewportPanel.Is2DMode())
		{
			ViewportPanel.Set2DMode(Context, true);
		}
		ViewportPanel.Snap.bEnabled = false;
		ViewportPanel.SetGizmoOperation(ETransformTool::Rotate);
		Context.Select(Target);
		ViewportPanel.FocusSelection(Context);
		VerifyGizmo.Target    = Target;
		VerifyGizmo.Step      = 1;
		VerifyGizmo.NextFrame = GetFrameIndex() + 3;
		return;
	}
	case 1: // 고리 반경 추정 (ImGuizmo: 화면 세로 × 0.1(클립) × 1.2 / 2) → 오른쪽 점에 마우스
	{
		if (!Scene.GetRegistry().IsValid(VerifyGizmo.Target) || ImageSize.Y <= 0.0f)
		{
			Fail("대상/뷰포트 없음");
			return;
		}
		VerifyGizmo.StartWorld = Scene.GetTransform(VerifyGizmo.Target).WorldMatrix;
		if (!ProjectToPixel(Camera.GetViewProjectionMatrix(), ImageSize, VerifyGizmo.StartWorld.GetOrigin(), VerifyGizmo.Center))
		{
			Fail("대상이 화면 밖");
			return;
		}
		VerifyGizmo.Radius     = ImageSize.Y * 0.06f;
		VerifyGizmo.ProbeIndex = 0;
		MoveMouse(VerifyGizmo.Center + FVector2(VerifyGizmo.Radius, 0.0f));
		VerifyGizmo.Step      = 2;
		VerifyGizmo.NextFrame = GetFrameIndex() + 3; // ImGuizmo::IsOver = 직전 프레임 손잡이 판정이라 2프레임 넘게 기다린다
		return;
	}
	case 2: // 고리 위인지 확인 (아니면 반경을 ±3px씩 넓혀 찾음 — 프로브마다 3프레임) → 누름
	{
		if (ViewportPanel.IsGizmoHovered())
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			VerifyGizmo.MoveIndex = 0;
			VerifyGizmo.Step      = 3;
			return;
		}
		++VerifyGizmo.ProbeIndex;
		if (VerifyGizmo.ProbeIndex > 40)
		{
			Fail(std::format("기즈모 고리를 찾지 못함 (추정 반경 {:.0f}px)", ImageSize.Y * 0.06f));
			return;
		}
		const int32 Offset = ((VerifyGizmo.ProbeIndex + 1) / 2) * 3 * ((VerifyGizmo.ProbeIndex % 2) != 0 ? 1 : -1);
		VerifyGizmo.Radius = ImageSize.Y * 0.06f + static_cast<float>(Offset);
		MoveMouse(VerifyGizmo.Center + FVector2(VerifyGizmo.Radius, 0.0f));
		VerifyGizmo.NextFrame = GetFrameIndex() + 3;
		return;
	}
	case 3: // 고리를 따라 화면 반시계로 60도 (픽셀 y는 아래로 + → 위쪽 = -sin)
	{
		++VerifyGizmo.MoveIndex;
		const float Angle = FMath::DegreesToRadians(TargetDegree * static_cast<float>(VerifyGizmo.MoveIndex) / static_cast<float>(MoveSteps));
		MoveMouse(VerifyGizmo.Center + FVector2(std::cos(Angle), -std::sin(Angle)) * VerifyGizmo.Radius);
		if (VerifyGizmo.MoveIndex >= MoveSteps)
		{
			VerifyGizmo.Step = 4;
		}
		return;
	}
	case 4:
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		VerifyGizmo.Step      = 5;
		VerifyGizmo.NextFrame = GetFrameIndex() + 3;
		return;
	case 5: // 판정: 화면 각(로컬 +X 축) 증가 = 반시계, 2D 각(Physics2DMath — 화면 반시계 +)도 같은 만큼 증가, Undo 한 단계
	{
		if (!Scene.GetRegistry().IsValid(VerifyGizmo.Target))
		{
			Fail("대상이 사라짐");
			return;
		}
		const FMatrix4x4 EndWorld    = Scene.GetTransform(VerifyGizmo.Target).WorldMatrix;
		const FMatrix4x4 ViewProj    = Camera.GetViewProjectionMatrix();
		const float      ScreenDelta = WrapDegrees(ScreenAngleOfLocalX(EndWorld, ViewProj, ImageSize) - ScreenAngleOfLocalX(VerifyGizmo.StartWorld, ViewProj, ImageSize));
		FVector3         Position;
		FQuat            StartRotation;
		FQuat            EndRotation;
		FVector3         Scale;
		VerifyGizmo.StartWorld.Decompose(Position, StartRotation, Scale);
		EndWorld.Decompose(Position, EndRotation, Scale);
		bool        bTilted    = false;
		const float AngleDelta = WrapDegrees(FMath::RadiansToDegrees(Physics2DMath::AngleFromRotation(EndRotation, &bTilted) -
		                                                             Physics2DMath::AngleFromRotation(StartRotation)));
		const bool bOk = std::abs(ScreenDelta - TargetDegree) < 4.0f && std::abs(AngleDelta - TargetDegree) < 4.0f && !bTilted;
		const std::string Summary = std::format("화면 회전 {:+.1f}도, 2D 각 {:+.1f}도 (기대 +{:.0f} — 반시계), 평면 밖 기울기 {}, 고리 반경 {:.0f}px",
		                                        ScreenDelta, AngleDelta, TargetDegree, bTilted ? "있음" : "없음", VerifyGizmo.Radius);
		if (bOk)
		{
			E_LOG(LogEditor, Display, "2D 기즈모 회전 검증 통과: {}", Summary);
		}
		else
		{
			E_LOG(LogEditor, Error, "2D 기즈모 회전 검증 실패: {}", Summary);
		}
		// 마우스를 뷰포트 밖으로 (스크린샷에 고리 강조가 남지 않게)
		IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		VerifyGizmo.Step = -1;
		return;
	}
	default:
		return;
	}
}

// ---------------------------------------------------------------- --verify-slice-rename

void FEditorApplication::VerifySliceRename()
{
	FChecks                     Checks;
	const std::filesystem::path TempDir = Context.ContentDirectory / L"__VerifySliceRename";
	std::error_code             ErrorCode;
	std::filesystem::remove_all(TempDir, ErrorCode);
	std::filesystem::create_directories(TempDir, ErrorCode);
	const std::filesystem::path Samples = Context.ContentDirectory / L"Sprites/Samples";
	std::filesystem::copy_file(Samples / L"SampleAtlas.esprite", TempDir / L"Atlas.esprite", ErrorCode);
	std::filesystem::copy_file(Samples / L"SampleAtlas.png", TempDir / L"SampleAtlas.png", ErrorCode);
	const std::string AtlasPath = "__VerifySliceRename/Atlas.esprite";

	// 플립북(같은 폴더 상대 참조) / 씬 / 프리팹 파일
	std::string FlipbookText = ReadText(Samples / L"SampleGems.eflipbook");
	if (const size_t At = FlipbookText.find("SampleAtlas.esprite"); At != std::string::npos)
	{
		FlipbookText.replace(At, std::string("SampleAtlas.esprite").size(), "Atlas.esprite");
	}
	WriteText(TempDir / L"Gems.eflipbook", FlipbookText);
	const std::string EntityJson = std::format(
		R"({{"Name":"VerifySprite","Parent":-1,"Components":{{"SpriteComponent":{{"Sprite":"{}","Slice":"Coin"}},"TransformComponent":{{"Position":[0,0,0],"Rotation":[0,0,0,1],"Scale":[1,1,1]}}}}}})",
		AtlasPath);
	WriteText(TempDir / L"Scene.escene", std::format(R"({{"Version":1,"Entities":[{}]}})", EntityJson));
	WriteText(TempDir / L"Prefab.eprefab", std::format(R"({{"Version":1,"NextId":1,"Entities":[{}]}})", EntityJson));

	// 열린 씬에 같은 아틀라스를 쓰는 엔티티 (Undo 한 단계)
	const FEntity Entity = Scene.CreateEntity("VerifySliceRename");
	FSpriteComponent& Sprite = Scene.GetRegistry().Emplace<FSpriteComponent>(Entity);
	Sprite.Sprite            = AtlasPath;
	Sprite.Slice             = "Coin";
	Context.MarkEdited("검증 엔티티");
	CommitPendingEdit();
	const FEntityPath EntityPath = FEntityPath::Build(Scene, Entity);
	const size_t      UndoBefore = UndoHistory.GetUndoCount();

	// 아틀라스 편집기 저장과 같은 경로: 저장본 대비 이름 변경 추정 → 저장 → 전파
	std::shared_ptr<const FSpriteAsset> Saved = FSprite2DLibrary::Get().LoadSprite(AtlasPath);
	Checks.Check(Saved != nullptr && Saved->FindSlice("Coin") >= 0, "임시 아틀라스 읽기");
	if (Saved != nullptr)
	{
		FSpriteAsset Current = *Saved;
		Sprite2DEditing::RenameSlice(Current, Current.FindSlice("Coin"), "VerifyCoin");
		const std::vector<Sprite2DEditing::FSliceRename> Renames = Sprite2DEditing::DetectSliceRenames(*Saved, Current);
		Checks.Check(Renames.size() == 1 && Renames[0] == (Sprite2DEditing::FSliceRename{ "Coin", "VerifyCoin" }), "이름 변경 추정");
		Checks.Check(FSprite2DLibrary::Get().SaveSprite(AtlasPath, Current), "아틀라스 저장");
		const SpriteSliceRename::FResult Result = SpriteSliceRename::Propagate(Context, AtlasPath, Renames);
		Checks.Check(Result.ChangedFiles.size() == 3, std::format("바꾼 파일 {}개 (기대 3)", Result.ChangedFiles.size()));
		Checks.Check(Result.OpenSceneComponents == 1, "열린 씬 컴포넌트 1개");

		FFlipbookAsset Flipbook;
		Checks.Check(FFlipbookAsset::FromJsonString(ReadText(TempDir / L"Gems.eflipbook"), Flipbook) && !Flipbook.Frames.empty() &&
		                 Flipbook.Frames[0].Slice == "VerifyCoin",
		             "플립북 프레임");
		const std::shared_ptr<const FFlipbookAsset> Library = FSprite2DLibrary::Get().LoadFlipbook("__VerifySliceRename/Gems.eflipbook");
		Checks.Check(Library != nullptr && !Library->Frames.empty() && Library->Frames[0].Slice == "VerifyCoin", "플립북 라이브러리 다시 읽음");
		Checks.Check(ReadText(TempDir / L"Scene.escene").find("\"VerifyCoin\"") != std::string::npos, "씬 파일");
		Checks.Check(ReadText(TempDir / L"Prefab.eprefab").find("\"VerifyCoin\"") != std::string::npos, "프리팹 파일");
		const FEntity Resolved = EntityPath.Resolve(Scene);
		Checks.Check(Resolved.IsValid() && Scene.GetRegistry().Get<FSpriteComponent>(Resolved).Slice == "VerifyCoin", "열린 씬 메모리");
		CommitPendingEdit();
		Checks.Check(UndoHistory.GetUndoCount() == UndoBefore + 1, "열린 씬 반영 = Undo 한 단계");
		UndoEdit();
		const FEntity Restored = EntityPath.Resolve(Scene);
		Checks.Check(Restored.IsValid() && Scene.GetRegistry().Get<FSpriteComponent>(Restored).Slice == "Coin", "열린 씬 Undo = 이전 이름");
	}

	// 정리: 검증 엔티티 Undo, 임시 폴더 삭제, 캐시 무효화
	UndoEdit();
	Checks.Check(!EntityPath.Resolve(Scene).IsValid(), "검증 엔티티 Undo");
	std::filesystem::remove_all(TempDir, ErrorCode);
	FSprite2DLibrary::Get().Invalidate();
	Checks.Report("슬라이스 이름 변경", std::format("임시 폴더 {}", FStringConv::ToUtf8(TempDir.filename().wstring())));
}
