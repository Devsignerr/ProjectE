# ProjectE — 자체 3D 게임 엔진

## 개요

- 목표: 에디터를 포함한 범용 3D 게임 엔진 (장기 프로젝트)
- 플랫폼: Windows x64 / DirectX 12 / C++20 / MSVC (VS 2022)
- 의존성 방침: 핵심(창, 입력, 수학, RHI, 렌더러, ECS, 리플렉션)은 직접 구현. 이미지·모델 로딩, UI, JSON, 스크립트 VM 등은 CMake FetchContent로 도입
- 스크립팅: 하이브리드. 엔진 시스템·무거운 게임플레이는 C++ ECS 시스템, 콘텐츠 로직은 Lua 스크립트 컴포넌트. 매 프레임 대량 순회를 스크립트에 두지 않는다
- 프로젝트 구조: 엔진/프로젝트 분리. 엔진 콘텐츠는 실행 파일 기준, 게임 콘텐츠는 `.eproject`가 있는 프로젝트 폴더(`Content/`, `Saved/`, `Config/`) 기준. 경로는 항상 `FPaths`로 얻고 컴파일 타임 절대 경로를 넣지 않는다
- 직렬화: 소스 에셋/씬은 JSON, 런타임은 쿠킹된 엔진 바이너리. 모델/이미지 로드는 반드시 `FAssetCache::Load*Asset`을 거친다(`<프로젝트>/Cooked/`, gitignore). 쿠킹 형식을 바꾸면 `FAssetCache::*Version`을 올린다
- 작업 순서와 상태는 `Plans.md`를 따른다

## 글로벌 UE 규칙의 적용 방식

이 프로젝트는 언리얼 엔진이 **아닌** 자체 엔진이다.

- 적용하지 않음: `UPROPERTY`/`UFUNCTION`, `NewObject`/`CreateDefaultSubobject`, UE GC 관련 규칙, `.uasset`/`.Build.cs` 보호 규칙, `Tick()` 규칙
- 유지함: UE 네이밍 (`F` 클래스/구조체, `E` enum, `I` 인터페이스, `T` 템플릿, `b` bool 접두사, PascalCase), 파일명 = 클래스명(접두사 제외), "Tick에 무거운 로직 금지" 정신은 `OnUpdate`/`OnRender`에 동일 적용
- 메모리: raw `new`/`delete` 금지. 소유는 값 멤버 또는 `std::unique_ptr`, COM 객체는 `ComPtr`, 비소유 참조는 raw 포인터/참조 + 주석으로 수명 명시

## 좌표계 / 수학 규약

