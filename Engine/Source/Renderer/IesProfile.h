#pragma once

#include "Core/CoreTypes.h"

#include <string>
#include <string_view>
#include <vector>

// IES 배광 프로필 (IESNA LM-63-1986/1991/1995/2002, Phase 52). 순수 로직 — 테스트 Ies_*.
//   읽기: 머리(첫 줄 IESNA 표식은 선택, [키워드] 줄 무시) → TILT=NONE|INCLUDE|<파일> (INCLUDE면 기울기 표를 건너뜀, 파일 이름이면 무시)
//         → 숫자 목록(공백·쉼표·줄바꿈 아무렇게나): 램프 수, 램프당 루멘, 칸델라 배율, 수직 각 수, 수평 각 수, 측광 종류, 단위, 폭, 길이, 높이,
//           안정기 계수, 미래용, 입력 와트, 수직 각들, 수평 각들, 칸델라(수평 각마다 수직 각 전부)
//   측광 종류 C(1)만 정식 지원 (A/B는 경고 후 C처럼 읽는다). 수직 각 0 = 천저(빛 축), 범위 0..90 / 90..180 / 0..180 — 범위 밖은 0.
//   수평 각 대칭: 0 하나(회전 대칭), 0..90(4분면), 0..180(좌우), 90..270(앞뒤), 0..360(전체) — Sample이 접어서 찾는다
//   값 = 칸델라 × 배율 × 안정기 계수. 텍스처는 AreaLightMath::ComputeIesUV와 같은 칸 배치(가로 θ, 세로 φ)로 최대값 1에 정규화
struct FIesProfile
{
	std::vector<float> VerticalAngles;   // 도, 오름차순
	std::vector<float> HorizontalAngles; // 도, 오름차순
	std::vector<float> Candela;          // [수평 각 번호 × 수직 각 수 + 수직 각 번호] (배율 적용)
	float              MaxCandela  = 0.0f;
	int32              PhotometricType = 1;
	float              LumensPerLamp   = 0.0f; // -1 = 절대 측광
	std::string        FormatName;              // "LM-63-2002" 등 (없으면 "LM-63-1986")

	// 텍스트 파싱. 실패하면 false + OutError (Out은 비워진다)
	static bool Parse(std::string_view Text, FIesProfile& Out, std::string* OutError = nullptr);

	// 칸델라 (θ = 수직 각, φ = 수평 각, 도). 범위 밖 수직 각은 0, 수평 각은 대칭으로 접는다
	float Sample(float ThetaDegrees, float PhiDegrees) const;
	// 수평 각 대칭 접기 (Sample 내부 — 테스트용 공개)
	float FoldHorizontalAngle(float PhiDegrees) const;

	// 텍스처 굽기 (Width = θ 0..180, Height = φ 0..360, 칸 가운데 표본 — AreaLightMath::ComputeIesUV), 최대 1로 정규화
	std::vector<float> BakeTexture(uint32 Width, uint32 Height) const;
};
