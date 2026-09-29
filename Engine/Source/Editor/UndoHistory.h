#pragma once

#include "Core/CoreTypes.h"

#include <string>
#include <string_view>
#include <vector>

// 스냅샷 기반 실행 취소 기록. 각 항목은 "편집 후 씬 상태"(직렬화 문자열)와 그 편집의 이름이다.
//   항목 0 = 기준 상태(열기/새 씬), Cursor = 현재 상태 항목.
//   Undo → Cursor-1 상태로, Redo → Cursor+1 상태로. 새 커밋은 Redo 꼬리를 버린다.
class FUndoHistory
{
public:
	static constexpr size_t DefaultMaxSteps = 100;

	explicit FUndoHistory(size_t InMaxSteps = DefaultMaxSteps) : MaxSteps(InMaxSteps < 1 ? 1 : InMaxSteps) {}

	// 기록을 비우고 기준 상태로 시작 (저장된 상태로 간주)
	void Reset(std::string State);

	// 편집 결과 커밋. 현재 상태와 같으면(변화 없음) 무시하고 false
	bool Commit(std::string Label, std::string State);

	bool CanUndo() const { return Cursor > 0; }
	bool CanRedo() const { return Cursor + 1 < Entries.size(); }

	// 이동 후의 상태를 반환 (불가능하면 nullptr)
	const std::string* Undo();
	const std::string* Redo();

	// 메뉴 표시용: 되돌릴/다시 할 편집 이름 (없으면 빈 문자열)
	const std::string& GetUndoLabel() const;
	const std::string& GetRedoLabel() const;

	const std::string& GetCurrentState() const;

	void MarkSaved() { SavedIndex = static_cast<int64>(Cursor); }
	bool IsDirty() const { return SavedIndex != static_cast<int64>(Cursor); }

	size_t GetUndoCount() const { return Cursor; }
	size_t GetRedoCount() const { return Entries.empty() ? 0 : Entries.size() - 1 - Cursor; }

private:
	struct FEntry
	{
		std::string Label;
		std::string State;
	};

	std::vector<FEntry> Entries;
	size_t              Cursor     = 0;
	int64               SavedIndex = 0; // -1: 저장된 상태가 기록에 없음
	size_t              MaxSteps   = DefaultMaxSteps;
};

// 편집 진행 중 알림을 모았다가, 조작이 끝난 시점에 한 번만 커밋하게 하는 게이트
// (드래그 한 번 = Undo 한 단계)
struct FPendingEdit
{
	std::string Label;

	void Mark(std::string_view InLabel)
	{
		if (Label.empty())
		{
			Label = InLabel;
		}
	}

	bool IsPending() const { return !Label.empty(); }

	// 조작이 끝났고 알림이 있으면 라벨을 꺼내 true
	bool TryTake(bool bInteractionActive, std::string& OutLabel)
	{
		if (Label.empty() || bInteractionActive)
		{
			return false;
		}
		OutLabel = std::move(Label);
		Label.clear();
		return true;
	}
};
