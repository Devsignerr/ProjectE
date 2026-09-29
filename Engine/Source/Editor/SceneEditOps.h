#pragma once

#include "Core/ECS/Entity.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class FScene;

// 에디터 씬 편집 연산 (ImGui/GPU 비의존, 단위 테스트 대상)
struct FSceneEditOps
{
	// Source 서브트리를 DestScene의 DestParent 아래로 복제한다 (같은 씬도 가능).
	// 리플렉션에 등록된 모든 컴포넌트/프로퍼티(Transient 포함)를 복사하며, 계층은 새로 구성한다.
	// 리소스 핸들은 그대로 공유된다 (GPU 리소스 추가 생성 없음). 반환: 복제된 루트
	static FEntity CloneSubtree(FScene& SourceScene, FEntity Source, FScene& DestScene, FEntity DestParent);

	// 선택 목록에서 조상이 함께 선택된 엔티티를 뺀 최상위 목록 (삭제/복제/기즈모 델타 대상). 순서 유지
	static std::vector<FEntity> GetTopLevel(const FScene& Scene, const std::vector<FEntity>& Entities);

	// 최상위 엔티티들을 같은 부모 아래 복제하고 이름에 번호를 붙인다. 반환: 새 루트들 (입력 순서)
	static std::vector<FEntity> Duplicate(FScene& Scene, const std::vector<FEntity>& Entities);

	// 최상위 엔티티들을 (자식 포함) 삭제
	static void Delete(FScene& Scene, const std::vector<FEntity>& Entities);

	// "Cube" → "Cube1", "Cube1" → "Cube2", "Rock_07" → "Rock_08" 처럼 끝 번호를 올려 IsUsed가 false인 이름을 찾는다
	static std::string MakeUniqueName(std::string_view Name, const std::function<bool(const std::string&)>& IsUsed);
};

// 스냅샷 복원 후에도 같은 엔티티를 다시 찾기 위한 경로: 루트부터 (이름, 같은 이름 형제 중 순번)
struct FEntityPath
{
	struct FSegment
	{
		std::string Name;
		uint32      Occurrence = 0;

		bool operator==(const FSegment&) const = default;
	};
	std::vector<FSegment> Segments;

	bool IsEmpty() const { return Segments.empty(); }
	bool operator==(const FEntityPath&) const = default;

	static FEntityPath Build(const FScene& Scene, FEntity Entity);
	FEntity            Resolve(const FScene& Scene) const; // 없으면 NullEntity
};
