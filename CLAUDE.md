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
- 외부 라이브러리 추가는 `CMake/ThirdParty.cmake`에서 커밋/해시 고정, `ThirdParty::<name>` 별칭으로 링크. 선언은 `FetchContent_Declare/MakeAvailable` 대신 `e_fetchcontent_declare/e_fetchcontent_make_available`(소스를 `Build/_deps/<이름>-src`에 한 번 받아 모든 빌드 폴더가 공유, 이후 다운로드·하위 빌드 생략, 선언/패치 파일이 바뀌면 자동으로 다시 받음 — 강제로 받으려면 `Build/_deps/<이름>.populated` 삭제). 서드파티 헤더 포함부는 `#pragma warning(push, 0)`으로 감싼다
- 리플렉션: 새 컴포넌트는 `Scene/SceneReflection.cpp`의 `RegisterSceneTypes()`에 `RegisterType<T>(...).Property(...).AsComponent()`로 등록해야 인스펙터/직렬화에 나타난다. 파생 값(WorldMatrix 등)은 등록하지 않는다. 프로퍼티 타입은 `EPropertyType`에 있는 것만 지원. 문자열 선택지 콤보는 `.StringOptions(공급자)`(인자 없음) 또는 같은 오브젝트의 다른 값에 따라 바뀌면 `.StringOptionsFor([](const T& 컴포넌트) {...})`(우선, 결과가 비면 인스펙터는 일반 문자열 칸 — `FPropertyInfo::GetStringOptions(Object)`, 테스트 `Reflection_StringOptionsProviders`)
- ECS: 컴포넌트는 POD에 가까운 struct, 로직은 시스템(뷰 순회)에 둔다. `View<...>().Each` 순회 중 같은 타입 컴포넌트 추가/제거 금지. 계층 변경은 반드시 `FScene::SetParent`
- 리소스 수명: GPU 리소스는 `FResourceManager` 핸들로만 참조하고 직접 소유하지 않는다. 렌더링 중 삭제는 `Destroy*`(지연 해제)로, 즉시 `Shutdown()`은 GPU Flush 이후에만
- 색공간: 백버퍼 RTV와 색상 텍스처는 sRGB 포맷, 셰이더는 선형 공간에서 계산. 데이터 텍스처(노멀 등)는 UNORM
- 머티리얼(PBR, glTF 금속/거칠기): 텍스처 슬롯 t0~t4 = 베이스(sRGB)/금속거칠기(선형, G=거칠기 B=금속)/노멀(선형)/AO(선형 R)/발광(sRGB). 머티리얼마다 셰이더 가시 힙에 연속 5칸 테이블(`FMaterial::TextureTable`)을 두며 `FResourceManager`만 만들고 갱신한다. 머티리얼 텍스처 핸들을 바꾸면 `RefreshMaterialTextures` 호출
- 텍스처 쿠킹: 파일 텍스처는 `FResourceManager::LoadTexture(Path, ETextureUsage)` → `FAssetCache::LoadTextureAsset`(용도별 `.etex`: 전체 밉 + BC7(색상 sRGB/선형)·BC5(노멀 XY)·BC4(마스크)). 노멀 맵은 셰이더가 XY에서 Z를 재구성하므로 B 채널에 의존하지 않는다. 밉 0이 4의 배수가 아니면 RGBA8. 형식을 바꾸면 `TextureVersion`을 올린다
- 탄젠트: `FVertex::Tangent.xyz` = UV +U 방향, `B = Cross(N, T) * Tangent.w` = 노멀 맵 +Y = UV 위쪽(-V). glTF TANGENT는 `FGltfLoader::ConvertTangent`(xyz 변환 + w 반전 — 축 반사 때문), 없으면 `FMeshData::ComputeTangents`
- glTF 임포트: 좌표 변환은 `FGltfLoader::Convert*`만 사용(쿼터니언 벡터부 부호 반전). 축 반사와 엔진 카메라 규약의 반사가 상쇄되어 화면상 와인딩은 유지되므로 로더가 인덱스 순서를 뒤집어 CW 앞면으로 맞춘다. 와인딩 규약 검증: `Cross(P1-P0, P2-P0)·Normal > 0`. 새 속성 추가 시 `GltfLoaderTests`에 케이스 추가
- 테스트 매크로 인자에 템플릿 쉼표(`View<A, B>()`)를 직접 넣지 말고 지역 변수로 받는다
- ImGui: UI는 `FImGuiLayer::BeginFrame()`~`EndFrame()` 사이에서만 기술하고, UI 드로우는 UNORM 백버퍼 뷰(`SetRenderTargetToBackBuffer(true)`)에 그린다. 뷰포트에 표시할 오프스크린 타깃은 `FD3D12RenderTarget`(UNORM SRV). `ImGuizmo.h`는 `imgui.h` 다음, Windows 헤더보다 먼저 포함
- 패널은 `FEditorContext`(비소유 포인터 + 선택 상태)만 받는 `Draw()` 클래스로 만들고, 씬 구조 변경(부모 변경/삭제)은 순회가 끝난 뒤 적용한다
- 수학/선택: `FMatrix4x4::TryGetInverse`의 특이 판정은 상대 기준(|det| / 행 길이 곱)이다 — 직교 뷰-투영은 det ≈ 1e-11이므로 절대 문턱을 다시 넣지 말 것(`Matrix_InverseOfOrthographicViewProjection`). 에디터 클릭 선택은 AABB로 거른 뒤 CPU 정점 삼각형으로 판정하고 스킨 메시만 AABB(검증 `--verify-pick <이름,...> [--verify-pick-ortho|--verify-pick-no-focus]`)
- 에디터 씬 편집 코드는 변경 직후 `Context.MarkEdited("라벨")`을 호출한다 → 조작(드래그/텍스트 입력/기즈모)이 끝나면 씬 JSON 스냅샷이 Undo 한 단계로 커밋된다. 선택은 `Context.Select*`/`ToggleSelection`/`SelectMany`로만 바꾼다(`SelectedEntity`=주 선택, `Selection`=전체). 여러 선택 대상 명령은 `FEditorActions`/`FSceneEditOps`(최상위 필터)를 쓴다. 에디터 인자 `--select A,B`(같은 이름 모두), `--verify-undo`(복제/Undo/Redo 자동 검증)
- 에디터 UI 테마: 색/아이콘은 `FEditorTheme`(Editor/EditorTheme.h)의 상수·함수를 쓴다(하드코딩 색 대신 `Accent`/`Success`/`Warning`/`Danger`). 아이콘은 `ICON_FA_*`(Font Awesome 6 Solid). 도킹 창 제목은 `FEditorTheme::PanelTitle(아이콘, "표시 이름", "영문Id")`로 만들어 ID를 고정하고, 새 패널은 `ApplyDefaultLayoutIfNeeded`의 기본 배치에도 넣는다
- 콘텐츠 브라우저(`Editor/ContentBrowser/`, `Panels/ContentBrowserPanel`): 파일 이동/이름 변경은 반드시 `FContentBrowserPanel::MoveAssets`/`RenameAsset` 경로를 거친다 (디스크 이동 → `FAssetReferenceUpdater::UpdateAfterMove` → `Context.AssetsMoved`: 리소스 캐시 키/열린 씬/실행 취소 기록/현재 씬 경로 갱신). 새 에셋 형식이 다른 파일을 경로로 참조하면 `AssetReferenceUpdater`의 기준 규칙(Content 기준 vs 파일 폴더 기준)에 확장자를 추가한다. 에셋 경로 문자열 프로퍼티는 리플렉션에 `.AssetFilter(".확장자")`를 붙이면 인스펙터가 드롭을 받는다. 콘텐츠 드래그 페이로드는 `FContentDragDrop`(경로 목록)
- 에셋 편집 창(`Editor/AssetEditors/`): 새 편집기는 `FAssetEditor`를 상속해 `LoadAsset`(되돌리기에서도 다시 불림 — 여러 번 호출 안전)/`SaveAsset`/`CaptureState`/`RestoreState`(에셋 JSON)/`DrawProperties`를 구현하고 `FAssetEditorManager`의 `CreateEditor`/`CanOpen`에 확장자를 등록한다. 미리보기는 창 소유 `FAssetPreview`(전용 씬)를 매니저의 전용 렌더러로 그리며 `Context.Scene`/`Context.Camera`를 쓰지 않는다. 편집 후 `MarkEdited`(창 안 Undo 한 단계). 공유 리소스(머티리얼/파티클 설정)를 실시간으로 고치는 편집기는 닫을 때 저장 안 한 변경을 파일 상태로 되돌린다. 3D 미리보기가 필요 없는 편집기(노드 그래프 등)는 `UsesPreview() = false` + `DrawPreviewArea`을 구현한다(예: `FBehaviorTreeEditor`)
- 작업 방식(2026-10-04 사용자: "왜 이렇게 오래 걸리냐"): **동시에 도는 작업이 없으면 워크트리 없이 메인 체크아웃에서** 한다(새 워크트리는 엔진 전체 빌드 약 8분 + 셰이더·모델 재쿠킹이 매번 든다). 큰 작업은 진단 결과를 먼저 짧게 보고한 뒤 수정하고, 성능 3회 측정은 마지막 확인 때만.
- **재사용 워크트리(2026-10-04 사용자 요청)**: 병렬 트랙은 새 워크트리를 만들지 않고 고정 작업 공간 3개 `lane-a`/`lane-b`/`lane-c` = `.claude/worktrees/lane-<a|b|c>`(브랜치 이름도 같음, 항상 3개 유지)를 재사용한다 — 빌드 폴더·쿠킹 캐시·`CMakeLocal.cmake`가 남아 증분 빌드만 든다. 트랙 시작 = `git -C <lane> checkout -B <작업 브랜치> master`(서브에이전트는 isolation 없이 띄우고 "모든 명령을 <lane 경로>에서" 지시), 머지·push가 끝나면 `git -C <lane> checkout -B <lane 이름> master` + `git clean -fd -e Build -e CMakeLocal.cmake -e Cooked -e Saved`로 되돌리고 작업 브랜치는 `git branch -d`. 트랙이 4개 이상 필요할 때만 임시 워크트리를 만들고 머지 뒤 바로 `git worktree remove`(지우기 전 정션 확인).
- 병렬 작업: 독립 트랙은 서브에이전트를 git worktree로 띄워 브랜치에 커밋시키고 메인이 머지한다. `.claude/worktrees/`는 gitignore. 트랙마다 수정 허용 범위를 명시할 것. 워크트리 빌드는 서드파티 소스를 **gitignore된 `CMakeLocal.cmake`의 `set(E_THIRDPARTY_SOURCE_CACHE E:/Projects/ProjectE/Build/_deps)`로만** 공유한다 — `Build/_deps`를 정션/심볼릭 링크로 잇지 않는다(워크트리를 지울 때 링크 너머 메인 소스까지 지워짐). 워크트리를 지우기 전에 재분석 지점(정션)이 없는지 확인한다
- 경고 = 에러 (`/W4 /WX`). 경고를 억제하지 말고 원인을 고친다. `/W4 /WX`는 자체 타깃에만 `e_set_target_defaults`가 건다(전역 `add_compile_options`에 경고 레벨을 넣지 않는다 — 서드파티 `/W0`와 겹쳐 D9025). 서드파티가 전역 플래그와 반대되는 옵션을 쓰면(GNS `/EHs-c-`, Jolt `/Zi`) 그 타깃에서만 정리한다
- 로그: `E_LOG(Category, Verbosity, "포맷 {}", 인자)` — std::format 문법. 카테고리는 헤더에서 `E_DECLARE_LOG_CATEGORY`, 하나의 .cpp에서 `E_DEFINE_LOG_CATEGORY`
- 검증: `E_CHECK(expr)`, `E_CHECKF(expr, "포맷", ...)` (실패 시 Fatal)
- D3D 호출: 초기화 경로는 `E_D3D_VERIFY(call)` (실패 시 Error 로그 + `return false`), 프레임 경로는 `E_D3D_CHECK(call)` (Fatal)
- 디버그 전용 코드는 `#if E_DEBUG` (CMake가 Debug=1 / Release=0 정의)
- 오디오: 사운드 위치는 `FAudioEngine::SetWorldPosition`(청자 기준 좌표로 변환 — miniaudio는 오른손 좌표계라 월드 좌표를 직접 넘기지 않는다). 오디오 컴포넌트는 Audio 모듈의 `RegisterAudioTypes()`로 등록하며 앱은 씬 로드 전에 호출. 자동 검증(`IsAutomationRun()`) 중에는 음소거. 오디오 테스트는 `bNoDevice` 엔진 + `ReadFrames`로 믹스 결과를 검증
- 플레이 모드: `FPlayMode`가 `FSceneCloner`로 편집 씬을 복제해 재생하고 정지 시 복원한다. 복제는 리플렉션 `CopyComponent` 훅을 쓰므로 **리플렉션에 등록되지 않은 컴포넌트는 플레이 씬에 복사되지 않는다**. 예외로 런타임 전용 데이터(`FSkinComponent`, `FAnimationComponent::Runtime`)는 `FSceneCloner::CopyRuntimeData/RemapRuntimeReferences`가 복사·재매핑하며, 새 런타임 전용 엔티티 참조를 추가하면 여기에 함께 추가한다(서브트리 복제 `FSceneEditOps::CloneSubtree`도 같은 함수 사용) 플레이 중 에디터 코드는 `Scene` 멤버가 아니라 `Context.Scene`(현재 씬)을 사용한다
- 맵 전환/서브 씬(Phase 31): 게임 중 맵 전환은 `FGameWorldTravel::Travel`(`World/GameWorldTravel.h`)만 쓴다(앱마다 순서를 복제하지 않는다). 씬 객체는 바꾸지 않고 내용만 교체한다. 서브 씬은 `FGameWorld` API로만 붙이고 내린다(루트 엔티티 아래, `FTransientComponent`로 메인 씬 저장 제외). 백그라운드 스레드는 파일 읽기/JSON 파싱만, ECS·GPU 작업은 메인 스레드. 서브 씬 안 복제 엔티티 NetId는 하위 트리 순서로 정하므로 서버/클라이언트가 같은 서브 씬 파일을 가져야 한다. 이전 맵만 쓰던 경로 리소스는 수거된다(Phase 37)
- 게임 월드 갱신 순서: 새 게임 시스템은 앱마다 따로 부르지 말고 `FGameWorld`(World/GameWorld.h)에 넣는다. `TickGameplay`(플레이 중: 스크립트 → 에셋 해석 → 게임 모듈 → AI → 물리 → UpdateTransforms, Client 역할은 게임 모듈·AI 없음), `TickPresentation`(항상: 애니메이션 → UpdateTransforms → 파티클), `BeginPlay`(물리 → 게임 모듈 → 스크립트 → AI)/`EndPlay`(역순). 시스템은 앱이 소유하고 `FGameWorldSystems`로 비소유 참조(Resources가 없으면 에셋 해석 생략 = GPU 없는 서버)
- 화면·시뮬레이션 시간 = 앱 프레임 시간(2026-10-04): 자동 노출 적응, 물·구름·머티리얼 `Time`·쿠키 패닝, 스트리밍 내림 지연처럼 **시간에 따라 바뀌는 화면/시뮬레이션 상태는 실제 시각(`std::chrono::steady_clock`)을 쓰지 않고** `Core/FrameTime.h`의 `FFrameTime::GetDeltaSeconds/GetTotalSeconds`(엔진 DLL 전역, `FApplication`이 프레임·헤드리스 틱마다 `FTimer` 델타로 진행 — `--fixed-delta`면 부하와 무관하게 실행마다 같은 화면)나 갱신 함수로 받은 dt를 쓴다. 렌더러가 "지난 호출 이후 경과"가 필요하면 `GetTotalSeconds()` 차이로 잰다. 실제 시각은 측정(프레임 간격·CPU/GPU 구간·로딩 시간·업로드 속도·로그 주기)에만. 앱 루프 밖(테스트·도구)에서는 0에 멈춰 있다
- 프레임 제한: 에디터 VSync는 환경설정 뷰포트 `bVSync`(기본 끔, 통계 창 체크박스도 저장) — 켜면 모니터 주사율로 고정. 런타임은 프로젝트 설정 Display `VSync` + 사용자 `GameUserSettings`, 측정은 `--no-vsync`

