#pragma once

#include "Core/ECS/Entity.h"

class FCamera;
class FScene;

// 씬의 카메라 컴포넌트(FCameraComponent) → 렌더용 FCamera
struct FSceneCamera
{
	// bPrimary인 첫 카메라 엔티티 (풀 순서). 없으면 NullEntity
	static FEntity FindPrimary(FScene& Scene);

	// 엔티티의 월드 위치/회전(스케일 무시)과 컴포넌트의 FOV/클립 거리를 OutCamera에 적용. 종횡비는 호출자가 지정
	static bool ApplyToCamera(FScene& Scene, FEntity CameraEntity, float AspectRatio, FCamera& OutCamera);
};
