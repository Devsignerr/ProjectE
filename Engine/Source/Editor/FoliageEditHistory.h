#pragma once

#include "Core/CoreTypes.h"
#include "Scene/Foliage.h"

#include <string>
#include <vector>

// 폴리지 편집 되돌리기 기록 (지형 FTerrainEditHistory와 같은 버전 사슬 방식). 레코드 = 편집 전/후 타입 목록 + 인스턴스 전체 복사본.
// 컴포넌트 EditRevision이 Undo/Redo로 에셋 Revision과 달라지면 Reconcile이 공통 조상까지 되감고 목표 버전을 따라간다
class FFoliageEditHistory
{
public:
	static constexpr size_t DefaultMaxBytes = 256ull * 1024 * 1024;

	struct FSnapshot
	{
		std::vector<FFoliageType>                  Types;
		std::vector<std::vector<FFoliageInstance>> Instances;

		static FSnapshot Capture(const FFoliageAsset& Asset) { return { Asset.Types, Asset.Instances }; }
		size_t           GetMemorySize() const;
	};

	uint32 MakeRevision();
	// Asset은 이미 After 상태. Asset.Revision을 NewRevision으로
	void Record(const std::string& AssetPath, FFoliageAsset& Asset, uint32 NewRevision, FSnapshot Before);
	bool Reconcile(const std::string& AssetPath, FFoliageAsset& Asset, uint32 Target);

	size_t GetRecordCount() const { return Records.size(); }
	size_t GetMemorySize() const { return MemoryBytes; }

private:
	struct FRecord
	{
		std::string Asset;
		uint32      Parent = 0;
		uint32      Child  = 0;
		FSnapshot   Before;
		FSnapshot   After;
		uint64      Order = 0;
	};
	const FRecord* FindByChild(const std::string& AssetPath, uint32 Child) const;

	std::vector<FRecord> Records;
	size_t               MemoryBytes = 0;
	uint64               NextOrder   = 1;
	uint32               Counter     = 0;
};
