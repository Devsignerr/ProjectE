#pragma once

#include "Core/CoreTypes.h"
#include "Scene/Terrain.h"

#include <string>
#include <unordered_map>
#include <vector>

// 지형 편집 되돌리기 기록. 씬 Undo는 씬 JSON 스냅샷만 저장하므로, 높이맵은 "편집 버전" 사슬로 맞춘다.
//   스트로크 하나 = 레코드 {에셋, 이전 버전 → 새 버전, 바뀐 영역의 전/후 복사본}
//   씬 스냅샷이 되돌아가 컴포넌트 EditRevision이 데이터 Revision과 달라지면 Reconcile이 두 버전 사이 경로(공통 조상까지
//   되감고 다시 따라감)의 레코드를 적용한다. 갈래(Undo 후 새 편집)도 부모 사슬로 찾는다.
class FTerrainEditHistory
{
public:
	static constexpr size_t DefaultMaxBytes = 256ull * 1024 * 1024;

	// 새 버전 번호 (0이 아닌 무작위에 가까운 값 — 저장된 파일 버전과 겹치지 않게)
	uint32 MakeRevision();

	// 스트로크 기록. Data.Revision을 NewRevision으로 바꾼다 (호출자는 컴포넌트 EditRevision도 같은 값으로)
	void Record(const std::string& Asset, FTerrainData& Data, uint32 NewRevision, FTerrainRegion Before, FTerrainRegion After);

	// Data를 Target 버전으로 맞춘다. 반환: 맞췄으면(또는 이미 같으면) true, 경로가 없으면 false (데이터는 그대로)
	bool Reconcile(const std::string& Asset, FTerrainData& Data, uint32 Target);

	void   Clear();
	size_t GetRecordCount() const { return Records.size(); }
	size_t GetMemorySize() const { return MemoryBytes; }
	void   SetMaxBytes(size_t Bytes) { MaxBytes = Bytes; }

private:
	struct FRecord
	{
		std::string    Asset;
		uint32         Parent = 0;
		uint32         Child  = 0;
		FTerrainRegion Before;
		FTerrainRegion After;
		uint64         Order = 0;
	};
	const FRecord* FindByChild(const std::string& Asset, uint32 Child) const;
	void           TrimMemory();

	std::vector<FRecord> Records;
	size_t               MemoryBytes = 0;
	size_t               MaxBytes    = DefaultMaxBytes;
	uint64               NextOrder   = 1;
	uint32               Counter     = 0;
};