## 영역별 상세 규칙 (`Docs/Rules/`)

**해당 영역의 코드·에셋을 고치기 전에 그 문서를 반드시 읽는다.** 아래 요약은 가장 자주 어기는 규칙만이고 상세(테스트 이름·검증 인자·측정치·배경)는 문서에 있다. 새 규칙은 문서나 관련 머리 주석에 쓰고 여기에는 한 줄 요약만 더한다(이 파일이 커지면 매 세션 컨텍스트를 잡아먹는다).

### [`Docs/Rules/Engine.md`](Docs/Rules/Engine.md) — 엔진 코어 (트랜스폼·엔진 DLL·게임 모듈·프리팹)

- 트랜스폼 갱신 캐시: `WorldMatrix`를 직접 쓰고 되돌리지 않는 코드는 `InvalidateWorldCache()`, 계층 컴포넌트를 `SetParent` 밖에서 고치면 `FScene::NotifyHierarchyChanged()`. `FMatrix4x4::operator*` SSE는 스칼라와 비트 동일(FMA 금지)
- 뼈 트랜스폼: 진실은 엔티티 로컬 TRS, 월드는 `FScene` 갱신만. 표시 틱에서 트랜스폼을 쓰는 새 코드는 쓴 엔티티를 `PresentationWritten`에 넣고, 앱은 `TickGameplay → TickPresentation` 사이에 트랜스폼을 쓰지 않는다
- 엔진 DLL: 런타임 모듈은 `ProjectEEngine.dll` 하나, 실행 파일/테스트/게임 모듈은 `ProjectE::Engine`만 링크. DLL 밖에서 쓰는 전역 데이터는 `E_ENGINE_API`, 내보내기 한도 65535
- 게임 모듈: `IGameModule` + `E_IMPLEMENT_GAME_MODULE`, `OnLoad`에서 리플렉션 등록. 인터페이스를 바꾸면 `GameModuleApiVersion`을 올린다
- 게임 모듈 핫 리로드: 모듈 타입 ID를 캐시해 다시 로드 너머로 쓰지 않는다(리플렉션으로만), 씬 밖 리플렉션 캐시는 `ReloadGameModule`에서 비운다. CMake 구성 생성 파일은 `file(CONFIGURE)`
- 프리팹: 원본을 바꾸는 코드는 반드시 `Context.ChangePrefab`, Lua 생성은 `Scene.SpawnPrefab`

