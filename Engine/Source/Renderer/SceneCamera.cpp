#include "Renderer/SceneCamera.h"

#include "Renderer/Camera.h"
#include "Scene/CameraProjection.h"
#include "Scene/Scene.h"

FEntity FSceneCamera::FindPrimary(FScene& Scene)
{
	return FCameraProjection::FindActiveCamera(Scene); // 스크립트(Camera.WorldToScreen)와 같은 규칙
}

bool FSceneCamera::ApplyToCamera(FScene& Scene, FEntity CameraEntity, float AspectRatio, FCamera& OutCamera)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(CameraEntity))
	{
		return false;
	}
	const FCameraComponent*    Camera    = Registry.TryGet<FCameraComponent>(CameraEntity);
	const FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(CameraEntity);
	if (Camera == nullptr || Transform == nullptr)
	{
		return false;
	}

	FVector3 Position;
	FQuat    Rotation;
	FVector3 Scale;
	Transform->WorldMatrix.Decompose(Position, Rotation, Scale);

	const float NearZ = FMath::Max(Camera->NearZ, 0.01f);
	const float FarZ  = FMath::Max(Camera->FarZ, NearZ + 1.0f);
	OutCamera.SetPerspective(FMath::Clamp(Camera->FovYDegrees, 1.0f, 179.0f), AspectRatio, NearZ, FarZ);
	if (Camera->bOrthographic)
	{
		// FOV는 원근 값 그대로 남겨 둔다 (스카이박스 방향 계산에 사용)
		OutCamera.SetOrthographic(FMath::Max(Camera->OrthoHeight, 1.0f), AspectRatio, NearZ, FarZ);
	}
	OutCamera.SetPosition(Position);
	OutCamera.SetRotation(Rotation.GetNormalized());
	return true;
}
