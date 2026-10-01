#include "Editor/FoliageEditHistory.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

size_t FFoliageEditHistory::FSnapshot::GetMemorySize() const
{
	size_t Bytes = Types.size() * sizeof(FFoliageType);
	for (const std::vector<FFoliageInstance>& List : Instances)
	{
		Bytes += List.size() * sizeof(FFoliageInstance);
	}
	return Bytes;
}

uint32 FFoliageEditHistory::MakeRevision()
{
	const uint64 Ticks = static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count());
	uint32       Value = static_cast<uint32>(Ticks ^ (Ticks >> 32)) ^ (++Counter * 0x85EBCA6Bu);
	Value ^= Value >> 16;
	Value *= 0x7FEB352Du;
	Value ^= Value >> 15;
	return Value == 0 ? 1u : Value;
}

void FFoliageEditHistory::Record(const std::string& AssetPath, FFoliageAsset& Asset, uint32 NewRevision, FSnapshot Before)
{
	FRecord Record;
	Record.Asset  = AssetPath;
	Record.Parent = Asset.Revision;
	Record.Child  = NewRevision;
	Record.Order  = NextOrder++;
	Record.After  = FSnapshot::Capture(Asset);
	Record.Before = std::move(Before);
	MemoryBytes += Record.Before.GetMemorySize() + Record.After.GetMemorySize();
	Records.push_back(std::move(Record));
	Asset.Revision = NewRevision;
	while (MemoryBytes > DefaultMaxBytes && Records.size() > 1)
	{
		const auto Oldest = std::min_element(Records.begin(), Records.end(), [](const FRecord& A, const FRecord& B) { return A.Order < B.Order; });
		MemoryBytes -= Oldest->Before.GetMemorySize() + Oldest->After.GetMemorySize();
		Records.erase(Oldest);
	}
}

const FFoliageEditHistory::FRecord* FFoliageEditHistory::FindByChild(const std::string& AssetPath, uint32 Child) const
{
	for (const FRecord& Record : Records)
	{
		if (Record.Child == Child && Record.Asset == AssetPath)
		{
			return &Record;
		}
	}
	return nullptr;
}

bool FFoliageEditHistory::Reconcile(const std::string& AssetPath, FFoliageAsset& Asset, uint32 Target)
{
	if (Asset.Revision == Target)
	{
		return true;
	}
	// 목표 상태는 목표 버전 레코드의 After(또는 공통 조상 레코드의 Before)로 바로 복원할 수 있다 (전체 복사본이라 경로를 따라갈 필요 없음).
	// 단, 목표가 기록의 처음 상태(어떤 레코드의 Parent)일 수도 있으니 둘 다 찾는다
	if (const FRecord* Record = FindByChild(AssetPath, Target))
	{
		Asset.Types     = Record->After.Types;
		Asset.Instances = Record->After.Instances;
	}
	else
	{
		const FRecord* Parent = nullptr;
		for (const FRecord& Candidate : Records)
		{
			if (Candidate.Parent == Target && Candidate.Asset == AssetPath)
			{
				Parent = &Candidate;
				break;
			}
		}
		if (Parent == nullptr)
		{
			return false;
		}
		Asset.Types     = Parent->Before.Types;
		Asset.Instances = Parent->Before.Instances;
	}
	Asset.Revision = Target;
	Asset.MarkChanged();
	return true;
}
