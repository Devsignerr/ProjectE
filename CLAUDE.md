# ProjectE — 자체 3D 게임 엔진

## 개요

- 목표: 에디터를 포함한 범용 3D 게임 엔진 (장기 프로젝트)
- 플랫폼: Windows x64 / DirectX 12 / C++20 / MSVC (VS 2022)
- 의존성 방침: 핵심(창, 입력, 수학, RHI, 렌더러, ECS)은 직접 구현. 이미지·모델 로딩, UI 등은 CMake FetchContent로 도입
- 작업 순서와 상태는 `Plans.md`를 따른다

## 글로벌 UE 규칙의 적용 방식

이 프로젝트는 언리얼 엔진이 **아닌** 자체 엔진이다.

- 적용하지 않음: `UPROPERTY`/`UFUNCTION`, `NewObject`/`CreateDefaultSubobject`, UE GC 관련 규칙, `.uasset`/`.Build.cs` 보호 규칙, `Tick()` 규칙
- 유지함: UE 네이밍 (`F` 클래스/구조체, `E` enum, `I` 인터페이스, `T` 템플릿, `b` bool 접두사, PascalCase), 파일명 = 클래스명(접두사 제외), "Tick에 무거운 로직 금지" 정신은 `OnUpdate`/`OnRender`에 동일 적용
- 메모리: raw `new`/`delete` 금지. 소유는 값 멤버 또는 `std::unique_ptr`, COM 객체는 `ComPtr`, 비소유 참조는 raw 포인터/참조 + 주석으로 수명 명시

## 좌표계 / 수학 규약

- 왼손 Z-up (UE 방식): `+X` 앞(Forward), `+Y` 오른쪽(Right), `+Z` 위(Up). `Cross(Forward, Right) == Up`
- 행렬 `FMatrix4x4`는 행우선 저장, 행벡터 규약 `v * M`. 합성은 적용 순서대로: `World = S * R * T`, `MVP = World * View * Proj`. 이동은 `M[3][0..2]`
- 쿼터니언 `A * B`는 B 먼저 적용. `FQuat::FromEuler(Pitch, Yaw, Roll)`: +Pitch 기수 위, +Yaw 오른쪽, +Roll 오른쪽 날개 아래 (UE 부호)
- 뷰 공간: `+X` 오른쪽, `+Y` 위, `+Z` 앞. 투영 깊이 [0, 1]. 앞면은 시계 방향(CW)
- HLSL은 `-Zpr`(행우선)로 컴파일하므로 행렬은 전치 없이 그대로 업로드하고 `mul(v, M)`을 사용

## 코드 규칙

- 탭 들여쓰기, Allman 중괄호, 멤버 정렬은 열 맞춤 허용
- 헤더: `#pragma once`. include 경로는 모듈 루트 기준 (`#include "Core/Log.h"`, `#include "RHI/D3D12/D3D12RHI.h"`)
- 공개 헤더에 `Windows.h`를 포함하지 않는다. 플랫폼 .cpp에서만 `Core/Platform/WindowsHeaders.h` 포함
- C++ 소스는 UTF-8(BOM 없음), `/utf-8`로 컴파일. 주석·로그는 한국어
- `.hlsl`/`.hlsli`/`.ps1`은 UTF-8 **BOM 필수** (SDK 번들 DXC 1.6과 PowerShell 5.1이 BOM 없는 한글 UTF-8을 읽지 못함). `.editorconfig`에 반영됨
- 단위 테스트: `Core/Testing/TestFramework.h`의 `E_TEST`, `E_EXPECT_*` 사용. 순수 로직(수학 등)은 테스트를 함께 작성
- 상수 버퍼: `Renderer/ShaderTypes.h` 구조체와 HLSL cbuffer를 항상 함께 수정. 16바이트 패킹을 지키고 `static_assert`로 크기 고정
- 셰이더 리소스 바인딩: 프레임/오브젝트/머티리얼 상수는 루트 CBV(동적 업로드 버퍼에서 할당), 텍스처는 `GetSrvAllocator()`의 셰이더 가시 힙 디스크립터 테이블, 샘플러는 정적 샘플러
- 외부 라이브러리 추가는 `CMake/ThirdParty.cmake`에서 커밋/해시 고정, `ThirdParty::<name>` 별칭으로 링크. 서드파티 헤더 포함부는 `#pragma warning(push, 0)`으로 감싼다
- ECS: 컴포넌트는 POD에 가까운 struct, 로직은 시스템(뷰 순회)에 둔다. `View<...>().Each` 순회 중 같은 타입 컴포넌트 추가/제거 금지. 계층 변경은 반드시 `FScene::SetParent`
- 리소스 수명: GPU 리소스는 `FResourceManager` 핸들로만 참조하고 직접 소유하지 않는다. 렌더링 중 삭제는 `Destroy*`(지연 해제)로, 즉시 `Shutdown()`은 GPU Flush 이후에만
- 색공간: 백버퍼 RTV와 색상 텍스처는 sRGB 포맷, 셰이더는 선형 공간에서 계산. 데이터 텍스처(노멀 등)는 UNORM
- glTF 임포트: 좌표 변환은 `FGltfLoader::Convert*`만 사용(반사 변환이라 CCW→CW, 쿼터니언 벡터부 부호 반전). 새 속성 추가 시 `GltfLoaderTests`에 케이스 추가
- 테스트 매크로 인자에 템플릿 쉼표(`View<A, B>()`)를 직접 넣지 말고 지역 변수로 받는다
- 경고 = 에러 (`/W4 /WX`). 경고를 억제하지 말고 원인을 고친다
- 로그: `E_LOG(Category, Verbosity, "포맷 {}", 인자)` — std::format 문법. 카테고리는 헤더에서 `E_DECLARE_LOG_CATEGORY`, 하나의 .cpp에서 `E_DEFINE_LOG_CATEGORY`
- 검증: `E_CHECK(expr)`, `E_CHECKF(expr, "포맷", ...)` (실패 시 Fatal)
- D3D 호출: 초기화 경로는 `E_D3D_VERIFY(call)` (실패 시 Error 로그 + `return false`), 프레임 경로는 `E_D3D_CHECK(call)` (Fatal)
- 디버그 전용 코드는 `#if E_DEBUG` (CMake가 Debug=1 / Release=0 정의)

