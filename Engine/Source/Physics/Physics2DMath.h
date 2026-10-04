#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <string>
#include <string_view>
#include <vector>

// 2D 물리 평면 규약 (Physics2DSystem/Physics2DWorld/스크립트가 모두 따른다 — 바꾸면 Physics2D_* 테스트도):
//   평면 = 월드 X(화면 오른쪽)·Z(화면 위). 2D 카메라는 +Y 쪽에서 -Y를 본다 (엔진 왼손 Z-up에서 -Y를 보면 오른쪽 = +X —
//   -Y 쪽에서 +Y를 보면 오른쪽이 -X라 좌우가 뒤집힌다. Physics2D_CameraLooksAlongMinusY 테스트로 고정).
//   평면 좌표 FVector2(X, Y) = (월드 X, 월드 Z), cm. Box2D (x, y) m = 평면 좌표 / 100 (변환은 Physics2DWorld.cpp 안에서만).
//   깊이 = 월드 Y: 바디는 깊이를 바꾸지 않는다 (엔티티 값을 유지).
//   2D 각도 = 화면에서 반시계 + (라디안, Box2D 각과 같다) = 월드 Y축 둘레 -각 회전: FQuat::FromAxisAngle(+Y, -각)
//   (엔진 FQuat은 해밀턴 곱 q v q*라 +Y축 +각은 +Z → +X, 즉 화면 시계 방향). 로컬 +X가 각만큼 반시계로 돈다.
//   엔티티의 다른 축 회전(Y축이 기울어진 회전)은 무시한다 — 각은 로컬 +X를 평면에 투영한 방향으로 정한다.
namespace Physics2DMath
{
	inline FVector2 ToPlane(const FVector3& World) { return FVector2(World.X, World.Z); }
	inline FVector3 FromPlane(const FVector2& Plane, float Depth) { return FVector3(Plane.X, Depth, Plane.Y); }

	// 2D 각(반시계 +, 라디안) → 월드 회전 (Y축 둘레)
	FQuat RotationFromAngle(float AngleRadians);
	// 월드 회전 → 2D 각 (로컬 +X의 평면 투영 방향). OutTilted = Y축이 기울어진 회전(평면 밖 회전 — 무시됨)
	float AngleFromRotation(const FQuat& Rotation, bool* bOutTilted = nullptr);

	// 평면 벡터를 각만큼 반시계로 돌린다
	FVector2 Rotate(const FVector2& V, float AngleRadians);

	// "x,z; x,z; ..." (cm) → 점 목록. 공백 허용, 빈 항목(끝의 ';')은 무시. 형식 오류면 false + OutError (OutPoints는 읽은 데까지)
	bool ParsePoints(std::string_view Text, std::vector<FVector2>& OutPoints, std::string* OutError = nullptr);
	std::string FormatPoints(const std::vector<FVector2>& Points);

	// 볼록 껍질 (Andrew monotone chain, 반시계, 같은 직선 위 점 제외). 3점 미만이면 빈 목록
	std::vector<FVector2> ComputeConvexHull(const std::vector<FVector2>& Points);
	// 볼록 다각형을 MaxPoints 이하로: 빼도 넓이를 가장 적게 잃는 점(이웃과 만드는 삼각형이 가장 작은 점)부터 뺀다
	std::vector<FVector2> ReduceConvexPolygon(std::vector<FVector2> Hull, uint32 MaxPoints);
	// 점 목록이 그 순서 그대로 볼록 다각형(반시계 또는 시계, 같은 직선 위 점 없음)인가
	bool IsConvexPolygon(const std::vector<FVector2>& Points);
	// 부호 있는 넓이 (반시계 +)
	float SignedArea(const std::vector<FVector2>& Points);
}
