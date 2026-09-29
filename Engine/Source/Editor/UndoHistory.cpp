#include "Editor/UndoHistory.h"

namespace
{
	const std::string GEmptyString;
}

void FUndoHistory::Reset(std::string State)
{
	Entries.clear();
	Entries.push_back({ std::string(), std::move(State) });
	Cursor     = 0;
	SavedIndex = 0;
}

bool FUndoHistory::Commit(std::string Label, std::string State)
{
	if (Entries.empty())
	{
		Reset(std::move(State));
		return false;
	}
	if (Entries[Cursor].State == State)
	{
		return false;
	}

	// Redo 꼬리 제거 (저장 지점이 거기 있었다면 더는 도달할 수 없다)
	Entries.resize(Cursor + 1);
	if (SavedIndex > static_cast<int64>(Cursor))
	{
		SavedIndex = -1;
	}

	Entries.push_back({ std::move(Label), std::move(State) });
	++Cursor;

	// 상한: 기준 상태 + MaxSteps 단계까지 유지, 오래된 것부터 버린다
	while (Entries.size() > MaxSteps + 1)
	{
		Entries.erase(Entries.begin());
		--Cursor;
		SavedIndex = SavedIndex > 0 ? SavedIndex - 1 : -1;
	}
	return true;
}

const std::string* FUndoHistory::Undo()
{
	if (!CanUndo())
	{
		return nullptr;
	}
	--Cursor;
	return &Entries[Cursor].State;
}

const std::string* FUndoHistory::Redo()
{
	if (!CanRedo())
	{
		return nullptr;
	}
	++Cursor;
	return &Entries[Cursor].State;
}

const std::string& FUndoHistory::GetUndoLabel() const
{
	return CanUndo() ? Entries[Cursor].Label : GEmptyString;
}

const std::string& FUndoHistory::GetRedoLabel() const
{
	return CanRedo() ? Entries[Cursor + 1].Label : GEmptyString;
}

const std::string& FUndoHistory::GetCurrentState() const
{
	return Entries.empty() ? GEmptyString : Entries[Cursor].State;
}
