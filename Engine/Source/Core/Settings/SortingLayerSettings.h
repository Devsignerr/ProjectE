#pragma once

#include "Core/CoreTypes.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

// 2D 정렬 레이어 (유니티식, 프로젝트 설정 "SortingLayers" = <프로젝트>/Config/SortingLayers.json).
//   레이어 = 이름 목록, 목록 순서 = 그리기 순서 (앞 칸이 먼저 = 뒤에 그려짐, 뒤 칸이 위). 0번 "Default"는 고정(이름·위치 불변).
//   스프라이트/타일맵 컴포넌트는 레이어 "이름"을 저장한다 (순서를 바꿔도 씬이 깨지지 않게).
//   비었거나 없는 이름 = Default(칸 0). 없는 이름은 ResolveLayerChecked가 이름마다 경고를 한 번만 남긴다.
//   렌더러(Phase 56-4)의 정렬 키 = (레이어 칸, OrderInLayer, 그다음 깊이) — 칸은 ResolveLayer 결과.
// JSON: { "Layers": ["Default", "Background", "Characters", ...] }
class FSortingLayerSettings
{
public:
	static constexpr uint32      MaxLayers        = 32;
	static constexpr const char* DefaultLayerName = "Default";

	FSortingLayerSettings(); // Default만

	uint32                          GetLayerCount() const { return static_cast<uint32>(Names.size()); }
	const std::string&              GetLayerName(uint32 Index) const { return Names[Index < Names.size() ? Index : 0]; }
	const std::vector<std::string>& GetLayerNames() const { return Names; } // 칸 순서 (인스펙터 콤보)

	// 이름 → 칸 (정확히 일치, 빈 이름/없음 = -1)
	int32 FindLayer(std::string_view Name) const;
	// 이름 → 칸 (비었거나 없으면 0 = Default)
	uint32 ResolveLayer(std::string_view Name) const;
	// ResolveLayer + 없는(비지 않은) 이름이면 경고 한 번 (같은 이름은 다시 경고하지 않는다, 메인 스레드)
	uint32 ResolveLayerChecked(std::string_view Name) const;

	// ---- 편집 (설정 창). 칸 0은 바꿀 수 없다. 이름은 비지 않고 중복되지 않아야 한다 (실패 false)
	bool AddLayer(std::string Name);
	bool RenameLayer(uint32 Index, std::string Name);
	bool RemoveLayer(uint32 Index);
	bool MoveLayer(uint32 From, uint32 To); // 둘 다 1 이상

	std::string ToJson() const;
	// 실패(JSON 오류) false. 중복/빈 이름·상한 초과·첫 칸이 Default가 아님은 고쳐서 읽고 Error에 적는다 (반환 true)
	bool FromJson(std::string_view Json, std::string* Error = nullptr);

private:
	bool IsValidNewName(std::string_view Name) const;

	std::vector<std::string>      Names;
	mutable std::set<std::string> WarnedNames; // ResolveLayerChecked 경고 한 번
};