### [`Docs/Rules/RenderingPipeline.md`](Docs/Rules/RenderingPipeline.md) — 렌더링 파이프라인 (렌더 그래프·PSO·렌더 스레드·성능·화면 품질·리소스)

- 렌더 그래프: 새 패스는 그래프 패스로만, 람다 안 `ResourceBarrier` 금지, 루트에 묶기만 하는 것까지 모든 리소스를 선언, 중간 버퍼는 `CreateTexture`
- PSO 캐시: PSO/루트 시그니처는 `FD3D12PipelineState::Init*`/`FD3D12RootSignature`로만. 존재 확인은 `IsInitialized()`(Get은 만든다). 드라이버 라이브러리 캐시는 기본 끔(틀린 화면 재현)
- 렌더 스레드(`r.RenderThread` 기본 1, 런타임만): 그래프 패스 람다는 씬·게임 전역을 읽지 않는다(값 캡처). 공유 상태를 바꾸는 게임 스레드 코드는 직전에 `RenderThreadSync::WaitForRenderThread()`(`FResourceManager` 변경 함수 첫 줄)
- 성능: 측정 `--perf-capture`(런타임 `--no-vsync`). 정적 메시 패스는 `FMeshInstanceList` + `FMeshPassBatches`(인스턴스 번호 오름차순 Add), LOD는 `SelectLods` 공유, 메시 쿠킹 형식을 바꾸면 `ModelVersion`
- 규모 목표(스킨 1000유닛 60fps): 성능 변경은 `Scripts\ScaleBench.ps1`로 전후 비교. 렌더러 수집 병렬은 뷰 순서 유지·자기 칸만 쓰기
- TAAU/동적 해상도: 씬 컬러를 다루는 패스는 내부 해상도(`RenderSceneColor`의 Width/Height), TAAU 이후는 출력 해상도. 머티리얼 텍스처 샘플은 `MaterialMipBias`, 오버레이 깊이는 `GetOverlayDepthDsv`
- 화면 품질 파이프라인: 새 메시 경로는 메인과 같은 VS로 사전 패스에 반드시 그리고, `ApplyDecals`/`EvaluateImageBasedLighting`을 같은 자리에서. 새 화면 공간 패스는 `FScreenPassRootSignature`
- 시간 안정성: 픽셀·프레임마다 흔드는 효과보다 결정적 필터 우선(SSR 확률 반사 금지), 지터 없는 오버레이는 음의 깊이 바이어스. 안정성 검증 방법(32프레임 표준편차 지도). TAA 깜빡임 감지(TSR식 뒤집힘 빈도, `r.TAA.FlickerReduction`) — 통계는 정지 화소에서만 쌓는다
- 포스트 프로세싱: 효과는 `FPostProcessor` 안에서, CPU/GPU 공용 식은 `PostProcessMath.h`와 함께. 프로젝트 기본 후처리는 설정 Rendering(`ApplyProjectPostProcessDefaults`). 피사계 심도는 씬 `DepthOfFieldComponent`(TAA 뒤·블룸 앞, 편집 카메라는 `PreviewInEditor`일 때만, 틸트시프트·다각형 보케 포함), 색 보정·비네트는 씬 `ColorGradingComponent`(LUT 굽기)·`VignetteComponent`(톤매핑 직후)
- 픽셀 아트: `PixelArtMath.h`와 셰이더를 함께, 물체 도트 스냅은 렌더 동안만. 픽셀 아트에서 화면 고정 노이즈·반해상도 효과 금지
- 텍스처 밉 스트리밍: 머티리얼 밖 텍스처는 공개 `LoadTexture`(고정), 텍스처 SRV/디스크립터를 프레임 넘겨 캐시 금지, 스트리밍 켬 화면 = 끔 화면 비트 동일
- 리소스 수거: 씬 밖에서 경로 핸들을 오래 드는 새 코드는 `FResourceManager::AddRootProvider`, 새 씬 컨테이너는 `FResourceRoots::AddScene`
- 비동기 업로드: 정적 데이터는 `FD3D12UploadQueue`, 메시는 `FStaticMesh::IsReady` 확인, 텍스처는 `ResolveTexture`/머티리얼 테이블로만. 작업 스레드는 파일·디코드·파싱만

