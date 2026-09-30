#pragma once

#include "Core/ECS/Entity.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class FScene;
struct FBox;

// 에디터 씬 편집 연산 (ImGui/GPU 비의존, 단위 테스트 대상)
struct FSceneEditOps
{
	// Source 서브트리를 DestScene의 DestParent 아래로 복제한다 (같은 씬도 가능).
	// 리플렉션에 등록된 모든 컴포넌트/프로퍼티(Transient 포함)를 복사하며, 계층은 새로 구성한다.
	// 리소스 핸들은 그대로 공유된다 (GPU 리소스 추가 생성 없음). 반환: 복제된 루트
	static FEntity CloneSubtree(FScene& SourceScene, FEntity Source, FScene& DestScene, FEntity DestParent);

	// SourceParent의 자식들을 DestParent 아래로 한 번에 복제한다 (DestParent는 기존 엔티티).
	// 형제 서브트리 사이 참조와 SourceParent의 런타임 데이터(애니메이션 노드)도 복제본 기준으로 옮긴다
	static void CloneChildren(FScene& SourceScene, FEntity SourceParent, FScene& DestScene, FEntity DestParent);

	// 선택 목록에서 조상이 함께 선택된 엔티티를 뺀 최상위 목록 (삭제/복제/기즈모 델타 대상). 순서 유지
	static std::vector<FEntity> GetTopLevel(const FScene& Scene, const std::vector<FEntity>& Entities);

	// 최상위 엔티티들을 같은 부모 아래 복제하고 이름에 번호를 붙인다. 반환: 새 루트들 (입력 순서)
	static std::vector<FEntity> Duplicate(FScene& Scene, const std::vector<FEntity>& Entities);

	// 최상위 엔티티들을 (자식 포함) 삭제
	static void Delete(FScene& Scene, const std::vector<FEntity>& Entities);

	// 복사(Ctrl+C): 최상위 엔티티들(자식 포함)을 월드 트랜스폼을 로컬 값으로 가진 루트 엔티티로 만든 씬 JSON 문자열.
	// 모델 하위 노드 등 저장하지 않는 엔티티는 빠지고 붙여넣은 뒤 에셋 해석으로 다시 생긴다. 선택이 비면 빈 문자열
	static std::string Copy(FScene& Scene, const std::vector<FEntity>& Entities);

	// 붙여넣기(Ctrl+V): Copy 결과를 Scene 루트에 만들고 이름에 번호를 붙인다. 반환: 새 루트들 (복사 순서). 형식이 틀리면 빈 목록
	static std::vector<FEntity> Paste(FScene& Scene, const std::string& Clipboard);

	// 바닥 붙이기(End): Surfaces 중 Bounds와 XY가 겹치고 윗면이 Bounds 중심 높이 이하인 것 가운데 가장 높은 윗면.
	// 없으면 false (바닥에 반쯤 묻힌 물체는 그 바닥 위로 올라온다)
	static bool FindFloorHeight(const FBox& Bounds, const std::vector<FBox>& Surfaces, float& OutHeight);

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
