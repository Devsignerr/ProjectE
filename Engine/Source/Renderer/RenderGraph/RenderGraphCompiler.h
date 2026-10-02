#pragma once

#include "Renderer/RenderGraph/RenderGraphTypes.h"

#include <string>
#include <vector>

// 렌더 그래프 컴파일 (순수 로직 — GPU 없이 테스트 가능, Tests/RendererTests/RenderGraphTests.cpp).
//
// 입력: 리소스(서브리소스 수, 가져온 리소스 여부, 시작/끝 상태) + 등록 순서대로 패스(큐, 컬링 금지, 접근 목록)
// 출력: 살아남은 패스와 실행 순서, 패스 앞 배리어 묶음, 비동기 계산 묶음(포크/조인), 끝 배리어, 리소스 수명
//
// 규칙 (바꾸면 RenderGraphTests를 함께 고친다):
//  1. 컬링: 뿌리 = bNeverCull 패스 + 가져온(외부) 리소스에 쓰는 패스. 살아 있는 패스가 읽는 서브리소스의 직전 쓰기 패스를 거꾸로 따라가
//     살린다. 쓰기는 bOverwrite가 아니면 이전 내용을 읽는 것으로 본다 (블렌딩/부분 갱신/UAV 누적 — 보수적).
//  2. 상태 전이: 서브리소스마다 현재 상태를 추적해 필요한 상태와 다르면 패스 앞에 전이. 읽기 상태로 바꿀 때는 다음 쓰기 전까지
//     이어지는 읽기 상태를 미리 합쳐(look-ahead) 한 번만 전이한다. 같은 UAV를 연속 패스가 쓰면 UAV 배리어.
//     리소스의 모든 서브리소스가 같은 전/후 상태로 바뀌면 ALL_SUBRESOURCES 배리어 하나로 묶는다.
//  3. 비동기 계산: 등록 순서상 연속한 살아 있는 AsyncCompute 패스들 = 묶음 하나.
//     포크 = 묶음보다 앞에 등록된 그래픽스 패스 중 묶음과 충돌(묶음이 읽는 것을 씀 / 묶음이 쓰는 것을 읽거나 씀)하는 마지막 패스 뒤.
//     조인 = 묶음보다 뒤에 등록된 그래픽스 패스 중 충돌하는 첫 패스 앞 (없으면 그래프 끝). 읽기끼리는 충돌이 아니다.
//     실행 타임라인 = 그래픽스 패스 순서 + 포크 위치에 묶음. 묶음이 처음 쓰는 상태로의 전이는 포크 앞 그래픽스 큐에서 하고(계산 큐는
//     픽셀 셰이더/렌더 타깃/깊이 상태로 전이할 수 없다), 묶음 안 전이는 계산 큐에서 계산 큐 합법 상태로만 한다.
//     묶음끼리는 등록 순서를 지킨다 (포크 위치가 앞 묶음보다 앞이 될 수 없다).
//     뒤 묶음의 포크가 앞 묶음의 조인보다 앞이면 두 묶음을 하나로 합친다 (포크 = 뒤 묶음 포크, 조인 = 이른 쪽 — 제출 수 절약).
//  4. 끝: 가져온 리소스는 FinalState(None이면 그대로 두고 FinalStates로 알림)로, 내부 리소스는 그대로 둔다(풀이 다음 사용 때 시작 상태로 받음).
//  5. 수명: 리소스별 실행 타임라인상 첫/마지막 사용 패스 (메모리 별칭 2차 작업용).
struct FRGCompileResource
{
	const char* Name      = "";
	uint32      MipCount  = 1;
	uint32      ArraySize = 1;
	bool        bImported = false;
	ERGAccess   InitialState = ERGAccess::Common;   // InitialStates가 비었을 때 모든 서브리소스
	std::vector<ERGAccess> InitialStates;          // 서브리소스별 시작 상태 (선택)
	ERGAccess   FinalState = ERGAccess::None;      // 가져온 리소스의 끝 상태 (None = 마지막 상태 유지)
	bool        bUniformFinal = false;             // FinalState None일 때 서브리소스 상태를 서브리소스 0의 상태로 맞춘다 (상태 하나로 추적하는 외부 소유자)