### [`Docs/Rules/RenderingLighting.md`](Docs/Rules/RenderingLighting.md) — 렌더링 조명 (RT·DDGI·RTAO·면광원·하늘·로컬 라이트·그림자 캐시)

- 레이 트레이싱(DXR 1.1 인라인): 가속 구조는 `FRayTracingScene`만, 추적 패스는 픽셀 셰이더 + `DeclareTraceReads`, 엔티티마다 그래프 리소스 Import 금지, RT 초기화는 `EnsureRayTracing` 지연
- DDGI: 새 메시 패스는 b9 + t40~t42 바인딩과 `Ddgi.DeclareShadingReads`를 함께. 조도 아틀라스 RGBA32F 유지, 식은 `DdgiMath.h` ↔ hlsl
- RTAO: 결과는 SSAO와 같은 형식(t16), 파라미터는 경로 추적 기준(`--rtao-reference`)과 rmse로 정한다
- 면광원/IES/쿠키: `AreaLightMath.h` ↔ hlsl 같은 식. 새 메시 패스·루트 시그니처도 공간 3 무제한 표(`RootParam_LightTextures`)를 묶는다
- 하늘·대기/구름/물/HDR: 대기 안은 km(`WorldToAtmosphere`로만), 방향광은 `PerFrame.DirectionalLight`, 조명 표는 `FIblRenderer::GetLightingTable()`, 씬 출력은 `Rhi->GetSceneOutput()`
- 로컬 라이트: `LightMath.h` ↔ `Lighting.hlsli` 함께(+`LightTests`), 새 메시 패스는 b5/t9~t12 바인딩. 정반사 배율 `SpecularScale`(Flags 비트 8~15 감소량) — 로컬 라이트를 평가하는 새 경로도 정반사 항에 곱한다
- 방향광 그림자 캐시: 새 그림자 캐스터 종류는 정적/동적을 알린다(`bShadowStatic` 또는 상태 해시), 해시에 안 담기는 변경은 `InvalidateCache()`, 병렬 단계에 콜백·리소스 생성 금지

### [`Docs/Rules/RenderingMeshes.md`](Docs/Rules/RenderingMeshes.md) — 렌더링 메시·머티리얼 (스킨·스킨 캐시·블렌드·머티리얼 그래프·파티클·지형)

- 스켈레탈 애니메이션: 팔레트 = `InverseBind * JointWorld`(t15, 월드 공간), `FSkinnedMeshPalette::Build` 프레임당 한 번. 새 메시 패스는 스킨 경로(`DrawSkinned` + 스킨 PSO, 스킨 캐시 변형 포함)도 함께
- 스킨 캐시(`r.SkinCache`, 기본 끔): 새 메시 패스는 `#ifdef E_SKIN_CACHE` VS 경로 + t15 `FSkinDrawSource` + `DeclareRead`, 인스턴스 LOD보다 고운 LOD로 그리는 패스 금지, 스키닝 식은 세 셰이더 함께
- 스킨 인스턴싱/컬링: 새 그림자 패스는 팔레트 가시성 판정에도 넣는다, 동적 업로드 버퍼 용량 주의
- 머티리얼 블렌드/인스턴스: 해석은 `FMaterialAsset::Resolve`만, 코드에서 고칠 땐 `ApplyMaterialAsset`. 새 메시 패스는 PSO 변형 8개와 Masked clip 지원
- 머티리얼 그래프: 새 노드는 `GetNodeTable` + 테스트 2종, 머티리얼 셰이더에서 ddx/ddy·`.Sample` 직접 금지(`MATERIAL_SAMPLE`), 새 메시 패스는 `EvaluateMaterial` 하나로
- 파티클(.eparticle v2): `EParticleModuleType`은 끝에만 추가, `Particles.cpp`와 `ParticleSimulate.hlsl`을 같은 식으로, 입력 순서 = 난수 흐름이라 중간 삽입 금지
- 지형/식생: 데이터를 바꾸면 `MarkChanged`, Undo는 스트로크 끝에만, 폴리지는 `AddExternal`(NullEntity 금지), 새 그림자 캐스터는 `FShadowCasterHook`

