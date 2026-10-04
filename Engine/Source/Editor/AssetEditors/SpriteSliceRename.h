#pragma once

#include "Editor/AssetEditors/Sprite2DEditing.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

struct FEditorContext;

// 스프라이트 슬라이스 이름 변경 전파 (Phase 56 2D 에디터 후속): 아틀라스(.esprite)를 저장할 때 바뀐 슬라이스 이름을 그 아틀라스를 쓰는 곳에 반영한다.
//   - 대상: Content 아래 모든 .eflipbook(Sprite가 이 아틀라스인 것의 Frames[].Slice), .eprefab/.escene(SpriteComponent.Sprite가 이 아틀라스인 것의 Slice)
//     — 텍스트 치환은 순수 함수 Sprite2DEditing::RenameSliceRefsIn*(테스트 Sprite2DEditing_SliceRename*)
//   - 프리팹 파일은 Context.ChangePrefab 안에서 쓴다(오버라이드 기록 → 쓰기 → 인스턴스 동기화 — 프리팹 규칙)
//   - 열린 씬: 파일은 쓰지 않고 메모리의 SpriteComponent만 고친 뒤 MarkEdited (Undo 한 단계 — 저장은 사용자가). 플레이 중이면 열린 씬은 건너뛴다
//   - 플립북 파일은 FSprite2DLibrary::Invalidate로 다시 읽게 한다 (열린 플립북 편집기는 디스크 감시로 다시 읽거나 알림 띠)
//   - 바꾼 파일 목록은 로그 + 알림
namespace SpriteSliceRename
{
	struct FResult
	{
		std::vector<std::filesystem::path> ChangedFiles;
		int32                              ChangedReferences   = 0; // 파일 안 프레임/컴포넌트 수
		int32                              OpenSceneComponents = 0; // 열린 씬 메모리에서 고친 컴포넌트 수
		bool                               bSkippedOpenScene   = false; // 플레이 중이라 열린 씬을 고치지 않음
	};

	// AtlasPath = Content 기준 .esprite 경로
	FResult Propagate(FEditorContext& Context, const std::string& AtlasPath, std::span<const Sprite2DEditing::FSliceRename> Renames);
	// 결과 요약 (로그·알림 공용)
	std::string Describe(const FResult& Result);
} // namespace SpriteSliceRename
