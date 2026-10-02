#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

class FScene;
struct FCameraComponent;

// 게임 카메라(FCameraComponent)의 투영 계산 — GPU 없는 순수 식 (스크립트 Camera.WorldToScreen, 렌더러 FSceneCamera 공용)
//
// 규칙:
//   - 활성 카메라 = bPrimary인 카메라 중 Priority가 가장 큰 것 (같으면 먼저 찾은 것). 렌더러(FSceneCamera::FindPrimary)와 같은 함수
//   - 뷰-투영 = FCamera와 같은 식 (MakeLookAt(위치, 위치 + Forward, Up) * 원근/직교, 지터 없음). 종횡비는 출력(뷰포트) 가로/세로
//   - 뷰포트 픽셀: (0, 0) = 왼쪽 위, +Y 아래, 크기 = 출력 픽셀. 픽셀 아트 모드도 최종 출력 기준이다 — 저해상도 렌더는
//     출력과 중심을 맞춰 확대하고 카메라 스냅 나머지를 확대 단계에서 되돌리므로(PixelArtMath.h) 스냅 전 카메라로 계산한 값과 같다
struct FCameraProjection
{
	// 활성 카메라 엔티티 (없으면 NullEntity)
	static FEntity FindActiveCamera(FScene& Scene);

	// 위치/회전 + 카메라 설정 → 뷰-투영 (행벡터 v * M)
	static FMatrix4x4 MakeViewProjection(const FVector3& Position, const FQuat& Rotation, const FCameraComponent& Camera, float AspectRatio);
	// 엔티티의 월드 행렬(스케일 무시) 기준. 카메라/트랜스폼이 없으면 false
	static bool ComputeViewProjection(FScene& Scene, FEntity CameraEntity, float AspectRatio, FMatrix4x4& OutViewProjection);

	// 월드 → 뷰포트 픽셀. 반환: 카메라 앞(깊이 [0, 1]) && 뷰포트 안. 카메라 뒤(원근 w <= 0)면 OutPixel은 의미 없음
	static bool WorldToViewport(const FMatrix4x4& ViewProjection, const FVector3& WorldPosition, const FVector2& ViewportSize, FVector2& OutPixel);
	// 뷰포트 픽셀 → 월드 광선 (근평면 점 → 원평면 방향, 정규화). 역행렬이 없으면 false
	static bool ViewportToWorldRay(const FMatrix4x4& ViewProjection, const FVector2& Pixel, const FVector2& ViewportSize, FVector3& OutOrigin,
	                               FVector3& OutDirection);
};
