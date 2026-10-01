#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Common.h"

#include <functional>

// 그림자 패스에 메시 인스턴스 목록 밖 캐스터(지형 등)를 더 그리는 콜백.
// 그림자 장(방향광 캐스케이드 / 점광원·스포트 장)마다 그 장의 DSV·뷰포트가 바인딩된 상태로 불린다.
// 콜백은 자기 루트 시그니처/PSO를 바인딩한다 (부른 쪽은 콜백 뒤 자기 상태를 다시 설정하지 않으므로 장마다의 메시 그리기 뒤에 부른다)
//   bLocalLight: 원근 그림자 장 (로컬 라이트 — 깊이 바이어스가 다르다)
using FShadowCasterHook = std::function<void(ID3D12GraphicsCommandList* CommandList, const FMatrix4x4& ViewProjection, const FFrustum& Frustum, bool bLocalLight)>;
