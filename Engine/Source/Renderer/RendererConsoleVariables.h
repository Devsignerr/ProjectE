#pragma once

#include "Core/Console/Console.h"

// 렌더러 콘솔 변수 (전역 — 모든 FSceneRenderer 공통). FSceneRenderer가 Render마다 읽어 반영한다 (ApplyConsoleVariables).
//   예전 명령줄 플래그는 각 변수의 별칭으로 그대로 동작한다 (--no-ssr = r.SSR 0 등), 새 방식은 --cvar r.SSR=0.
//   r.TAA/r.SSAO/r.SSR은 렌더러별 FPostProcessSettings 값과 AND (썸네일처럼 렌더러가 끈 것은 켜지지 않는다).
//   주의: 아래 전역 객체는 엔진 DLL 안에서만 직접 쓴다 (데이터는 DLL 밖으로 내보내지 않음) —
//   에디터/게임 모듈은 FConsoleManager::Get().FindVariable("r.SSR")로 찾는다.
namespace RendererCVars
{
	extern TAutoConsoleVariable<bool>  TemporalAA;       // r.TAA            (--no-taa)
	extern TAutoConsoleVariable<bool>  AmbientOcclusion; // r.SSAO           (--no-ssao)
	extern TAutoConsoleVariable<bool>  Reflections;      // r.SSR            (--no-ssr)
	extern TAutoConsoleVariable<bool>  DepthPrepass;     // r.DepthPrepass   (--no-depth-prepass)
	extern TAutoConsoleVariable<bool>  Occlusion;        // r.Occlusion      (--occlusion)
	extern TAutoConsoleVariable<bool>  SkinCulling;      // r.SkinCulling    (--no-skin-culling)
	extern TAutoConsoleVariable<bool>  ParticleCulling;  // r.ParticleCulling (--no-particle-culling)
	extern TAutoConsoleVariable<bool>  Lod;              // r.LOD            (--no-lod)
	extern TAutoConsoleVariable<int32> ForceLod;         // r.ForceLOD       (--force-lod N)
	extern TAutoConsoleVariable<float> LodHysteresis;    // r.LODHysteresis  (--lod-hysteresis X)
	extern TAutoConsoleVariable<bool>  Jitter;           // r.Jitter         (--jitter)
	extern TAutoConsoleVariable<int32> DebugView;        // r.DebugView      (--debug-view normal|velocity|depth|ao|ssr)
	extern TAutoConsoleVariable<bool>  ResourceAutoCollect; // r.ResourceAutoCollect (리소스 자동 수거, Phase 37)
} // namespace RendererCVars
