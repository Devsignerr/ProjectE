#pragma once

#include "UI/UITypes.h"

#include <string>
#include <vector>

class FUIFont;

// 그리기 모드 (UI.hlsl과 같은 번호)
enum class EUIDrawMode : uint32
{
	Box     = 0, // 둥근 사각형 + 테두리, 텍스처 곱하기
	SdfText = 1, // 글꼴 아틀라스 SDF (+ 외곽선)
};

// 사각형 하나 (화면 픽셀). 색은 선형 + 직선 알파. UI.hlsl FUIQuad와 1:1 (ShaderTypes.h의 FUIQuadGpu와 같은 배치)
struct FUIDrawQuad
{
	FVector4 Rect;           // (MinX, MinY, MaxX, MaxY)
	FVector4 UV;             // (U0, V0, U1, V1). SdfText는 아틀라스 텍셀 좌표
	FVector4 Color;          // 채우기 / 글자 색
	FVector4 SecondaryColor; // 테두리 / 외곽선 색
	FVector4 Params;         // Box: (모서리 반지름, 테두리 폭, 모드, 0)  SdfText: (거리 범위, 외곽선 폭, 모드, 0) — 픽셀
};
static_assert(sizeof(FUIDrawQuad) == 80);

// 텍스처 참조: 파일(Content 기준 경로) 또는 글꼴 아틀라스. 둘 다 없으면 흰색
struct FUITextureRef
{
	std::string    Path;
	const FUIFont* Font = nullptr;

	bool operator==(const FUITextureRef& Other) const { return Font == Other.Font && Path == Other.Path; }
};

// 같은 텍스처·잘림 영역으로 이어지는 사각형 묶음 (그리기 호출 하나)
struct FUIDrawBatch
{
	FUITextureRef Texture;
	FUIRect       Clip; // 화면 픽셀 (시저)
	uint32        FirstQuad = 0;
	uint32        QuadCount = 0;
};

// 한 프레임의 UI 그리기 목록 (여러 UI 인스턴스를 순서대로 쌓을 수 있다)
struct FUIDrawList
{
	std::vector<FUIDrawQuad>  Quads;
	std::vector<FUIDrawBatch> Batches;

	void Clear()
	{
		Quads.clear();
		Batches.clear();
	}
	bool IsEmpty() const { return Quads.empty(); }

	// 앞 묶음과 텍스처·잘림이 같으면 이어 붙인다
	void AddQuad(const FUIDrawQuad& Quad, const FUITextureRef& Texture, const FUIRect& Clip);
};
