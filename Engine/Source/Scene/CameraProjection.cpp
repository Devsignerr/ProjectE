#include "Scene/CameraProjection.h"

#include "Scene/Components.h"
#include "Scene/Scene.h"

FEntity FCameraProjection::FindActiveCamera(FScene& Scene)
{
	FEntity Found;
	int32   FoundPriority = 0;
	Scene.GetRegistry().View<FCameraComponent, FTransformComponent>().Each(
		[&](FEntity Entity, FCameraComponent& Camera, FTransformComponent&) {
			if (Camera.bPrimary && (!Found.IsValid() || Camera.Priority > FoundPriority))
			{
				Found         = Entity;
				FoundPriority = Camera.Priority;
			}
		});
	return Found;
}

FMatrix4x4 FCameraProjection::MakeViewProjection(const FVector3& Position, const FQuat& Rotation, const FCameraComponent& Camera, float AspectRatio)
{
	// FSceneCamera::ApplyToCamera + FCamera::GetUnjitteredViewProjectionMatrix와 같은 식 (값 보정 포함)
	const FQuat      Normalized = Rotation.GetNormalized();
	const FMatrix4x4 View       = FMatrix4x4::MakeLookAt(Position, Position + Normalized.GetForwardVector(), Normalized.GetUpVector());
	const float      Aspect     = AspectRatio > 0.0f ? AspectRatio : 1.0f;
	const float      NearZ      = FMath::Max(Camera.NearZ, 0.01f);
	const float      FarZ       = FMath::Max(Camera.FarZ, NearZ + 1.0f);
	if (Camera.bOrthographic)
	{
		const float Height = FMath::Max(Camera.OrthoHeight, 1.0f);
		return View * FMatrix4x4::MakeOrthographic(Height * Aspect, Height, NearZ, FarZ);
	}
	const float Fov = FMath::Clamp(Camera.FovYDegrees, 1.0f, 179.0f);
	return View * FMatrix4x4::MakePerspectiveFov(FMath::DegreesToRadians(Fov), Aspect, NearZ, FarZ);
}

bool FCameraProjection::ComputeViewProjection(FScene& Scene, FEntity CameraEntity, float AspectRatio, FMatrix4x4& OutViewProjection)
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
	OutViewProjection = MakeViewProjection(Position, Rotation, *Camera, AspectRatio);
	return true;
}

bool FCameraProjection::WorldToViewport(const FMatrix4x4& ViewProjection, const FVector3& WorldPosition, const FVector2& ViewportSize, FVector2& OutPixel)
{
	const FVector4 Clip = ViewProjection.TransformVector4(FVector4(WorldPosition, 1.0f));
	if (!(Clip.W > 1.0e-6f))
	{
		OutPixel = FVector2(-1.0f, -1.0f);
		return false; // 카메라 뒤 (직교는 w = 1이라 여기 오지 않는다)
	}
	const float InvW  = 1.0f / Clip.W;
	const float NdcX  = Clip.X * InvW;
	const float NdcY  = Clip.Y * InvW;
	const float Depth = Clip.Z * InvW;
	OutPixel          = FVector2((NdcX * 0.5f + 0.5f) * ViewportSize.X, (0.5f - NdcY * 0.5f) * ViewportSize.Y);
	return Depth >= 0.0f && Depth <= 1.0f && OutPixel.X >= 0.0f && OutPixel.X <= ViewportSize.X && OutPixel.Y >= 0.0f &&
	       OutPixel.Y <= ViewportSize.Y;
}

bool FCameraProjection::ViewportToWorldRay(const FMatrix4x4& ViewProjection, const FVector2& Pixel, const FVector2& ViewportSize, FVector3& OutOrigin,
                                           FVector3& OutDirection)
{
	FMatrix4x4 Inverse;
	if (!(ViewportSize.X > 0.0f && ViewportSize.Y > 0.0f) || !ViewProjection.TryGetInverse(Inverse))
	{
		return false;
	}
	const float    NdcX  = Pixel.X / ViewportSize.X * 2.0f - 1.0f;
	const float    NdcY  = 1.0f - Pixel.Y / ViewportSize.Y * 2.0f;
	const FVector4 Near  = Inverse.TransformVector4(FVector4(NdcX, NdcY, 0.0f, 1.0f));
	const FVector4 Far   = Inverse.TransformVector4(FVector4(NdcX, NdcY, 1.0f, 1.0f));
	if (FMath::Abs(Near.W) < 1.0e-12f || FMath::Abs(Far.W) < 1.0e-12f)
	{
		return false;
	}
	const FVector3 NearPoint = FVector3(Near.X, Near.Y, Near.Z) * (1.0f / Near.W);
	const FVector3 FarPoint  = FVector3(Far.X, Far.Y, Far.Z) * (1.0f / Far.W);
	const FVector3 Direction = FarPoint - NearPoint;
	if (Direction.IsNearlyZero())
	{
		return false;
	}
	OutOrigin    = NearPoint;
	OutDirection = Direction.GetNormalized();
	return true;
}