### [`Docs/Rules/2DPhysics.md`](Docs/Rules/2DPhysics.md) — 2D 물리 (Box2D·타일맵 충돌·2D 관절·2D 캐릭터 이동기·2D 물리 예측)

- 2D 물리(평면 X·Z, 카메라 +Y→-Y)·타일맵 충돌·2D 관절·2D 캐릭터 이동기·2D 물리 예측: Box2D 헤더·m 변환은 `Physics2DWorld.cpp` 안에서만, Box2D 콜백은 읽기만, 스크립트는 2D 캐릭터 이동을 계산하지 않고 입력만

### [`Docs/Rules/2DRendering.md`](Docs/Rules/2DRendering.md) — 2D 스프라이트/타일맵 렌더링과 데이터

- 2D 스프라이트 렌더링: 패스 위치 = 안개 뒤·반투명 메시 앞, 메시 루트 시그니처 재사용, 수집은 그림자 패스 등록 전. 새 2D 그리기 종류는 `FSpriteDrawItem` 또는 청크 메시. TAA: Masked는 움직임 벡터 + 반응형 0, 반투명은 정지면 알파 보존 PSO — 새 2D 그리기도 같은 규칙(HD-2D). 빌보드 `SpriteComponent.Billboard`(전체/세로축 — 수집기가 카메라 축으로 회전만 바꿈)
- 2D 스프라이트/타일맵 데이터: 읽기는 `FSprite2DLibrary::Get().Load*`, enum은 끝에만 추가, 게임플레이 타일 편집은 `MarkTilemapEdited` 지연 커밋

### [`Docs/Rules/2DEditor.md`](Docs/Rules/2DEditor.md) — 2D 에디터와 2D 에셋 편집기

- 2D 에디터(2D 뷰포트·선택·충돌 외곽선·타일 칠하기): 타일 칠하기 Undo는 스트로크 끝에만(끄는 중 MarkEdited 금지)
- 2D 에셋 편집기: 편집 중 라이브러리를 고치지 않는다(저장 = `FSprite2DLibrary::Save*`), 구조 변경은 `Sprite2DEditing`, 미리보기 텍스처는 편집기 소유 UNORM

### [`Docs/Rules/Crypt2D.md`](Docs/Rules/Crypt2D.md) — Crypt2D 예제 프로젝트

- Crypt2D 예제: 에셋은 `FetchAssets.ps1`, 스프라이트·데이터·씬·`Rooms.lua`는 생성물(`Tools/BuildCrypt2D.py`를 고쳐 다시 만듦), 자동 검증 씬 5종

### [`Docs/Rules/FarmBie.md`](Docs/Rules/FarmBie.md) — FarmBie 예제 프로젝트 (농장 + 좀비 디펜스)

- FarmBie: 콘텐츠는 `Projects/FarmBie/Tools/BuildFarmBie.py` 생성물, 거의 수직 탑뷰라 캐릭터는 전체 빌보드, 대량 처리는 게임 모듈(C++)·흐름은 Lua, 자동 검증 `Scenes/Tests/Farm*`

### [`Docs/Rules/Animation.md`](Docs/Rules/Animation.md) — 애니메이션 (그래프·몽타주·IK·노티파이·리타기팅·루트 모션·병렬 갱신)

- 애니메이션 그래프(.eanimgraph): 재생·전이·노티파이 규칙은 `AnimGraph.h` 머리 주석, 파라미터는 비복제
- 애니메이션 고급(2D 블렌드·레이어·몽타주·IK): 몽타주는 로컬 전용, 발 IK 바닥 탐색은 World(Scene → Physics 의존 금지)
- 애니메이션 갱신 병렬/URO: 평가 단계에서 로그·ECS 구조 변경·콜백·다른 엔티티 쓰기 금지
- 노티파이/소켓: 모델별 편집 데이터는 `<모델>.emeta`, 로컬↔월드 변환은 `FScene::GetParentWorldMatrix`
- 애니메이션 그래프/시퀀서 파일을 바꾸면 `FAnimGraphLibrary`/`FSequenceLibrary::Invalidate`, 씬을 직렬화하는 새 코드는 `SwapScenePreviews`로 감싼다
- 리타기팅: 클립 번호를 기억하는 코드는 세트가 바뀌면 이름으로 다시 찾고, 매핑을 바꾸면 `FAnimRetargetLibrary::Invalidate`
- 루트 모션: 추출·섞기는 `RootMotionMath`만, 새 포즈 원천도 제자리화

### [`Docs/Rules/Gameplay.md`](Docs/Rules/Gameplay.md) — 게임플레이·물리·캐릭터·입력·AI

- 물리(Jolt): cm↔m 변환은 `FPhysicsWorld` 경계에서만, 중력만 -Z, 스크립트가 만든 엔티티의 바디는 다음 물리 갱신에 생긴다
- 충돌 알림/관절/래그돌: Jolt 콜백에서 게임 코드를 부르지 않는다, Jolt 관절은 `FPhysicsWorld`만 소유
- 충돌 레이어(최대 16, 이름 저장): 새 바디/질의 코드는 `CollisionLayer` 칸을 채우고 질의 필터는 `FQueryLayerFilter`
- 플레이어 캐릭터/입력: 프리팹 자식도 `ReplicatedComponent`가 있어야 복제, 따라가는 카메라는 `OnLateUpdate`
- 캐릭터 이동/예측(3D, 넉백 포함): 스크립트는 `AddMovementInput`/`Jump`만, 새 클라이언트 앱은 `SetTransformFilter(!IsPredicted)`, 무브/ack 형식을 바꾸면 `NetProtocolVersion`
- 물리 예측: 새 클라이언트 앱은 `World.SetReplicationClient`를 BeginPlay 전에
- 입력 액션: 게임 코드는 키 대신 액션(Lua `Input.GetAction`, C++ `FInput::GetActionValue`)
- 입력 모드/플레이 빙의/마우스 커서: 앱은 `GetInputModeRouting`/`SelectGameInput`만, 뷰포트 편집 상호작용은 `Context.CanEditInViewport()`로 막는다
- 게임플레이(체력/게임 모드/세이브): 데미지·점수 변경은 서버만, enum은 끝에만 추가, 세이브는 `FSaveGame`만
- 능력 시스템(GAS식): 서버만 효과·속성 변경, 다른 대상 효과·스폰은 `ctx:HasAuthority()` 안에, `FLuaRuntime`의 새 sol 멤버는 소멸자에서 비운다
- 게임 시간 배율/히트스톱(Standalone 전용): 새 앱은 `FUISystem::Update` 전에 `GetUpcomingTimeScale()`
- AI(BT + Recast): Recast 헤더는 `NavMesh.cpp`에서만, 좌표 변환은 `NavCoordinates.h`, `.enav`/빌드 설정을 바꾸면 `NavFileVersion`
- 실내 절차적 생성: `GenerateBuilding`은 순수·시드 결정적(unordered 순회·표준 분포 금지), 씬 적용은 `FBuildingSceneBuilder`만

