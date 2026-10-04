#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <string>
#include <string_view>
#include <vector>

// ---- 2D 공통 규약 (Phase 56 — 스프라이트/플립북/타일셋/타일맵 데이터, Scene/Sprite/ 전체가 따른다) ----------------------------------
//   2D 평면 = 월드 X(오른쪽) · Z(위). 엔진 왼손 Z-up(1 = 1cm)에서 화면 오른쪽 = +X가 되려면 2D 카메라는 **+Y 쪽에서 -Y를 본다**
//   (yaw -90 — -Y에서 +Y를 보면 화면 오른쪽이 -X가 된다). 스프라이트/타일 사각형은 엔티티 로컬 X-Z 평면 위에 놓이고 앞면(법선)은
//   +Y(카메라 쪽), 엔티티 Y가 깊이다. 2D 회전각(화면 반시계 +) = FQuat::FromAxisAngle(+Y, -각). 이 폴더의 "반시계"는 화면(X 오른쪽, Z 위) 기준. 이 폴더의 "로컬 2D 좌표" FVector2(X, Y)는 (로컬 X, 로컬 Z)를 뜻한다.
//   이미지 픽셀 좌표(슬라이스 X/Y, 타일 다각형 점)는 왼쪽 위 원점, 아래로 +. UV도 같은 방향(V 아래로 +, D3D 규약).
//   크기 단위: "1px = UnitsPerPixel cm" (기본 1.0 — 픽셀당 cm 수. 유니티 PixelsPerUnit의 역수 개념이지만 이름 혼동을 피해 이렇게 둔다).
//   상대 경로: 에셋 안의 다른 파일 경로(Texture, Sprite)는 .emat처럼 "이 파일이 있는 폴더 기준" (FSprite2DLibrary::ResolveReference).
// --------------------------------------------------------------------------------------------------------------------

// 텍스처 샘플링 필터 (번호는 JSON에 이름으로 저장 — 끝에만 추가)
enum class ESpriteFilter : int32
{
	Point,  // 최근접 (픽셀 아트 기본)
	Linear, // 이중 선형
};

const char*   ToString(ESpriteFilter Filter);
ESpriteFilter ParseSpriteFilter(std::string_view Name, bool* bOutValid = nullptr); // 대소문자 무시, 모르면 Point

// 아틀라스 안 사각형 하나 (px, 왼쪽 위 원점)
struct FSpriteSlice
{
	std::string Name;
	int32       X = 0;
	int32       Y = 0;
	int32       W = 0;
	int32       H = 0;
	// 피벗 (0~1, 슬라이스 기준): X = 왼쪽 0 → 오른쪽 1, Y = **아래 0 → 위 1** (월드 Z 방향과 같게 — 이미지 Y와 반대). 기본 가운데
	FVector2 Pivot = FVector2(0.5f, 0.5f);
	// 9-슬라이스 테두리 (px, 왼/위/오른/아래). 모두 0이면 9-슬라이스 아님
	int32 BorderLeft   = 0;
	int32 BorderTop    = 0;
	int32 BorderRight  = 0;
	int32 BorderBottom = 0;

	bool HasBorder() const { return BorderLeft > 0 || BorderTop > 0 || BorderRight > 0 || BorderBottom > 0; }
	bool operator==(const FSpriteSlice& Other) const = default;
};

// UV 사각형 (U0,V0 = 이미지 왼쪽 위 모서리, U1,V1 = 오른쪽 아래 모서리. V 아래로 +)
struct FSpriteUvRect
{
	float U0 = 0.0f;
	float V0 = 0.0f;
	float U1 = 0.0f;
	float V1 = 0.0f;

	bool operator==(const FSpriteUvRect& Other) const = default;
};

// 스프라이트 사각형 정점 4개 (엔티티 로컬 2D = (X, Z) cm, 피벗 = 원점). 순서는 항상 공간상 왼쪽 아래 → 오른쪽 아래 → 오른쪽 위 → 왼쪽 위
// (반전해도 이 공간 순서를 유지하고 UV만 바뀐다 — 와인딩 불변). 화면(+Y에서 봄)에서 이 순서는 반시계이므로, 앞면 = 시계(CW) 규약인
// 렌더러는 삼각형을 (0, 2, 1), (0, 3, 2)로 묶는다 (Phase 56-4)
struct FSpriteQuad
{
	FVector2 Positions[4];
	FVector2 Uvs[4];
};