	uint32 GetSubresourceCount() const { return MipCount * ArraySize; }
};

struct FRGCompileAccess
{
	uint32              Resource = 0;
	FRGSubresourceRange Range;
	ERGAccess           Access     = ERGAccess::None;
	bool                bOverwrite = false; // 쓰기가 이전 내용을 전혀 읽지 않음 (컬링 의존성 끊음)
};

struct FRGCompilePass
{
	const char*                   Name       = "";
	ERGQueue                      Queue      = ERGQueue::Graphics;
	bool                          bNeverCull = false; // 부수 효과 (외부 출력·리드백·통계 등)
	std::vector<FRGCompileAccess> Accesses;
};

struct FRGCompileOptions
{
	bool bCullPasses        = true;
	bool bAsyncCompute      = true; // 끄면 AsyncCompute 패스도 그래픽스 큐에서 등록 순서대로
	bool bMergeAsyncBatches = true; // 실행 구간이 겹치는 묶음을 하나로 (규칙 3.5)
};

struct FRGBarrier
{
	static constexpr uint32 AllSubresources = ~0u;

	bool      bUav        = false; // UAV 배리어 (상태 전이 아님)
	uint32    Resource    = 0;
	uint32    Subresource = AllSubresources;
	ERGAccess Before      = ERGAccess::None;
	ERGAccess After       = ERGAccess::None;
};

// 실행 타임라인 한 칸: 그래픽스 패스 하나 또는 비동기 계산 묶음 하나(포크 위치)
struct FRGStep
{
	bool   bAsyncBatch = false;
	uint32 Index       = 0; // 패스 번호 또는 묶음 번호
};

struct FRGAsyncBatch
{
	std::vector<uint32>     Passes;               // 등록 순서
	int32                   ForkAfterPass  = -1;  // 이 그래픽스 패스 뒤에서 포크 (-1 = 그래프 처음)
	int32                   JoinBeforePass = -1;  // 이 그래픽스 패스 앞에서 조인 (-1 = 그래프 끝)
	std::vector<FRGBarrier> ForkBarriers;         // 포크 직전 그래픽스 큐에서
};

struct FRGLifetime
{
	int32 FirstPass = -1; // 패스 번호 (등록 순서), 사용 없음 = -1
	int32 LastPass  = -1;
	int32 FirstStep = -1; // 실행 타임라인 칸 번호
	int32 LastStep  = -1;
};

struct FRGCompiledPass
{
	bool                    bCulled = false;
	bool                    bAsync  = false; // 계산 큐에서 실행
	std::vector<FRGBarrier> Barriers;        // 패스 직전 (실행 큐에서)
};

struct FRGCompileResult
{
	std::vector<FRGCompiledPass> Passes;
	std::vector<FRGStep>         Steps;
	std::vector<FRGAsyncBatch>   Batches;
	std::vector<FRGBarrier>      FinalBarriers;      // 그래프 끝 그래픽스 큐에서 (가져온 리소스 → FinalState)
	std::vector<FRGLifetime>     Lifetimes;          // 리소스별
	std::vector<std::vector<ERGAccess>> FinalStates; // 리소스·서브리소스별 그래프 끝 상태 (끝 배리어 반영)
	std::vector<std::string>     Errors;             // 잘못된 선언 (빈 목록이어야 정상)

	uint32 CulledPassCount   = 0;
	uint32 TransitionCount   = 0; // 상태 전이 배리어 수 (ALL_SUBRESOURCES 묶음은 1개)
	uint32 UavBarrierCount   = 0;
	uint32 BarrierBatchCount = 0; // ResourceBarrier 호출 수 (빈 묶음 제외)
};

namespace RenderGraphCompiler
{
	FRGCompileResult Compile(const std::vector<FRGCompileResource>& Resources, const std::vector<FRGCompilePass>& Passes,
	                         const FRGCompileOptions& Options = FRGCompileOptions{});
}
