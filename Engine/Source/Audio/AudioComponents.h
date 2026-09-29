#pragma once

#include "Core/CoreTypes.h"

#include <string>

// 사운드 발생원. 트랜스폼 월드 위치에서 재생된다 (bSpatial일 때).
// FAudioSystem이 컴포넌트를 발견하면 사운드를 만들고, bPlayOnStart면 바로 재생한다.
// 속성 변경(볼륨/피치/루프/거리)은 매 프레임 반영된다. 클립 경로를 바꾸면 사운드를 다시 만든다.
struct FAudioSourceComponent
{
	std::string ClipAsset;           // 프로젝트 Content 기준 상대 경로 (.wav/.flac/.mp3)
	float       Volume       = 1.0f; // 선형 게인
	float       Pitch        = 1.0f;
	bool        bLoop        = false;
	bool        bPlayOnStart = true;
	bool        bSpatial     = true;
	float       MinDistance  = 100.0f;  // cm: 이 거리까지는 감쇠 없음
	float       MaxDistance  = 5000.0f; // cm: 이 거리 이후로는 더 감쇠하지 않음
};