- 왼손 Z-up (UE 방식): `+X` 앞(Forward), `+Y` 오른쪽(Right), `+Z` 위(Up). `Cross(Forward, Right) == Up`
- 단위: 1 = 1cm (UE 방식), 질량 kg, 시간 초. 미터 기반 외부 데이터/라이브러리(glTF, 물리)는 경계에서 `FUnits::MetersToUnits`/`UnitsToMeters`로만 변환. glTF는 위치·이동만 ×100(`FGltfLoader::ImportScale`), 방향 벡터는 스케일하지 않는다
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
- 리플렉션: 새 컴포넌트는 `Scene/SceneReflection.cpp`의 `RegisterSceneTypes()`에 `RegisterType<T>(...).Property(...).AsComponent()`로 등록해야 인스펙터/직렬화에 나타난다. 파생 값(WorldMatrix 등)은 등록하지 않는다. 프로퍼티 타입은 `EPropertyType`에 있는 것만 지원
- ECS: 컴포넌트는 POD에 가까운 struct, 로직은 시스템(뷰 순회)에 둔다. `View<...>().Each` 순회 중 같은 타입 컴포넌트 추가/제거 금지. 계층 변경은 반드시 `FScene::SetParent`
- 리소스 수명: GPU 리소스는 `FResourceManager` 핸들로만 참조하고 직접 소유하지 않는다. 렌더링 중 삭제는 `Destroy*`(지연 해제)로, 즉시 `Shutdown()`은 GPU Flush 이후에만
- 색공간: 백버퍼 RTV와 색상 텍스처는 sRGB 포맷, 셰이더는 선형 공간에서 계산. 데이터 텍스처(노멀 등)는 UNORM
- 머티리얼(PBR, glTF 금속/거칠기): 텍스처 슬롯 t0~t4 = 베이스(sRGB)/금속거칠기(선형, G=거칠기 B=금속)/노멀(선형)/AO(선형 R)/발광(sRGB). 머티리얼마다 셰이더 가시 힙에 연속 5칸 테이블(`FMaterial::TextureTable`)을 두며 `FResourceManager`만 만들고 갱신한다. 머티리얼 텍스처 핸들을 바꾸면 `RefreshMaterialTextures` 호출
- 탄젠트: `FVertex::Tangent.xyz` = UV +U 방향, `B = Cross(N, T) * Tangent.w` = 노멀 맵 +Y = UV 위쪽(-V). glTF TANGENT는 `FGltfLoader::ConvertTangent`(xyz 변환 + w 반전 — 축 반사 때문), 없으면 `FMeshData::ComputeTangents`
- glTF 임포트: 좌표 변환은 `FGltfLoader::Convert*`만 사용(쿼터니언 벡터부 부호 반전). 축 반사와 엔진 카메라 규약의 반사가 상쇄되어 화면상 와인딩은 유지되므로 로더가 인덱스 순서를 뒤집어 CW 앞면으로 맞춘다. 와인딩 규약 검증: `Cross(P1-P0, P2-P0)·Normal > 0`. 새 속성 추가 시 `GltfLoaderTests`에 케이스 추가
- 스켈레탈 애니메이션: 스킨 정점은 슬롯 1 스트림(`FSkinVertex`, 정적 메시 정점 형식은 불변), 본 팔레트 = `InverseBind * JointWorld`(루트 CBV b4, `SkinnedMesh.hlsli`)로 바로 월드 공간이며 World는 항등. 팔레트는 `FSceneRenderer`가 프레임당 한 번 `FSkinnedMeshPalette::Build`로 올리고 섀도우/아웃라인이 공유한다. 재생 로직은 `FAnimationSystem`(Scene), 제어는 `Play/Stop/SetSpeed` API. 새 메시 패스를 추가하면 스킨 경로(`DrawSkinned` + 스킨 PSO)도 함께 추가
- 테스트 매크로 인자에 템플릿 쉼표(`View<A, B>()`)를 직접 넣지 말고 지역 변수로 받는다
- ImGui: UI는 `FImGuiLayer::BeginFrame()`~`EndFrame()` 사이에서만 기술하고, UI 드로우는 UNORM 백버퍼 뷰(`SetRenderTargetToBackBuffer(true)`)에 그린다. 뷰포트에 표시할 오프스크린 타깃은 `FD3D12RenderTarget`(UNORM SRV). `ImGuizmo.h`는 `imgui.h` 다음, Windows 헤더보다 먼저 포함
- 패널은 `FEditorContext`(비소유 포인터 + 선택 상태)만 받는 `Draw()` 클래스로 만들고, 씬 구조 변경(부모 변경/삭제)은 순회가 끝난 뒤 적용한다
- 병렬 작업: 독립 트랙은 서브에이전트를 git worktree로 띄워 브랜치에 커밋시키고 메인이 머지한다. `.claude/worktrees/`는 gitignore. 트랙마다 수정 허용 범위를 명시할 것
- 경고 = 에러 (`/W4 /WX`). 경고를 억제하지 말고 원인을 고친다
- 로그: `E_LOG(Category, Verbosity, "포맷 {}", 인자)` — std::format 문법. 카테고리는 헤더에서 `E_DECLARE_LOG_CATEGORY`, 하나의 .cpp에서 `E_DEFINE_LOG_CATEGORY`
- 검증: `E_CHECK(expr)`, `E_CHECKF(expr, "포맷", ...)` (실패 시 Fatal)
- D3D 호출: 초기화 경로는 `E_D3D_VERIFY(call)` (실패 시 Error 로그 + `return false`), 프레임 경로는 `E_D3D_CHECK(call)` (Fatal)
- 디버그 전용 코드는 `#if E_DEBUG` (CMake가 Debug=1 / Release=0 정의)
- 포스트 프로세싱: 효과는 `FPostProcessor` 안에서 확장하고(씬 렌더러는 `Render` 한 번 호출), 설정은 `FPostProcessSettings`, CPU/GPU 공용 식은 `Renderer/PostProcessMath.h`와 셰이더를 함께 수정. PSO 블렌드는 `FGraphicsPipelineDesc::BlendMode`(`bAlphaBlend`는 하위 호환용)

