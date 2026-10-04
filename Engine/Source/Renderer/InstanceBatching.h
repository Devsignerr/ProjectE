#pragma once

#include "Core/CoreTypes.h"

#include <vector>

// 패스별 인스턴스 묶음 (순수 로직 — RendererTests의 InstancingTests)
//   항목 = (묶음 키, 정렬 깊이, 인스턴스 번호). 키가 같은 항목(같은 PSO·머티리얼·메시·LOD)은 DrawIndexedInstanced 한 번으로 그린다.
//   정렬: 키 → 깊이(작은 = 가까운 순) → 인스턴스 번호. 묶음 안 인스턴스도 가까운 순이라 초기 깊이 기각에 유리하다.
//   구현(SortFrontToBack): 항목이 인스턴스 번호 오름차순으로 들어오면(패스들은 목록 순서로 Add한다) 키 묶음 정렬 — 고유 키만 비교 정렬해
//   키별로 안정 분배(같은 키 안 = 번호 순)하고, 깊이가 모두 같지 않으면 묶음마다 (깊이, 번호) 정렬(항목이 많으면 묶음끼리 FParallel).
//   비교 정렬과 결과가 같다. 번호 순이 아니거나 NaN 깊이면 비교 정렬. 바꾸면 InstancingTests의 비교 정렬 대조 테스트도 함께
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
	uint64 Key      = 0; // 묶음 키 (일반 키면 LOD = GetLod — 그림자 캐스케이드별 LOD처럼 인스턴스 LOD와 다를 수 있다)
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
	constexpr uint32 GetLod(uint64 Key) { return static_cast<uint32>(Key & 0xFu); }

	// 키 → 깊이 → 인스턴스 번호 순으로 정렬 (위 머리 주석, 스레드 안전 — 임시 버퍼는 스레드별)
	void SortFrontToBack(std::vector<FInstanceSortItem>& Items);
	// SortFrontToBack 순서로 이미 정렬된 목록들을 하나로 합친다 (결과 = 이어 붙여 정렬한 것과 같다. 같은 항목은 앞 목록 먼저).
	// 병렬로 조각마다 정렬한 뒤 합칠 때 (방향광 그림자 캐스터 조각)
	void MergeSortedLists(const std::vector<FInstanceSortItem>* const* Lists, uint32 ListCount, std::vector<FInstanceSortItem>& Out);
	// 이미 정렬된 Items로 묶음만 만든다
	void BuildSorted(const std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches);
	// Items를 정렬하고 묶음을 만든다 (Items 순서는 바뀐다)
	void Build(std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches);
	// 반투명 패스: 깊이 큰(먼) 것부터 정렬하고, 정렬 결과에서 바로 이웃한 같은 키끼리만 묶는다 (그리기 순서 = 뒤→앞 유지)
	void BuildBackToFront(std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches);
} // namespace InstanceBatching
