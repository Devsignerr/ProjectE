#pragma once

#include "Core/ECS/Entity.h"
#include "Scene/Scene.h"

#include <string>
#include <unordered_map>

// 스냅샷 복원(Undo/Redo) 시 모델 하위 노드를 다시 로드하지 않기 위한 캐시.
// 복원 전 씬의 모델 인스턴스(FModelComponent + 생성된 자식)를 에셋 경로별로 별도 씬에 복제해 두고,
// 복원 후 자식이 없는 모델 루트에 같은 자식 구성을 복제한다 (메시/머티리얼 핸들 공유 → GPU 리소스 추가 생성·누수 없음).
class FModelTemplateCache
{
public:
	// Scene의 모델 인스턴스 중 아직 캐시에 없는 에셋 경로를 등록한다
	void Capture(FScene& Scene);

	// 자식이 없는 모델 루트에 캐시된 하위 노드를 복제한다. 반환: 채운 모델 수
	uint32 Instantiate(FScene& Scene);

	void Clear();

	size_t Num() const { return Templates.size(); }

private:
	FScene                                   TemplateScene;
	std::unordered_map<std::string, FEntity> Templates; // 에셋 경로 → TemplateScene의 템플릿 루트
};