## 디렉터리

```
CMake/            CMake 헬퍼 모듈
Engine/Source/
  Core/           타입, 로그, 어설트, 타이머, 창(Win32), 입력, 애플리케이션 루프
  Core/Paths.h    FPaths: 엔진 디렉터리(실행 파일 상위에서 Engine/Shaders/Common.hlsli 마커 탐지), 프로젝트(.eproject) Content/Saved/Config
                  FCommandLine(--project 등), FProjectDescriptor(.eproject JSON)
  Core/Math/      FMath, FVector2/3/4, FQuat, FMatrix4x4, FBox, FFrustum (통합 헤더 Math.h)
  Core/ECS/       FEntity(세대 핸들), TSparseSet, FRegistry(+TView) — 자체 희소 집합 ECS
  Core/Containers/ THandle(태그별 세대 핸들), TResourcePool
  Core/Reflection/ FTypeRegistry/FTypeInfo/FPropertyInfo, TTypeBuilder — 인스펙터·직렬화·스크립트 바인딩 공용 타입 정보
  Core/Testing/   경량 단위 테스트 프레임워크
  Scene/          FScene(계층/트랜스폼 갱신), Components.h(Name/Transform/Hierarchy/StaticMesh/DirectionalLight), ResourceHandles.h
  RHI/D3D12/      디바이스, 커맨드 큐, 디스크립터 힙/할당자, 스왑체인, 깊이 버퍼, 동적 업로드 버퍼,
                  RHI 파사드, 셰이더 컴파일러(DXC), 루트 시그니처, PSO, 정적 버퍼, 텍스처, 밉 생성기
  RHI/ShaderLibrary.h  FShaderLibrary: 셰이더 바이트코드 공급(메모리 캐시 → Engine/Shaders/Cooked DXIL → DXC 컴파일). 셰이더는 항상 이걸로 얻는다
  RHI/ShaderManifest.h Shaders.json 파싱, 쿠킹 파일명 규칙(<스템>_<Entry>_<Stage>[_<디파인 해시>][.debug].dxil), #include 의존 파일 스캔
  Renderer/       카메라, 플라이 카메라 컨트롤러, 메시 데이터/프리미티브, FStaticMesh, 이미지 로더(stb_image),
                  FResourceManager(메시/텍스처/머티리얼 핸들 소유), FGltfLoader(cgltf)+FModelLoader(씬 배치),
                  FSceneRenderer(수집→컬링→정렬→드로우), ShaderTypes.h (cbuffer와 1:1 대응하는 CPU 구조체)
                  모듈 의존: Renderer → Scene → Core, Renderer → RHI → Core
  Editor/         FImGuiLayer, FEditorApplication, EditorContext, Panels/(Viewport/Hierarchy/Inspector/ContentBrowser)
Engine/Shaders/   HLSL (Common.hlsli 공통 헤더, Mesh.hlsl, GenerateMips.hlsl) + Shaders.json(쿠킹 매니페스트 — 새 셰이더/엔트리는 여기 추가). Cooked/는 생성물(git 제외)
Editor/Source/    ProjectEEditor 실행 파일 (main만)
Runtime/Source/   ProjectERuntime 게임 런타임 실행 파일 (창 서브시스템, `--project`로 프로젝트 지정)
Tools/Cook/       ProjectECook 쿠킹 도구 (셰이더 → DXIL, GPU 불필요)
Sandbox/Source/   엔진 검증용 런타임 데모 실행 파일
Projects/Sample/  예제 프로젝트 (Sample.eproject, Content/ 에셋). 인자 없이 실행하면 기본으로 열린다
Tests/            CoreTests, RendererTests, RhiTests (CTest 등록)
CMake/ThirdParty.cmake  FetchContent 외부 라이브러리 (커밋/해시 고정): stb_image, cgltf, imgui, ImGuizmo, nlohmann/json
Scripts/          빌드 스크립트 (Build.ps1), 패키징 스크립트 (Package.ps1)
Build/            CMake 빌드 출력 (git 제외)
```

새 모듈은 `Engine/Source/<Module>/CMakeLists.txt`에 `E<Module>` 정적 라이브러리 + `ProjectE::<Module>` 별칭으로 추가하고 `e_set_target_defaults`를 호출한다.

## 빌드

