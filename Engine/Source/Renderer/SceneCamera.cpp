#include "Renderer/SceneCamera.h"

#include "Renderer/Camera.h"
#include "Scene/Scene.h"

FEntity FSceneCamera::FindPrimary(FScene& Scene)
{
	FEntity Found;
	Scene.GetRegistry().View<FCameraComponent, FTransformComponent>().Each(
		[&](FEntity Entity, FCameraComponent& Camera, FTransformComponent&) {
			if (!Found.IsValid() && Camera.bPrimary)
			{
				Found = Entity;
			}
		});
	return Found;
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
	OutCamera.SetPosition(Position);
	OutCamera.SetRotation(Rotation.GetNormalized());
	return true;
}
