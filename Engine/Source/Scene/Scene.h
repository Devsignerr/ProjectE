#pragma once

#include "Core/ECS/Registry.h"
#include "Scene/Components.h"

#include <string_view>

// 씬: 레지스트리 소유 + 계층/트랜스폼 편의 기능
class FScene
{
public:
	FScene(); // 컴포넌트 리플렉션 등록 보장

	FRegistry&       GetRegistry() { return Registry; }
	const FRegistry& GetRegistry() const { return Registry; }

	// 이름/트랜스폼/계층 컴포넌트를 갖춘 엔티티 생성
	FEntity CreateEntity(std::string_view Name = "Entity");

	// 자식까지 재귀 파괴
	void DestroyEntity(FEntity Entity);

	// 모든 엔티티 파괴
	void Clear();

	// 루트 엔티티(부모 없음) 목록을 생성 순서(풀 순서)대로 반환
	std::vector<FEntity> GetRootEntities() const;

	// Parent가 NullEntity면 루트로 만든다. 순환은 거부한다(로그 후 무시).
	void SetParent(FEntity Child, FEntity Parent);

	FEntity                     GetParent(FEntity Entity) const;
	const std::vector<FEntity>& GetChildren(FEntity Entity) const;
	bool                        IsAncestorOf(FEntity Ancestor, FEntity Entity) const;

	// 모든 엔티티의 WorldMatrix를 부모→자식 순서로 갱신. 소켓 부착 엔티티는 대상 모델 뒤에 소켓 기준으로
	void UpdateTransforms();

	// 모델 루트의 소켓(.emeta) 월드 행렬 = 소켓 로컬 × 뼈 월드 (직전 UpdateTransforms 기준). 소켓/뼈가 없으면 false
	bool GetSocketWorldMatrix(FEntity ModelRoot, std::string_view Socket, FMatrix4x4& OutWorld) const;
	// 로컬 ↔ 월드 변환 기준 (소켓 부착이면 소켓, 아니면 계층 부모, 루트면 단위 행렬). 기즈모/물리가 로컬 값을 쓸 때 사용
	FMatrix4x4 GetParentWorldMatrix(FEntity Entity) const;
	// 유효한 대상(자기 하위가 아닌 엔티티)에 소켓 부착되어 있는지
	bool IsSocketAttached(FEntity Entity) const;

	FTransformComponent&       GetTransform(FEntity Entity) { return Registry.Get<FTransformComponent>(Entity); }
	const FTransformComponent& GetTransform(FEntity Entity) const { return Registry.Get<FTransformComponent>(Entity); }

private:
	void UpdateTransformRecursive(FEntity Entity, const FMatrix4x4& ParentWorld, bool bAllowDefer);

	FRegistry            Registry;
	std::vector<FEntity> DeferredAttachments; // UpdateTransforms 작업 목록
};