### [`Docs/Rules/Scripting.md`](Docs/Rules/Scripting.md) — 스크립팅 (Lua)

- 스크립팅(Lua 5.4 + sol2): sol 헤더는 Scripting 내부에서만, 바인딩 오류는 `std::runtime_error`, 새 컴포넌트는 리플렉션 등록만 하면 노출
- 스크립트 편의(Timer/Coroutine/모양 질의/디버그 선): 인스턴스 코드를 부르는 새 경로에는 `FLuaRuntime::FInstanceScope`
- 스크립트 모듈: 공용 Lua는 `Script.Require`만(모듈에 가변 상태 금지), 위젯 값을 쓰는 새 바인딩은 `MarkLayoutDirty`
- Lua 디버거: 새 Lua 실행 경로에 줄 훅을 상시로 걸지 않는다, 코루틴은 교체된 `coroutine.create`, 인스턴스 호출은 `Traceback` 오류 처리기

### [`Docs/Rules/Network.md`](Docs/Rules/Network.md) — 네트워크·복제

- 네트워크(GNS): GNS 헤더는 Network .cpp에서만, 메시지 형식을 바꾸면 `NetProtocolVersion`, 실제 소켓 테스트는 `AllowRealSockets()`, 새 앱은 `NetBindPolicy::Configure`. 멀티플레이 변경 후 `Verify.ps1 -Multiplayer`
- 복제: 클라이언트에서 트랜스폼을 직접 쓰지 않는다, 정적 NetId는 씬 로드 직후·게임 시작 전에 `Begin`

### [`Docs/Rules/Editor.md`](Docs/Rules/Editor.md) — 에디터 편집기 (머티리얼 노드·데이터)

- 노드 머티리얼 편집기: 그래프 구조 변경은 `MaterialGraphEditing`만
- 데이터 편집기: `FDataEditorBase` 상속, 값 위젯은 `DataValueWidgets::Draw` 공유

### [`Docs/Rules/UI.md`](Docs/Rules/UI.md) — 게임 UI·다국어

- 게임 UI(UMG식): 새 위젯 종류는 `EUIWidgetType` 끝에 추가하고 관련 파일을 함께, 레이아웃/입력 규칙을 바꾸면 `UITests`
- 다국어: 화면 글자는 `GetDisplayText`로, pak에서는 문자열 표를 `Localization.StringTables`에 적는다

### [`Docs/Rules/DataAndDemos.md`](Docs/Rules/DataAndDemos.md) — 데이터 테이블·RPG 데모·데모 맵

- 데이터 테이블/에셋: 읽기는 `FDataLibrary::Get().Load*`, 쓰기는 `Save*`, 구조체 변경은 `SaveStructAndMigrate`
- RPG 수치는 `Data/RPG/*`, 스크립트는 `RPGData.lua`로만 읽는다
- RPG 데모: 스크립트 간 계약 = `GameManager`/`Player`/적
- 데모 맵: 씬 파일을 직접 고치지 말고 `Tools/DemoMap/Build*.py`를 고쳐 다시 만든다, `Scenes/Tests/`는 데모용으로 고치지 않는다
- HD2D 낮밤·지역(항구): 새 맵은 `Regions.etable` 행 + 등불 `Night_#`/창 `NightWin_#` 이름 규칙, 시간에 따른 화면은 `DayNightKeys.etable`(스크립트 상수 금지)

### [`Docs/Rules/Packaging.md`](Docs/Rules/Packaging.md) — 패키징·Steam·설정·콘솔·프로파일링

- 배포/패키징: 런타임 쓰기는 `FPaths::GetSavedDirectory()` 아래, 콘텐츠 읽기는 `FFileSystem`만(ifstream 금지), 경로를 통째로 조립해 여는 에셋은 `Packaging.AdditionalAssets`
- Steam: Steam 헤더는 `SteamSubsystem.cpp`에서만, 런타임만 초기화
- 설정: 새 항목은 구조체 필드 + 등록부 `.Property(...)`만, `.eproject`에 정체성 외 값 추가 금지
- 콘솔/CVar: 디버그 토글은 `TAutoConsoleVariable`(도움말 필수), 렌더러 토글은 `RendererConsoleVariables.cpp`(Init에서 명령줄 파싱 금지)
- 프로파일링(Tracy): `Core/Profiling.h` 매크로만, 새 렌더 구간은 `ERenderTimer`에 추가

## 디렉터리