세 가지 방법이 있으며 모두 같은 CMake 설정을 쓴다. CMake/Ninja는 PATH에 없고 VS 2022 번들 버전을 사용한다.

1. **Visual Studio에서 폴더 열기 (권장)**: VS 2022 → `파일 → 열기 → 폴더`로 프로젝트 루트를 연다. `CMakePresets.json`의 `ninja-debug`가 자동 선택되고, 솔루션 탐색기의 "CMake 대상 보기"에서 모듈 트리가 보인다. 시작 항목을 `ProjectEEditor.exe`로 고르고 F5. 빌드는 Ctrl+Shift+B.
2. **.sln으로 빌드**: `GenerateSolution.bat` 더블클릭 → `Build\vs2022\ProjectE.sln`이 생성되어 열린다. 시작 프로젝트는 `ProjectEEditor`로 설정되어 있고 F5로 빌드+실행. 파일/모듈을 추가한 뒤에는 이 배치를 다시 실행하거나 VS의 ZERO_CHECK 프로젝트가 자동 재생성한다. 산출물은 `Build\vs2022\Bin\<Debug|Release>\`.
3. **더블클릭**: 루트의 `Build.bat`(빌드), `RunEditor.bat`(빌드 후 에디터 실행).
4. **명령줄**:

```powershell
.\Scripts\Build.ps1                  # 구성 + Ninja Debug 빌드
.\Scripts\Build.ps1 -Config Release
.\Scripts\Build.ps1 -Run             # 빌드 후 에디터 실행
.\Scripts\Build.ps1 -RunSandbox      # 빌드 후 런타임 데모 실행
.\Scripts\Build.ps1 -Test            # 빌드 후 단위 테스트 (ctest)
.\Scripts\Build.ps1 -VisualStudio    # .sln 생성 (Build\vs2022\ProjectE.sln)
.\Scripts\Package.ps1 [-Project Projects\Sample] [-Config Release]  # Release 빌드 → 셰이더 쿠킹 → Build\Package\<프로젝트>\ 스테이징 (Run.bat 포함)
```

수동(VS 개발자 명령 프롬프트): `cmake --preset ninja-debug` → `cmake --build --preset ninja-debug`. 실행 파일은 `Build/ninja-<config>/Bin/` 아래 `ProjectEEditor.exe`(에디터), `Sandbox.exe`(런타임 데모).

참고: 2026-09-30 VS Installer 복구 이후 `vs2022` 프리셋(.sln 생성)도 정상 동작한다. 이전에는 VS 설치 메타데이터 손상으로 CMake가 VS 인스턴스를 찾지 못했었다. 같은 증상이 재발하면 VS Installer의 "복구"로 해결한다.

셰이더만 빠르게 검증할 때는 SDK의 dxc.exe를 직접 사용한다 (`-HV 2021 -Zpr -WX -I Engine/Shaders`). 에디터 실행 중에는 `Engine/Shaders/*.hlsl|hlsli`를 저장하면 자동 반영(핫 리로드, 실패 시 기존 셰이더 유지)되고, Ctrl+R(도구 → 셰이더 다시 로드)은 강제 재컴파일한다.

화면 자동 검증: `.\Scripts\Verify.ps1 [-Target Editor|Runtime|Sandbox] [-ExtraArgs "--select <이름>"] [-Name <이름>]` → `Saved/Verify/<이름>.png`와 로그, D3D12 디버그 레이어 오류/경고 요약(종료 코드 0 = 오류 없음). 앱 공통 인자: `--exit-after <N>`, `--screenshot <경로>`, `--log <경로>`, 에디터 `--select <엔티티 이름>`. 렌더링/에디터 변경 후에는 반드시 실행해 스크린샷을 직접 보고, 디버그 레이어 오류 0건을 확인한 뒤 커밋한다.

빌드/단위 테스트 실행은 사용자 승인(2026-09-29)에 따라 이 프로젝트에서 항상 자동으로 수행한다. 에디터/Sandbox 실행(화면 확인)과 외부 다운로드는 사용자에게 안내/확인한다.

## 검증 체크리스트 (구현 후)

- [ ] `/W4 /WX` 통과
- [ ] 디버그 레이어 에러/경고 없음, 종료 시 라이브 오브젝트 보고에 누수 없음
- [ ] 리사이즈·최소화·포커스 전환에서 크래시 없음
- [ ] `Plans.md` 상태 갱신
