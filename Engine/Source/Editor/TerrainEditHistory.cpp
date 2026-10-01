#include "Editor/TerrainEditHistory.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

uint32 FTerrainEditHistory::MakeRevision()
{
	// 시간 + 카운터 해시: 같은 세션에서 겹치지 않고, 다른 세션의 저장 버전과도 겹칠 일이 거의 없다
	const uint64 Ticks = static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count());
	uint32       Value = static_cast<uint32>(Ticks ^ (Ticks >> 32)) ^ (++Counter * 0x9E3779B9u);
	Value ^= Value >> 16;
	Value *= 0x7FEB352Du;
	Value ^= Value >> 15;
	return Value == 0 ? 1u : Value;
}

void FTerrainEditHistory::Record(const std::string& Asset, FTerrainData& Data, uint32 NewRevision, FTerrainRegion Before, FTerrainRegion After)
{
	FRecord Record;
	Record.Asset  = Asset;
	Record.Parent = Data.Revision;
	Record.Child  = NewRevision;
	Record.Order  = NextOrder++;
	MemoryBytes += Before.GetMemorySize() + After.GetMemorySize();
	Record.Before = std::move(Before);
	Record.After  = std::move(After);
	Records.push_back(std::move(Record));
	Data.Revision = NewRevision;
	TrimMemory();
}

const FTerrainEditHistory::FRecord* FTerrainEditHistory::FindByChild(const std::string& Asset, uint32 Child) const
{
	for (const FRecord& Record : Records)
	{
		if (Record.Child == Child && Record.Asset == Asset)
		{
			return &Record;
		}
	}
	return nullptr;
}

bool FTerrainEditHistory::Reconcile(const std::string& Asset, FTerrainData& Data, uint32 Target)
{
	if (Data.Revision == Target)
	{
		return true;
	}
	// 현재 버전의 조상 사슬
	std::vector<uint32>        CurrentChain;
	std::unordered_set<uint32> CurrentSet;
	for (uint32 Revision = Data.Revision; CurrentChain.size() <= Records.size();)
	{
		CurrentChain.push_back(Revision);
		CurrentSet.insert(Revision);
		const FRecord* Record = FindByChild(Asset, Revision);
		if (Record == nullptr)
		{
			break;
		}
		Revision = Record->Parent;
	}
	// 목표에서 올라가며 공통 조상 찾기
	std::vector<const FRecord*> RedoPath;
	uint32                      Common = Target;
	bool                        bFound = CurrentSet.contains(Target);
	while (!bFound && RedoPath.size() <= Records.size())
	{
		const FRecord* Record = FindByChild(Asset, Common);
		if (Record == nullptr)
		{
			return false;
		}
		RedoPath.push_back(Record);
		Common = Record->Parent;
		bFound = CurrentSet.contains(Common);
	}
	if (!bFound)
	{
		return false;
	}

	FTerrainRect Changed;
	// 되감기: 현재 → 공통 조상
	for (const uint32 Revision : CurrentChain)
	{
		if (Revision == Common)
		{
			break;
		}
		const FRecord* Record = FindByChild(Asset, Revision);
		Record->Before.Apply(Data);
		Changed.Add(Record->Before.Rect);
	}
	// 다시 따라가기: 공통 조상 → 목표
	for (auto It = RedoPath.rbegin(); It != RedoPath.rend(); ++It)
	{
		(*It)->After.Apply(Data);
		Changed.Add((*It)->After.Rect);
	}
	Data.Revision = Target;
	Data.MarkChanged(Changed);
	return true;
}

void FTerrainEditHistory::Clear()
{
	Records.clear();
	MemoryBytes = 0;
}

void FTerrainEditHistory::TrimMemory()
{
	// 오래된 레코드부터 버린다 (그보다 앞 버전으로는 되돌릴 수 없게 된다)
	while (MemoryBytes > MaxBytes && Records.size() > 1)
	{
		const auto Oldest = std::min_element(Records.begin(), Records.end(), [](const FRecord& A, const FRecord& B) { return A.Order < B.Order; });
		MemoryBytes -= Oldest->Before.GetMemorySize() + Oldest->After.GetMemorySize();
		Records.erase(Oldest);
	}
}