```
CMake/            CMake 헬퍼 모듈
Engine/Source/
  Core/           타입, 로그, 어설트, 타이머, 창(Win32), 입력, 애플리케이션 루프
  Core/Paths.h    FPaths: 엔진 디렉터리(실행 파일 상위에서 Engine/Shaders/Shaders.json 마커 탐지), 프로젝트(.eproject) Content/Saved/Config
                  FCommandLine(--project 등), FProjectDescriptor(.eproject JSON)
  Core/Math/      FMath, FVector2/3/4, FQuat, FMatrix4x4, FBox, FFrustum (통합 헤더 Math.h)
  Core/ECS/       FEntity(세대 핸들), TSparseSet, FRegistry(+TView) — 자체 희소 집합 ECS
  Core/Containers/ THandle(태그별 세대 핸들), TResourcePool
  Core/Reflection/ FTypeRegistry/FTypeInfo/FPropertyInfo, TTypeBuilder — 인스펙터·직렬화·스크립트 바인딩 공용 타입 정보
  Core/Testing/   경량 단위 테스트 프레임워크
  Scene/          FScene(계층/트랜스폼 갱신), Components.h(Name/Transform/Hierarchy/StaticMesh/DirectionalLight), ResourceHandles.h, GameModule.h/GameModuleHost(게임 모듈 DLL)
  RHI/D3D12/      디바이스, 커맨드 큐, 디스크립터 힙/할당자, 스왑체인, 깊이 버퍼, 동적 업로드 버퍼,
                  RHI 파사드, 셰이더 컴파일러(DXC), 루트 시그니처, PSO, 정적 버퍼, 텍스처, 밉 생성기
  RHI/ShaderLibrary.h  FShaderLibrary: 셰이더 바이트코드 공급(메모리 캐시 → Engine/Shaders/Cooked DXIL → DXC 컴파일). 셰이더는 항상 이걸로 얻는다
  RHI/ShaderManifest.h Shaders.json 파싱, 쿠킹 파일명 규칙(<스템>_<Entry>_<Stage>[_<디파인 해시>][.debug].dxil), #include 의존 파일 스캔
                       쿠킹 유효성은 파일 시각이 아니라 소스+include 내용 해시(`.dxil.srchash` 사이드카, `HashShaderSources`)
  Renderer/       카메라, 플라이 카메라 컨트롤러, 메시 데이터/프리미티브, FStaticMesh, 이미지 로더(stb_image),
                  FResourceManager(메시/텍스처/머티리얼 핸들 소유), FGltfLoader(cgltf)+FModelLoader(씬 배치),
                  FSceneRenderer(수집→컬링→정렬→드로우), ShaderTypes.h (cbuffer와 1:1 대응하는 CPU 구조체)
                  모듈 의존: Renderer → Scene → Core, Renderer → RHI → Core
  Audio/          FAudioEngine(miniaudio 래퍼, 장치 없으면 무음 계속), FAudioSystem(FAudioSourceComponent ↔ 사운드 동기화), AudioMath(청자 공간 변환), RegisterAudioTypes()
  Physics/        FPhysicsWorld(Jolt 래퍼, cm↔m), FPhysicsSystem(씬 동기화, 고정 스텝, 보간, 레이캐스트), 강체/콜라이더 컴포넌트 (모듈 의존: Physics → Scene → Core)
  AI/             BehaviorTree/(블랙보드, .ebt 에셋, 노드 레지스트리, 실행기, 기본 노드), Navigation/(FNavMesh: Recast 굽기 + Detour 경로, .enav), FAISystem(트리 실행 + 이동), AI 컴포넌트/태스크 (모듈 의존: AI → Scene → Core, World → AI)
  Scripting/      FScriptSystem(Lua 5.4 + sol2): 스크립트 컴포넌트 실행, 리플렉션 바인딩, 핫 리로드 (모듈 의존: Scripting → Scene → Core)
  World/          FGameWorld: 게임 월드 갱신 순서 한곳 (런타임/에디터 플레이/전용 서버 공용, 모듈 의존: World → Scripting/Physics/Renderer)
  Network/        멀티플레이: INetTransport(GNS / 루프백), FNetDriver(연결 수립·플레이어), NetMessages(프로토콜), FNetLaunchOptions (모듈 의존: Network → Core)
  UI/             게임 UI: 위젯 트리/레이아웃/SDF 글꼴/.eui 에셋/그리기 목록/입력 라우팅 (모듈 의존: UI → Core, Renderer → UI)
  Editor/         FImGuiLayer, FEditorApplication, EditorContext, FPlayMode(재생/정지/복원), Panels/(Viewport/Hierarchy/Inspector/ContentBrowser)
Engine/Shaders/   HLSL (Common.hlsli 공통 헤더, Mesh.hlsl, GenerateMips.hlsl) + Shaders.json(쿠킹 매니페스트 — 새 셰이더/엔트리는 여기 추가). Cooked/는 생성물(git 제외)
Editor/Source/    ProjectEEditor 실행 파일 (main만)
Runtime/Source/   ProjectERuntime 게임 런타임 실행 파일 (창 서브시스템, `--project`로 프로젝트 지정)
Server/Source/    ProjectEServer 전용 서버 실행 파일 (콘솔, 창/GPU 없음 — `FApplicationDesc::bHeadless` 고정 틱 루프 + `FGameWorld`, `--exit-after`는 틱 수)
Tools/Cook/       ProjectECook 쿠킹 도구 (셰이더 → DXIL, GPU 불필요)
Sandbox/Source/   엔진 검증용 런타임 데모 실행 파일
Projects/Sample/  예제 프로젝트 (Sample.eproject, Content/ 에셋, Source/ = SampleGame.dll 게임 모듈). 인자 없이 실행하면 기본으로 열린다
Engine/EngineDll.cpp  ProjectEEngine.dll 진입 단위 (엔진 모듈 객체가 여기로 링크됨)
Tests/            CoreTests, RendererTests, RhiTests, AudioTests, EditorTests, ScriptingTests, PhysicsTests, NetworkTests, AITests, UITests (CTest 등록)
CMake/ThirdParty.cmake  FetchContent 외부 라이브러리 (커밋/해시 고정): stb_image, cgltf, imgui, ImGuizmo, nlohmann/json, miniaudio, bc7enc_rdo, Lua, sol2, Jolt Physics, protobuf(GNS 전용), GameNetworkingSockets, Recast/Detour, imgui-node-editor(에디터 전용). 서드파티 소스 수정이 필요하면 `CMake/Patches/*.cmake`를 PATCH_COMMAND로 적용한다(여러 번 실행해도 안전하게, 대상 문자열을 못 찾으면 FATAL_ERROR)
Scripts/          빌드 스크립트 (Build.ps1), 패키징 스크립트 (Package.ps1)
Build/            CMake 빌드 출력 (git 제외)
```

새 모듈은 `Engine/Source/<Module>/CMakeLists.txt`에 `E<Module>` 정적 라이브러리 + `ProjectE::<Module>` 별칭으로 추가하고 `e_set_target_defaults`를 호출한다.

## 빌드

세 가지 방법이 있으며 모두 같은 CMake 설정을 쓴다. CMake/Ninja는 PATH에 없고 VS 2022 번들 버전을 사용한다.

1. **Visual Studio에서 폴더 열기 (권장)**: VS 2022 → `파일 → 열기 → 폴더`로 프로젝트 루트를 연다. `CMakePresets.json`의 `ninja-debug`가 자동 선택되고, 솔루션 탐색기의 "CMake 대상 보기"에서 모듈 트리가 보인다. 시작 항목을 `ProjectEEditor.exe`로 고르고 F5. 빌드는 Ctrl+Shift+B.
2. **.sln으로 빌드**: `GenerateSolution.bat` 더블클릭 → `Build\vs2022\ProjectE.sln`이 생성되어 열린다. 시작 프로젝트는 `ProjectEEditor`로 설정되어 있고 F5로 빌드+실행. 파일/모듈을 추가한 뒤에는 이 배치를 다시 실행하거나 VS의 ZERO_CHECK 프로젝트가 자동 재생성한다. 산출물은 `Build\vs2022\Bin\<Debug|Release>\`.
3. **더블클릭**: 루트의 `Build.bat`(빌드), `RunEditor.bat`(Release 빌드 후 에디터 실행 — `Build\ninja-release\Bin`). 둘 다 `Scripts\Build.bat`(cmd, `Build.ps1`과 같은 절차 — 옵션 `-Config Release`, `-Run`, `-RunSandbox`, `-Test`, `-Clean`)을 부른다. PowerShell 서명 정책(AllSigned)으로 `.ps1`이 막힌 PC에서도 동작한다
4. **명령줄**:

```powershell
.\Scripts\Build.ps1                  # 구성 + Ninja Debug 빌드
.\Scripts\Build.ps1 -Config Release
.\Scripts\Build.ps1 -Run             # 빌드 후 에디터 실행
.\Scripts\Build.ps1 -RunSandbox      # 빌드 후 런타임 데모 실행
.\Scripts\Build.ps1 -Test            # 빌드 후 단위 테스트 (ctest)
.\Scripts\Build.ps1 -VisualStudio    # .sln 생성 (Build\vs2022\ProjectE.sln)
.\Scripts\Package.ps1 [-Project Projects\Sample] [-Config Release] [-IncludeSources]  # Release 빌드 → 쿠킹 → Build\Package\<프로젝트>\<ExecutableName>.exe (아이콘/버전 스탬프, VC++ 런타임 동봉, 종속 DLL 검사) + Build\Package\<프로젝트>-Symbols\ (PDB)
.\Scripts\SteamUpload.ps1 -Account <빌드 계정> [-SetLive beta] [-Preview] [-GenerateOnly] [-SkipPackage]  # 패키징 → SteamPipe vdf → steamcmd 업로드
```

수동(VS 개발자 명령 프롬프트): `cmake --preset ninja-debug` → `cmake --build --preset ninja-debug`. 실행 파일은 `Build/ninja-<config>/Bin/` 아래 `ProjectEEditor.exe`(에디터), `Sandbox.exe`(런타임 데모).

참고: 2026-09-30 VS Installer 복구 이후 `vs2022` 프리셋(.sln 생성)도 정상 동작한다. 이전에는 VS 설치 메타데이터 손상으로 CMake가 VS 인스턴스를 찾지 못했었다. 같은 증상이 재발하면 VS Installer의 "복구"로 해결한다.

셰이더만 빠르게 검증할 때는 SDK의 dxc.exe를 직접 사용한다 (`-HV 2021 -Zpr -WX -I Engine/Shaders`). 에디터 실행 중에는 `Engine/Shaders/*.hlsl|hlsli`를 저장하면 자동 반영(핫 리로드, 실패 시 기존 셰이더 유지)되고, Ctrl+R(도구 → 셰이더 다시 로드)은 강제 재컴파일한다.

자동 검증 실행(`--exit-after`/`--screenshot`)은 **창을 띄우지 않는다**(`FWindowDesc::bHidden` — 그리기·스크린샷은 그대로, 작업 중 포커스를 빼앗지 않음, 스크린샷은 보이는 창과 비트 동일). `--show-window`는 **사용자가 직접 요청할 때만** 쓴다 — 작업·조사·성능 측정 중에는 쓰지 않는다(2026-10-04 사용자: "게임 화면이 계속 뜬다", 높은 fps 측정도 숨긴 창 + `--no-vsync`로 하고 로그 프레임 시간을 함께 기록). 창을 보이게 만드는 새 코드(ShowWindow·`SWP_SHOWWINDOW`·`WS_VISIBLE`)는 `FWindow::bHidden`을 존중한다.

화면 자동 검증: `.\Scripts\Verify.ps1 [-Target Editor|Runtime|Sandbox] [-ExtraArgs "--select <이름>"] [-Name <이름>]` → `Saved/Verify/<이름>.png`와 로그, D3D12 디버그 레이어 오류/경고 요약(종료 코드 0 = 오류 없음). 앱 공통 인자: `--exit-after <N>`, `--screenshot <경로>`, `--log <경로>`, 에디터 `--select <엔티티 이름>`, `--open-asset <Content 기준 경로>[,...]`(에셋 편집 창 열기), `--verify-asset-close`(편집 후 저장 안 함 닫기 검증), `--reset-layout`(기본 창 배치), `--content-dir <폴더>`(콘텐츠 브라우저 시작 폴더), `--verify-asset-move`(임시 복사본으로 이동·이름 변경 참조 갱신 검증), `--bake-navmesh`(시작 씬 내비메시 굽기, 씬 옆 .enav 생성), `--show-navmesh`(뷰포트 내비메시 표시), `--play`(시작 시 플레이 모드), `--then-open <씬> [--then-open-frame N]`(시작 씬 뒤 N프레임(기본 120)에 다른 씬 열기 — 같은 레지스트리 재사용 확인), 에디터/런타임 `--scene <Content 기준 경로>`. 렌더링/에디터 변경 후에는 반드시 실행해 스크린샷을 직접 보고, 디버그 레이어 오류 0건을 확인한 뒤 커밋한다. 크래시 진단: 힙 손상(0xC0000374)·빠른 실패도 `FCrashHandler`(벡터 예외 처리기)가 `<Saved>/Crashes/`에 보고서를 남기고(발견 지점 — 손상 지점은 아닐 수 있음), 자동 검증(`--exit-after`)의 Debug 실행은 CRT assert/힙 검사 실패를 대화 상자 대신 로그(`==== CRT assert`) + 덤프 후 종료 코드 3으로 끝낸다 — Release 힙 손상은 Debug 자동 검증으로 원인 지점을 먼저 찾는다.

빌드/단위 테스트 실행은 사용자 승인(2026-09-29)에 따라 이 프로젝트에서 항상 자동으로 수행한다. 에디터/Sandbox 실행(화면 확인)과 외부 다운로드는 사용자에게 안내/확인한다.

## 검증 체크리스트 (구현 후)

- [ ] `/W4 /WX` 통과
- [ ] 디버그 레이어 에러/경고 없음, 종료 시 라이브 오브젝트 보고에 누수 없음
- [ ] 리사이즈·최소화·포커스 전환에서 크래시 없음
- [ ] `Plans.md` 상태 갱신
