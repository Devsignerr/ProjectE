#pragma once

#include "Core/CoreTypes.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// 충돌 레이어 (유니티식, 프로젝트 설정 "Collision" = <프로젝트>/Config/Collision.json).
//   레이어 = 이름이 있는 칸 (최대 16, 0번 "Default" 고정). 빈 칸 = 쓰지 않음.
//   콜라이더/캐릭터 이동 컴포넌트는 레이어 "이름"을 저장한다 (칸 순서를 바꿔도 씬이 깨지지 않게).
//   비었거나 없는 이름 = Default → 레이어를 쓰지 않던 씬은 예전과 똑같이 동작한다.
//   충돌 행렬 = 레이어 × 레이어 대칭 (기본 모두 켬). 꺼진 쌍은 서로 통과하고 트리거 알림도 없다.
//   레이캐스트는 레이어 마스크(비트 i = 칸 i)로 거른다 (기본 전체).
// JSON: { "Layers": ["Default", "Ground", ...], "DisabledPairs": [["Character", "Character"], ...] } — 칸 순서대로, 쌍은 이름
class FCollisionLayerSettings
{
public:
	static constexpr uint32 MaxLayers     = 16;
	static constexpr uint32 AllLayersMask = 0xFFFFu;
	static constexpr const char* DefaultLayerName = "Default";

	FCollisionLayerSettings(); // Default만, 모두 충돌

	// ---- 레이어 이름 (칸 0은 바꿀 수 없다). 이름을 비우거나 바꾸면 그 칸의 행렬 줄은 모두 켬으로 돌아간다
	const std::string& GetLayerName(uint32 Index) const { return Names[Index < MaxLayers ? Index : 0]; }
	bool               SetLayerName(uint32 Index, std::string Name);
	// 이름 → 칸 (정확히 일치하는 첫 칸, 빈 이름/없음 = -1)
	int32 FindLayer(std::string_view Name) const;
	// 이름 → 칸 (비었거나 없으면 0 = Default)
	uint32 ResolveLayer(std::string_view Name) const;
	// 이름이 있는 레이어 (칸 순서, 인스펙터 콤보)
	std::vector<std::string> GetLayerNames() const;

	// ---- 충돌 행렬 (대칭)
	bool   ShouldCollide(uint32 A, uint32 B) const;
	void   SetCollision(uint32 A, uint32 B, bool bCollide);
	uint16 GetCollisionMask(uint32 Layer) const { return Layer < MaxLayers ? Matrix[Layer] : static_cast<uint16>(AllLayersMask); }

	// 레이어 이름 목록 → 마스크. 없는 이름이 있으면 false + OutUnknown(첫 번째)
	bool MakeMask(const std::vector<std::string>& LayerNames, uint32& OutMask, std::string* OutUnknown = nullptr) const;

	std::string ToJson() const;
	// 실패(JSON 오류) false. 없는 레이어 이름의 쌍·중복 이름 등은 건너뛰고 Error에 적는다 (반환 true)
	bool FromJson(std::string_view Json, std::string* Error = nullptr);

private:
	std::array<std::string, MaxLayers> Names;
	std::array<uint16, MaxLayers>      Matrix; // 줄 i의 비트 j = i와 j가 부딪힌다
};