## 디렉터리

```
CMake/            CMake 헬퍼 모듈
Engine/Source/
  Core/           타입, 로그, 어설트, 타이머, 창(Win32), 입력, 애플리케이션 루프
  Core/Math/      FMath, FVector2/3/4, FQuat, FMatrix4x4, FBox, FFrustum (통합 헤더 Math.h)
  Core/ECS/       FEntity(세대 핸들), TSparseSet, FRegistry(+TView) — 자체 희소 집합 ECS
  Core/Containers/ THandle(태그별 세대 핸들), TResourcePool
  Core/Testing/   경량 단위 테스트 프레임워크
  Scene/          FScene(계층/트랜스폼 갱신), Components.h(Name/Transform/Hierarchy/StaticMesh/DirectionalLight), ResourceHandles.h
  RHI/D3D12/      디바이스, 커맨드 큐, 디스크립터 힙/할당자, 스왑체인, 깊이 버퍼, 동적 업로드 버퍼,
                  RHI 파사드, 셰이더 컴파일러(DXC), 루트 시그니처, PSO, 정적 버퍼, 텍스처
  Renderer/       카메라, 플라이 카메라 컨트롤러, 메시 데이터/프리미티브, FStaticMesh, 이미지 로더(stb_image),
                  FResourceManager(메시/텍스처/머티리얼 핸들 소유), FGltfLoader(cgltf)+FModelLoader(씬 배치),
                  FSceneRenderer(수집→컬링→정렬→드로우), ShaderTypes.h (cbuffer와 1:1 대응하는 CPU 구조체)
                  모듈 의존: Renderer → Scene → Core, Renderer → RHI → Core
Engine/Shaders/   HLSL (Common.hlsli 공통 헤더). 런타임에 소스 트리에서 직접 로드 (E_ENGINE_SHADER_DIR)
Sandbox/Source/   엔진 검증용 실행 파일
Sandbox/Assets/   테스트 에셋 (E_SANDBOX_ASSET_DIR)
Tests/            CoreTests, RendererTests (CTest 등록)
CMake/ThirdParty.cmake  FetchContent 외부 라이브러리 (커밋/해시 고정)
Scripts/          빌드 스크립트
Build/            CMake 빌드 출력 (git 제외)
```

새 모듈은 `Engine/Source/<Module>/CMakeLists.txt`에 `E<Module>` 정적 라이브러리 + `ProjectE::<Module>` 별칭으로 추가하고 `e_set_target_defaults`를 호출한다.

## 빌드

CMake와 Ninja는 PATH에 없고 VS 2022 번들 버전을 사용한다. 기본 경로는 Ninja 프리셋(VsDevCmd 환경 필요)이며 검증됨. Visual Studio 제너레이터 프리셋(`vs2022`)은 샌드박스 셸에서 VS 인스턴스 검색에 실패하므로 `.sln`이 필요할 때만 `-VisualStudio`로 사용자 셸에서 실행한다.

```powershell
.\Scripts\Build.ps1                  # 구성 + Ninja Debug 빌드
.\Scripts\Build.ps1 -Config Release
.\Scripts\Build.ps1 -Run             # 빌드 후 Sandbox 실행
.\Scripts\Build.ps1 -Test            # 빌드 후 단위 테스트 (ctest)
.\Scripts\Build.ps1 -VisualStudio    # .sln 생성 (Build\vs2022\ProjectE.sln)
```

셰이더만 빠르게 검증할 때는 SDK의 dxc.exe를 직접 사용한다 (`-HV 2021 -Zpr -WX -I Engine/Shaders`).

수동(VS 개발자 명령 프롬프트): `cmake --preset ninja-debug` → `cmake --build --preset ninja-debug`. 실행 파일은 `Build/ninja-<config>/Bin/Sandbox.exe`.

빌드/단위 테스트 실행은 사용자 승인(2026-09-29)에 따라 이 프로젝트에서 항상 자동으로 수행한다. Sandbox 실행(화면 확인)과 외부 다운로드는 사용자에게 안내/확인한다.

## 검증 체크리스트 (구현 후)

- [ ] `/W4 /WX` 통과
- [ ] 디버그 레이어 에러/경고 없음, 종료 시 라이브 오브젝트 보고에 누수 없음
- [ ] 리사이즈·최소화·포커스 전환에서 크래시 없음
- [ ] `Plans.md` 상태 갱신
