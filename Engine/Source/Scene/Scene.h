#pragma once

#include "Core/ECS/Registry.h"
#include "Scene/Components.h"

#include <string_view>

// 씬: 레지스트리 소유 + 계층/트랜스폼 편의 기능
class FScene
{
public:
	FRegistry&       GetRegistry() { return Registry; }
	const FRegistry& GetRegistry() const { return Registry; }

	// 이름/트랜스폼/계층 컴포넌트를 갖춘 엔티티 생성
	FEntity CreateEntity(std::string_view Name = "Entity");

	// 자식까지 재귀 파괴
	void DestroyEntity(FEntity Entity);

	// Parent가 NullEntity면 루트로 만든다. 순환은 거부한다(로그 후 무시).
	void SetParent(FEntity Child, FEntity Parent);

	FEntity                     GetParent(FEntity Entity) const;
	const std::vector<FEntity>& GetChildren(FEntity Entity) const;
	bool                        IsAncestorOf(FEntity Ancestor, FEntity Entity) const;

	// 모든 엔티티의 WorldMatrix를 부모→자식 순서로 갱신
	void UpdateTransforms();

	FTransformComponent&       GetTransform(FEntity Entity) { return Registry.Get<FTransformComponent>(Entity); }
	const FTransformComponent& GetTransform(FEntity Entity) const { return Registry.Get<FTransformComponent>(Entity); }

private:
	void UpdateTransformRecursive(FEntity Entity, const FMatrix4x4& ParentWorld);

	FRegistry Registry;
};
