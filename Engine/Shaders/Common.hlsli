#ifndef E_COMMON_HLSLI
#define E_COMMON_HLSLI

// 엔진 공통 셰이더 헤더
// - 행렬은 CPU(FMatrix4x4)와 동일한 행우선(row-major) 레이아웃 (컴파일 옵션 -Zpr)
// - 벡터는 행벡터: 변환은 mul(v, M)
// - 좌표계: 왼손 Z-up (+X 앞, +Y 오른쪽, +Z 위)

static const float E_PI = 3.14159265358979f;

#endif // E_COMMON_HLSLI