// .esprite — 스프라이트 아틀라스 (JSON)
// { "Version": 1, "Texture": "Hero.png", "TextureWidth": 128, "TextureHeight": 64, "UnitsPerPixel": 1.0, "Filter": "Point",
//   "Slices": [ { "Name": "Idle_0", "X": 0, "Y": 0, "W": 32, "H": 32, "Pivot": [0.5, 0.0], "Border": [4, 4, 4, 4] } ] }
//   Pivot 생략 = [0.5, 0.5], Border 생략 = 없음. TextureWidth/Height는 이미지를 읽지 않고 UV를 계산하려고 편집기/생성 도구가 채운다
//   (0이면 UV 계산 불가 — 렌더러가 텍스처를 읽은 뒤 실제 크기로 계산해도 된다).
struct FSpriteAsset
{
	static constexpr uint32         Version   = 1;
	static constexpr const wchar_t* Extension = L".esprite";

	std::string               Texture;           // 이 파일 폴더 기준 상대 경로
	int32                     TextureWidth  = 0; // px
	int32                     TextureHeight = 0;
	float                     UnitsPerPixel = 1.0f; // 1px = N cm
	ESpriteFilter             Filter        = ESpriteFilter::Point;
	std::vector<FSpriteSlice> Slices;

	// 이름 → 칸 (없으면 -1). 빈 이름이면 0번(슬라이스가 있으면)
	int32 FindSlice(std::string_view Name) const;
	std::vector<std::string> GetSliceNames() const;

	std::string ToJsonString() const;
	// 형식 오류(JSON 아님, 루트가 객체 아님)만 false. 이상한 값(음수 크기, 텍스처 밖, 이름 중복 등)은 고쳐서 읽고 OutWarnings에 적는다
	static bool FromJsonString(std::string_view Json, FSpriteAsset& OutAsset, std::vector<std::string>* OutWarnings = nullptr, std::string* OutError = nullptr);

	bool operator==(const FSpriteAsset& Other) const = default;
};

// 순수 계산 (테스트: Sprite_*)
namespace SpriteMath
{
	// 격자 자르기: 왼쪽 위에서 Margin px 띄우고 CellW x CellH 칸을 Spacing px 간격으로 행 우선(위 → 아래, 왼 → 오른) 자른다.
	// 칸 수 = (텍스처 - 2 × Margin + Spacing) / (Cell + Spacing) 내림 (오른쪽/아래 여백도 Margin으로 본다). 이름 = NamePrefix + 번호(0부터)
	std::vector<FSpriteSlice> SliceGrid(int32 TextureWidth, int32 TextureHeight, int32 CellWidth, int32 CellHeight, int32 Margin, int32 Spacing,
	                                    std::string_view NamePrefix);

	// 슬라이스 → UV. 반 텍셀 보정 없이 텍셀 경계 그대로 (X / 폭 ~ (X + W) / 폭):
	//   D3D10 이후 래스터라이저는 픽셀 가운데(.5)에서 표본을 잡고 텍셀 i는 UV [i/폭, (i+1)/폭]를 덮으므로, 경계 UV로 그린 사각형의
	//   픽셀 가운데는 항상 슬라이스 안 텍셀 가운데 쪽에 떨어진다 — 최근접 필터에서 정확히 슬라이스 텍셀만 나온다.
	//   반 텍셀을 안으로 줄이면(D3D9 시절 보정) 가장자리 텍셀이 반쪽만 보이고 크기가 1px 어긋난다. 선형 필터의 이웃 칸 번짐은 UV를 줄여
	//   해결하지 않고 아틀라스 간격(Spacing)/여백 확장과 픽셀 스냅으로 해결한다. 텍스처 크기가 0 이하면 {0,0,0,0}
	FSpriteUvRect ComputeUvRect(const FSpriteSlice& Slice, int32 TextureWidth, int32 TextureHeight);

	// 슬라이스의 그려질 크기 (cm): Size 두 축이 0 이하면 픽셀 크기 × UnitsPerPixel, 한 축만 주면 그 축에 맞춰 비율 유지, 둘 다 주면 그대로
	// (엔티티 스케일은 월드 행렬이 곱한다 — 여기서 다루지 않는다)
	FVector2 ComputeSize(const FSpriteSlice& Slice, float UnitsPerPixel, const FVector2& Size);

	// 로컬 사각형 (피벗 = 원점). bFlipX/Y는 피벗을 지나는 축으로 거울 반사 (유니티 SpriteRenderer.flipX/Y와 같다)
	FSpriteQuad ComputeQuad(const FSpriteSlice& Slice, int32 TextureWidth, int32 TextureHeight, float UnitsPerPixel, const FVector2& Size, bool bFlipX,
	                        bool bFlipY);
} // namespace SpriteMath
