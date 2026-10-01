#pragma once

#include "Core/CoreTypes.h"

#include <vector>

// 패스별 인스턴스 묶음 (순수 로직 — RendererTests의 InstancingTests)
//   항목 = (묶음 키, 정렬 깊이, 인스턴스 번호). 키가 같은 항목(같은 PSO·머티리얼·메시·LOD)은 DrawIndexedInstanced 한 번으로 그린다.
//   정렬: 키 → 깊이(작은 = 가까운 순). 묶음 안 인스턴스도 가까운 순이라 초기 깊이 기각에 유리하다.
//   결과: OutIndices = 묶음 순서로 이어 붙인 인스턴스 번호 (셰이더 t14), 묶음 = 그 안의 [First, First + Count) 구간
struct FInstanceSortItem
{
	uint64 Key      = 0;
	float  Depth    = 0.0f;
	uint32 Instance = 0;
};

struct FInstanceBatch
{
	uint32 First    = 0; // OutIndices 안 시작 위치 (= 셰이더 InstanceOffset)
	uint32 Count    = 0; // 인스턴스 수
	uint32 Instance = 0; // 대표 인스턴스 (메시/머티리얼/스킨 정보를 읽는다)
};

namespace InstanceBatching
{
	// 최상위 비트: 묶지 않는 항목 (스킨 메시 — 인스턴스마다 본 팔레트가 다르다). 일반 항목보다 뒤에 정렬된다
	constexpr uint64 UniqueKeyBit = 1ull << 63;

	// 일반 키: PSO 종류(3비트) | 머티리얼 슬롯(24비트) | 메시 슬롯(24비트) | LOD(4비트). 슬롯 번호는 핸들 Index (같은 프레임 안에서 유일)
	constexpr uint64 MakeKey(uint32 Pipeline, uint32 Material, uint32 Mesh, uint32 Lod)
	{
		return (static_cast<uint64>(Pipeline & 0x7u) << 52) | (static_cast<uint64>(Material & 0xFFFFFFu) << 28) |
		       (static_cast<uint64>(Mesh & 0xFFFFFFu) << 4) | static_cast<uint64>(Lod & 0xFu);
	}
	constexpr uint64 MakeUniqueKey(uint32 Instance) { return UniqueKeyBit | Instance; }
	constexpr bool   IsUniqueKey(uint64 Key) { return (Key & UniqueKeyBit) != 0; }

	// Items를 정렬하고 묶음을 만든다 (Items 순서는 바뀐다)
	void Build(std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches);
} // namespace InstanceBatching
