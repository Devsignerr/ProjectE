# ProjectE 개발 계획

상태 표기: `[ ]` 대기 · `[~]` 진행중 · `[x]` 완료 · `[!]` 블로커

## 핵심 결정 (2026-09-29)

- **그래픽스 API**: DirectX 12 (Windows x64 전용, Windows SDK 10.0.22621)
- **언어/빌드**: C++20, MSVC (VS 2022), CMake 3.25+ (VS 번들), Visual Studio / Ninja 제너레이터
- **의존성 방침**: 핵심(창/입력/수학/RHI/렌더러/ECS)은 직접 구현. 이미지·모델 로딩, UI 등은 CMake FetchContent로 도입
- **최종 목표**: 에디터를 포함한 범용 3D 게임 엔진
- **좌표계/수학 규약** (2026-09-29): 왼손 Z-up (+X 앞, +Y 오른쪽, +Z 위, UE 방식). 행렬은 행우선·행벡터(`v * M`), 합성은 적용 순서대로 곱함(`S * R * T`, `World * View * Proj`). 뷰 공간은 +X 오른쪽/+Y 위/+Z 앞, 깊이 [0, 1]. 쿼터니언 `A * B`는 B 먼저 적용. HLSL은 `-Zpr`로 동일 레이아웃
- **프로젝트 구조** (2026-09-30): 엔진/프로젝트 분리. 엔진 콘텐츠(셰이더 등)는 실행 파일 기준, 게임 콘텐츠는 프로젝트 폴더(`Content/`, `Saved/`, `Config/`) 기준. 에디터는 `.eproject`를 열고 패키징은 프로젝트 단위. 예제 프로젝트를 저장소에 하나 둔다
- **직렬화** (2026-09-30): 에디터가 다루는 씬/머티리얼/설정은 JSON(사람이 읽고 diff 가능), 런타임용은 쿠킹 단계에서 엔진 바이너리로 변환. JSON 파서는 라이브러리 사용
- **스크립팅** (2026-09-30): 하이브리드. 엔진 시스템과 무거운 게임플레이는 C++ ECS 시스템, 콘텐츠 로직(이벤트/결정/UI 흐름)은 Lua 스크립트 컴포넌트(핫 리로드). 원칙: 매 프레임 대량 순회는 C++, 스크립트는 이벤트 중심·배치 API. 리플렉션은 인스펙터/직렬화/스크립트 바인딩이 공유
- **셰이더**: DXC(Windows SDK 번들 1.6, SM 6.0) 런타임 컴파일. `.hlsl/.hlsli`는 UTF-8 **BOM 필수** (DXC 1.6이 BOM 없는 한글 UTF-8을 읽지 못함)

## Phase 0 — 프로젝트 골격

**DoD**: `Sandbox.exe` 실행 시 창이 열리고 DX12로 배경색이 클리어된다. 리사이즈 / ESC 종료 / F1 VSync 토글이 동작하고, 디버그 레이어 에러 및 종료 시 라이브 오브젝트 누수가 없다.

- [x] CMake 프로젝트 구성 (프리셋, 모듈 분리, 공통 옵션, 빌드 스크립트)
- [x] Core: 기본 타입 / 로그(`E_LOG`) / 어설트(`E_CHECK`) / 문자열 변환
- [x] Core: 타이머
- [x] Core: Win32 창 + 이벤트 (키/마우스/리사이즈/포커스)
- [x] Core: 입력 상태 관리 (Down / Pressed / Released, 마우스 델타)
- [x] Core: 애플리케이션 루프 (`FApplication`)
- [x] RHI/D3D12: 디바이스, 커맨드 큐 + 펜스, 디스크립터 힙, 스왑체인
- [x] RHI/D3D12: 프레임 루프 파사드 (`FD3D12RHI` — 클리어, 프레젠트, 리사이즈)
- [x] Sandbox 실행 파일
- [x] 빌드 검증 (Ninja Debug, `/W4 /WX` 경고 0 — 2026-09-29)
- [x] 실행 검증 (사용자 확인 2026-09-29)

## Phase 1 — 첫 삼각형

**DoD**: Sandbox 화면 중앙에 빨강/초록/파랑 정점 색이 보간된 삼각형이 그려진다. 수학 단위 테스트 전부 통과, 디버그 레이어 에러 없음.

- [x] 수학 라이브러리: `FMath`, `FVector2/3/4`, `FQuat`, `FMatrix4x4` (`Core/Math/`)
- [x] 단위 테스트 프레임워크 (`Core/Testing/`) + `Tests/CoreTests` 15개 통과 (CTest 등록)
- [x] 셰이더 컴파일: DXC 런타임 컴파일 (`FD3D12ShaderCompiler`), `Engine/Shaders/` (Common.hlsli, Triangle.hlsl), DXC DLL 자동 복사
- [x] 루트 시그니처 빌더 (`FD3D12RootSignature`: 상수/CBV/디스크립터 테이블/정적 샘플러, v1.1)
- [x] 그래픽스 파이프라인 래퍼 (`FD3D12PipelineState`, 기본값 위주 `FGraphicsPipelineDesc`)
- [x] 정적 버퍼 (`FD3D12Buffer::InitStatic`: 업로드 힙 → 디폴트 힙 동기 복사, VBV/IBV)
- [x] Sandbox 삼각형 드로우 코드 + 빌드 검증 (`/W4 /WX` 경고 0), 셰이더 dxc.exe 오프라인 컴파일 검증
- [x] 실행 검증 (사용자 확인 2026-09-29: 삼각형 표시 정상)

## Phase 2 — 3D 기초

**DoD**: UV 체커 텍스처가 입혀진 큐브가 자전하며 Blinn-Phong 방향광으로 음영·하이라이트가 보인다. 우클릭 + WASD/마우스로 시점 이동, 창 리사이즈 시 종횡비 유지, 디버그 레이어 에러 없음.

- [x] 프레임 링 동적 업로드 버퍼 (`FD3D12DynamicUploadBuffer`, 루트 CBV용 256B 정렬 할당)
- [x] 깊이 버퍼 (`FD3D12DepthBuffer`, D32_FLOAT, 리사이즈 연동) + RHI BeginFrame에서 RTV/DSV 바인딩·클리어
- [x] Renderer 모듈 신설: `FCamera`, `FFlyCameraController`(우클릭+WASD/QE, 휠 속도), `FMeshData`/`FVertex`, `FPrimitiveShapes::MakeCube`, `FStaticMesh`, `ShaderTypes.h`
- [x] 텍스처: stb_image(FetchContent, v2.30 고정) `FImageLoader`, `FD3D12Texture::Init2D`(풋프린트 업로드), 셰이더 가시 `FD3D12DescriptorAllocator`, 정적 샘플러
- [x] Blinn-Phong 방향광 + 머티리얼 상수(b2) + 역전치 법선 변환 (`Mesh.hlsl`)
- [x] `Tests/RendererTests` (큐브 위상/와인딩, 카메라)
- [x] 실행 검증 (사용자 확인 2026-09-29: 텍스처 큐브 + 라이팅 + 카메라 조작 정상)

후속 과제 (Phase 3 이후): 텍스처 밉맵 생성, sRGB 처리(백버퍼/텍스처 포맷), 디스크립터/리소스 지연 해제, 비동기 업로드 컨텍스트

## Phase 3 — 리소스 / 씬

**DoD**: Sandbox에 DamagedHelmet(glTF)이 텍스처와 함께 올바른 방향(Z-up)으로 표시되고, 회전하는 부모 아래 큐브 링이 따라 돌며(계층), 창 제목의 "메시 N/M 표시"가 시점에 따라 변한다(컬링). F2로 프러스텀을 고정하고 카메라를 돌리면 화면 밖 메시가 사라지는 것이 보인다. 디버그 레이어 에러 없음.

- [x] 자체 ECS (`Core/ECS`: FEntity 세대 핸들, TSparseSet, FRegistry, TView) + 테스트 5개
- [x] Scene 모듈 (`FScene`: 이름/트랜스폼/계층 컴포넌트, SetParent 순환 거부, 재귀 파괴, 계층 순서 WorldMatrix 갱신) + 테스트 4개
- [x] 리소스 관리자 (`FResourceManager`: TResourcePool 세대 핸들, 경로 캐시, 기본 흰색 텍스처/머티리얼, 지연 해제)
- [x] RHI 지연 해제 (`DeferRelease`/`DeferFreeDescriptor`, 프레임 펜스 이후 처리) + sRGB (백버퍼 RTV/색상 텍스처 `_SRGB`)
- [x] glTF 로더 (cgltf, 커밋 고정): 좌표계 변환(반사, 쿼터니언/행렬 켤레 테스트), 노드 계층, 머티리얼, GLB 내장 이미지, 유니코드 경로 + `FModelLoader`로 씬 배치
- [x] 프러스텀 컬링 (`FFrustum` p-vertex AABB 테스트) + `FSceneRenderer` (수집 → 컬링 → 머티리얼/메시/거리 정렬 → 드로우, 통계, 프러스텀 고정)
- [x] Sandbox: ECS 씬(태양광 엔티티, 바닥, 공전 큐브 링, 원거리 큐브 40개, DamagedHelmet)
- [x] 빌드 검증 (경고 0), 테스트 26개 통과 (DamagedHelmet 실제 파싱 포함)
- [ ] 실행 검증 (DoD 확인)

후속 과제: ~~텍스처 밉맵 생성~~(완료: 컴퓨트 GenerateMips, 렌더링 트랙 B), 비동기 업로드 컨텍스트, 카메라 컴포넌트, 다중 광원, 노멀 맵/탄젠트(glTF), 머티리얼 인스턴스 정렬 키 최적화

## Phase 4 — 에디터

**DoD**: `ProjectEEditor.exe`에서 도킹된 뷰포트/계층/인스펙터/콘텐츠/통계 패널이 보이고, 계층에서 엔티티를 선택하거나 뷰포트를 클릭해 선택하면 기즈모로 이동/회전/스케일이 되며 인스펙터 값이 함께 바뀐다. 콘텐츠 패널에서 DamagedHelmet을 씬에 추가할 수 있다. 창 리사이즈/뷰포트 리사이즈에서 크래시·디버그 레이어 에러 없음.

- [x] Dear ImGui(도킹 브랜치 1.93) + ImGuizmo FetchContent, `FImGuiLayer`(Win32/DX12 백엔드, 엔진 SRV 할당자, 한글 폰트, DPI)
- [x] 창 메시지 훅, UI용 UNORM 백버퍼 RTV 뷰, `FD3D12RHI::SetRenderTargetToBackBuffer`
- [x] `Editor` 모듈 + `ProjectEEditor` 실행 파일, 메뉴 바, 통계 창, 레이아웃 ini(`Saved/`)
- [x] 오프스크린 뷰포트 (`FD3D12RenderTarget`: TYPELESS 색상 sRGB RTV/UNORM SRV, 지연 해제 리사이즈)
- [x] 계층 패널 (선택, 드래그 부모 변경, 우클릭 생성/삭제), 인스펙터 (이름/트랜스폼/스태틱 메시/머티리얼/방향광, 컴포넌트 추가·제거), 콘텐츠 브라우저 (폴더 탐색, glTF 씬 추가)
- [x] 기즈모 (ImGuizmo, W/E/R, 로컬/월드, 부모 기준 로컬 분해) + 뷰포트 클릭 선택 (광선-AABB)
- [x] 병렬 트랙 B (서브에이전트 워크트리): 컴퓨트 셰이더 밉맵 생성 (`FD3D12MipGenerator`), 이방성 샘플러 — 머지 완료
- [x] 빌드 검증 (경고 0), 테스트 통과
- [x] 실행 검증 (사용자 확인 2026-09-30: 도킹/패널/기즈모/선택 정상. glTF 와인딩·기즈모 BeginFrame 수정 후)

후속 과제: 카메라 컴포넌트/에디터 카메라 저장, 다중 선택, 실행 취소(Undo), 에디터 상태 직렬화(Phase 5와 함께), 선택 하이라이트(아웃라인), 그리드/축 표시

## Phase 5 — 에셋 파이프라인 / 직렬화

**DoD**: 에디터가 `.eproject`를 열어 프로젝트 콘텐츠를 보여주고, 씬을 JSON으로 저장/열기할 수 있으며, 인스펙터는 리플렉션 정보만으로 모든 등록 컴포넌트를 편집한다. 컴파일 타임 절대 경로가 없고, 쿠킹된 프로젝트를 런타임 실행 파일이 로드한다.

- [x] (트랙 A) 경로/프로젝트 체계: `FPaths`(엔진 디렉터리 탐지, 프로젝트 Content/Saved/Config), `.eproject`(JSON), `Projects/Sample` 예제 프로젝트, `--project` 인자, 절대 경로 define 제거, nlohmann/json 도입
- [x] (트랙 B) 리플렉션/프로퍼티 시스템: `FTypeRegistry`, `FTypeInfo`/`FPropertyInfo`(타입/오프셋/표시명/범위/플래그), `TTypeBuilder` 유창한 등록, ECS 훅(Has/Add/Remove/Get), `RegisterSceneTypes`(FScene 생성자에서 보장) + 테스트 4개
- [x] (트랙 B) 인스펙터를 리플렉션 기반 범용 UI로 전환 (타입별 위젯, 쿼터니언 오일러 캐시, 숨김/읽기 전용/색 플래그), "컴포넌트 추가" 메뉴 자동 생성
- [x] (트랙 C) JSON 직렬화: `FSceneSerializer`(.escene, 리플렉션 기반, Transient/핸들 제외, 관용적 로드), 에셋 참조(`MeshAsset`/`MaterialAsset`/`FModelComponent`) + `FSceneAssetResolver`, `.emat` 머티리얼 에셋(`FMaterialAsset`, `LoadMaterial`), 에디터 파일 메뉴(새 씬/열기/저장/다른 이름, Ctrl+N/O/S), 프로젝트 기본 씬 자동 생성 + 테스트 3개
- [x] (트랙 D) 셰이더 사전 컴파일: `Engine/Shaders/Shaders.json` 매니페스트, `FShaderLibrary`(메모리 캐시 → 쿠킹 DXIL(소스·포함 파일 mtime 검사) → DXC 컴파일+기록), `ProjectECook` 도구(GPU 불필요), `FD3D12MipGenerator`·`FSceneRenderer` 라이브러리 경유 + RhiTests 6개
- [x] (트랙 D) 런타임 실행 파일 `ProjectERuntime` (창 서브시스템, `--project`, 프로젝트 DefaultScene 로드 → 없으면 자리표시 씬)
- [x] (트랙 E) 에셋 쿠킹: `FAssetCache`(glTF → `.emodel`, 이미지 → `.etex`, `<프로젝트>/Cooked/`에 Content 상대 경로로 기록, 형식 버전+mtime 검사, 손상 파일 방어), 로더가 쿠킹본 우선 사용(없으면 변환 후 기록), `FBinaryWriter/Reader`(Core/Serialization), `ProjectECook`이 셰이더+에셋 쿠킹(`--force`), Package.ps1이 Cooked 포함 + 테스트 8개. 헬멧 로드 Debug 562ms → 120ms
- [x] (트랙 F, 서브에이전트) 셰이더 핫 리로드: `FFileWatcher`(ReadDirectoryChangesW overlapped + 메인 스레드 폴링, `FChangeDebouncer` 150ms) → `FShaderLibrary::Invalidate`(#include 의존 추적) → `FSceneRenderer::ReloadShaders`(새 PSO 생성 성공 시 교체, 이전 PSO 지연 해제, 실패 시 유지), 에디터 "도구 → 셰이더 다시 로드"(Ctrl+R 강제 재컴파일) + 우하단 알림. BOM 때문에 첫 줄 #include를 놓치던 의존 스캔 버그 수정 + 테스트 3개
- [x] (트랙 D) 패키징 스크립트 `Scripts/Package.ps1` (Release 빌드 → 쿠킹 → `Build/Package/<프로젝트>/` 스테이징 + Run.bat, 엔진 마커 검사)

후속 과제 (트랙 E): 텍스처 BC7/BC5 블록 압축(현재 비압축 RGBA8 — 헬멧 .emodel 81MB), 밉 사전 생성, 외부 .bin/이미지를 참조하는 .gltf의 의존 파일 mtime 검사, 쿠킹본 LZ4 압축

후속 과제 (트랙 D): ~~`FSceneRenderer` 셰이더 로드 `FShaderLibrary` 전환~~(완료), 런타임 파일 로그(콘솔 없음 → `FLog` 파일 싱크), 패키지에서 셰이더 소스·DXC DLL 제거(쿠킹 DXIL만 배포), 에셋 쿠킹

## Phase 6 — 렌더링 고도화

**DoD**: DamagedHelmet이 PBR(금속/거칠기/노멀/AO/발광)로 올바르게 보이고, 방향광 그림자가 드리우며, HDR + 톤매핑/블룸이 적용된다. 에디터에서 선택한 물체는 뷰포트에 아웃라인, 계층 패널에 강조(부모 자동 펼침·스크롤)가 표시된다. 디버그 레이어 에러 없음.

- [x] (기반) HDR 파이프라인: 씬을 R16G16B16A16_FLOAT(`FD3D12RenderTarget` 포맷 설정화)에 그린 뒤 `FPostProcessor`(노출 + ACES/Reinhard 톤매핑, `Tonemap.hlsl`/`Fullscreen.hlsli`)로 출력(`FRenderOutput`: 백버퍼 또는 뷰포트 RTV)
- [x] (트랙 G) 선택 표시: `FSelectionOutline`(선택+하위 메시를 R8 마스크에 → `Outline.hlsl` 원형 반경 가장자리 합성, 내부 약한 틴트, 가려져도 실루엣 표시), 계층 패널 강조(주황 계열 행 색, 외부 선택 시 조상 자동 펼침 + 스크롤), 뷰포트에서 모델 하위 노드 클릭 시 모델 루트 선택, 셰이더 핫 리로드 연동
- [x] (트랙 J, 서브에이전트) 포스트 프로세싱: 블룸(Jimenez 13탭 다운샘플 + Karis 평균/소프트 니 임계값, 텐트 업샘플 가산, 절반 해상도부터 최대 6단계, `Bloom.hlsl`), 자동 노출(1/4 해상도 로그 휘도 히스토그램 PS UAV → 컴퓨트 평균 + 지수 시간 적응, `AutoExposure.hlsl`), 수동 EV, 톤매핑 연산자, `EBlendMode`(Opaque/Alpha/Additive) PSO 블렌드, 에디터 "포스트 프로세스" 패널, `FPostProcessMath` + 테스트 5개. 비네트/그레인은 미구현
- [x] (트랙 H, 서브에이전트) PBR 금속/거칠기: Cook-Torrance(GGX, Smith height-correlated, Schlick) + 에너지 보존 Lambert, glTF 텍스처 5종(베이스/금속거칠기/노멀/AO/발광, 발광 강도 확장 포함), 정점 탄젠트(glTF TANGENT 변환 또는 UV로 계산), 머티리얼별 연속 SRV 5칸 디스크립터 테이블(`AllocateRange`), `.emat` PBR 필드(구 형식 호환), 쿠킹 모델 형식 v2, 간이 환경광(하늘/지면 반구 + EnvBRDFApprox, `EvaluateAmbient`) + 테스트 7개
- [x] 섀도우 맵: 방향광 CSM(최대 4), 3x3 PCF, 텍셀 스냅, 바이어스/거리/해상도 설정 패널. VS Debug/Release 빌드·테스트 및 셰이더 18개 쿠킹 통과. 수학 테스트 3개 추가. GUI 시각 검증은 별도 필요.
- [x] IBL: `FIblRenderer`가 초기화 때 GPU로 절차적 환경 큐브맵, 조도(E/π), GGX 프리필터 6밉, BRDF LUT를 생성. PBR의 t5~t7/s1에 연결하고 HDR 스카이박스 및 셰이더 핫 리로드 지원. VS Debug/Release 빌드·테스트 3종 및 셰이더 24개 쿠킹 통과, IBL 수학 테스트 6개 추가. 외부 HDR 환경맵 입력은 후속 과제.
- [x] Phase 6 실행 검증 (자동 검증, 2026-10-01): 그림자/IBL/스카이박스/아웃라인/계층 강조 화면 확인, 디버그 레이어 오류 0건. 리사이즈·최소화는 수동 확인 필요
- [x] 에디터 콘솔 제거: Windows GUI 서브시스템, 도킹 가능한 출력 로그 패널(검색/지우기/자동 스크롤/오류·경고 색상), 초기화 로그 포함 최근 2,000개 보관. 배치 실행 시 빌드 창은 에디터를 띄운 뒤 종료. VS Debug/Release 빌드·테스트 3종 통과, 두 EXE의 GUI 서브시스템 및 PowerShell 구문 확인. 패널 화면 검증은 에디터 실행 승인 후.
- [ ] 스켈레탈 애니메이션

## 야간 자율 작업 (2026-10-01, 사용자 사전 승인)

승인 범위: 아래 전부를 승인/선택지 제시 없이 진행. 외부 라이브러리/에셋 다운로드(공식 저장소, 버전·SHA256 고정, MIT/CC-BY), 에디터/런타임 GUI 자동 실행·스크린샷 자체 검증, master 직접 커밋(push 없음). 품질 기준: 순수 로직은 단위 테스트, 렌더링/에디터는 스크린샷 확인 후 커밋.

결정 사항:
- **단위**: 센티미터(언리얼 방식), 1 = 1cm, 중력 -Z 980. glTF(미터)는 임포트 시 100배 변환(에셋별 임포트 스케일 옵션). 기존 씬/카메라/그림자 거리 값 100배 조정
- **플레이 모드**: 뷰포트에서 재생(씬 복제), 정지 시 편집 전 상태 복원, 일시정지/한 프레임 진행, 카메라 컴포넌트가 있으면 그 시점
- **Lua**: 5.4 + sol2, 유니티형 컴포넌트 테이블(`local S = {}; function S:OnStart() end; function S:OnUpdate(dt) end; S.Properties = {...}; return S`)
- **물리**: Jolt, 60Hz 고정 스텝 + 렌더 보간, 엔진 cm ↔ Jolt m 변환 계층
- **애니메이션**: 클립 재생/루프/속도, 크로스페이드, 루트 모션, Lua 제어 (상태 머신은 후속)
- **Undo**: 모든 씬 편집(트랜스폼, 컴포넌트 속성/추가/삭제, 엔티티 생성/삭제/부모 변경, 복제), 드래그 1회 = 1단계, 엔티티 스냅샷 기반
- **오디오**: miniaudio
- **엔진 DLL화**: master에 직접
- **데모 씬**: Main.escene은 사용자 수정본 그대로 커밋, 새 기능 데모는 `Scenes/Demo_*.escene`

순서:
- [x] 0a) 자동 검증: `--exit-after/--screenshot/--log/--select`, D3D12 디버그 레이어 메시지 → 엔진 로그(디버거 없으면 중단 안 함), 백버퍼 PNG 스크린샷, `Scripts/Verify.ps1`. Phase 6 화면 자체 검증 완료(PBR/CSM/IBL 하늘/아웃라인/계층 강조/패널) + 발견한 디버그 레이어 오류 수정(IBL·포스트 공용 테이블 DATA_VOLATILE, HDR 최적 클리어 값) → 에디터/런타임/Sandbox 오류 0건
- [x] 0b) 센티미터 전환: `FUnits`(Core/Math/Units.h), glTF 임포트 ×100(`FGltfLoader::ImportScale`, 방향 벡터 제외), `ModelVersion` 3, 기본 큐브 100cm, 카메라 근/원 10/100000, 플라이 속도 500cm/s, 그림자 거리 6000/5000, Main.escene·코드 씬 위치 ×100. 크래시 핸들러(`FCrashHandler`: SEH 예외 코드 + 심볼 콜스택 → stderr/로그) 추가
- [x] 1a) (서브에이전트) 스켈레탈 애니메이션: glTF 스킨/애니메이션 임포트(IBM 이동 ×100), GPU 스키닝(메인+그림자+아웃라인), `FAnimationComponent`/`FAnimationSystem`(재생/루프/속도/크로스페이드/루트 모션, Lua용 API), ModelVersion 4, Khronos Fox(CC-BY) + `Demo_Animation.escene`, `--scene` 인자, AnimationTests. 한계: 클립 선택은 텍스트, 피킹은 바인드 포즈 경계, 모프 타깃 미지원
- [x] 1b) (서브에이전트) Lua 스크립팅 + 에디터 플레이 모드: Lua 5.4.9 + sol2 3.3.0(C++로 컴파일), Scripting 모듈(리플렉션 자동 바인딩, Scene/Log/Input/Time, 핫 리로드, 오류 격리), 유니티형 테이블 + Properties 인스펙터, 플레이 모드(메모리 복제 `FSceneCloner`, 정지 시 복원, 일시정지/한 프레임, 카메라 컴포넌트), `Demo_Scripting.escene`, ScriptingTests. 머지 시 통합: 복제(플레이/Ctrl+D/Undo 템플릿) 시 스킨·애니메이션 런타임 참조 재매핑(`FSceneCloner::CopyRuntimeData/RemapRuntimeReferences`), 플레이 중 Undo 차단, Lua 애니메이션 API(`entity:PlayAnimation/StopAnimation/ResumeAnimation/SetAnimationSpeed/GetAnimationClip(s)`)
- [x] 1c) (서브에이전트) 에디터 편의: 스냅샷 Undo/Redo(100단계, 드래그 1회=1단계, dirty `*`), 무한 그리드·축(Grid.hlsl), 스냅(10cm/15°/0.1, Ctrl 반전), 복제 Ctrl+D(리플렉션 서브트리 복제), 다중 선택(Ctrl/Shift), 에디터 카메라 저장 + F 포커스, `--select A,B`/`--verify-undo`, EditorTests. 한계: 머티리얼 편집은 Undo 제외, 대형 씬 스냅샷 비용
- [x] 2) (서브에이전트) 물리: Jolt 5.6.0(MIT, 태그 zip SHA256 고정), Physics 모듈(`FPhysicsWorld` cm↔m 변환·축/쿼터니언 그대로(해밀턴 곱 동일, 테스트로 검증), `FPhysicsSystem` 60Hz 고정 스텝 + 보간, 부모 있는 엔티티 로컬 역변환, 키네마틱 이동, 스크립트 텔레포트 감지), RigidBody/Box/Sphere/Capsule 콜라이더 컴포넌트(`RegisterPhysicsTypes()`), Lua `AddForce/AddImpulse/SetVelocity/GetVelocity`·`Physics.Raycast`(`FScriptPhysicsHooks`), 에디터 플레이 모드·런타임 연동, `primitive:sphere`, `Demo_Physics.escene` + `Launcher.lua`, PhysicsTests 12개. 한계: 엔티티당 콜라이더 1개, 충돌 콜백/트리거 없음, 에디터 콜라이더 표시 없음
- [x] 3) 오디오(miniaudio 0.11.25): Audio 모듈(FAudioEngine/FAudioSystem/FAudioSourceComponent), 3D 공간화(청자 공간 변환), 런타임 연동 + `--scene`, 샘플 Hum/Chime.wav(자체 생성), `Demo_Audio.escene`, AudioTests 10개 완료. 에디터 플레이 모드 재생(보는 카메라가 청자, 정지 시 해제), Lua `entity:PlaySound/StopSound`, `Audio.PlayOneShot`(Scripting은 `FScriptAudioHooks`로 Audio에 비의존)
- [x] 4) 텍스처 압축: `TextureCompression`(CPU 밉 — sRGB 선형 평균/노멀 재정규화, BC7/BC5/BC4, bc7enc_rdo), `FD3D12Texture::Init2DFromMips`, 용도별 쿠킹(`*.color|linear|normal|mask.etex`), .emat 슬롯 용도, 셰이더 노멀 Z 재구성, Cook 도구 용도 수집 완료. 배포 패키지: 쿠킹 DXIL + Shaders.json(엔진 마커)만, dxcompiler.dll 지연 로드(없으면 쿠킹 셰이더 전용), 쿠킹본 있는 원본 에셋 제외(`-IncludeSources`로 포함) — DXC 없는 패키지 실행 검증 완료. 모델 내장 이미지도 머티리얼 용도별 압축(`CompressModelImages`, ModelVersion 5) — DamagedHelmet 쿠킹 80MB → 26MB
- [x] 5) 엔진 DLL화 + 게임 모듈: `ProjectEEngine.dll`(런타임 모듈 OBJECT → 공유 라이브러리, export 24k, `E_ENGINE_SHARED` OFF면 정적), ECS 타입 ID 엔진 DLL 전역화, `E_ENGINE_API`/엔진 로그 카테고리, `IGameModule` + `FGameModuleHost`(버전 검사, 소유자별 타입 제거), `.eproject "GameModule"`, `Projects/Sample/Source` → `SampleGame.dll`(Spinner/Hover 컴포넌트 + C++ 시스템), 에디터 플레이 모드/런타임 연동, `Demo_GameModule.escene`, GameModuleTests(실제 DLL 로드·타입 ID 공유·직렬화·언로드), 패키지에 엔진/게임 DLL 포함(37.8MB). 남음: 게임 모듈 핫 리로드, 설치형 배포(find_package)
- [x] 후속: 쿠킹 셰이더 캐시를 타임스탬프 대신 소스+include 내용 해시로 검증 (`<쿠킹 파일>.srchash` 사이드카, 소스 없으면(패키지) 신뢰) (머지 직후 스킨 아웃라인 VS가 오래된 DXIL로 남아 보이지 않던 사례 — 재컴파일 후 정상, 경위 미확정)

## Phase 7 — 게임 시스템

- [x] 물리 (Jolt — 야간 작업 2)
- [x] 오디오 (miniaudio — 야간 작업 3)
- [x] 스크립팅: Lua 스크립트 컴포넌트 (sol2 바인딩, 리플렉션 기반 자동 노출, 파일 변경 감지 핫 리로드)
- [x] 엔진 DLL화: 엔진 모듈을 공유 라이브러리로 전환, 공개 API 내보내기 매크로(`E_CORE_API` 등)
- [x] 게임 모듈 분리: `Projects/<이름>/Source/` → `<이름>Game.dll`, 에디터/런타임이 `.eproject` 기준으로 로드, 게임 모듈이 컴포넌트/시스템을 리플렉션에 등록
- [ ] 설치형 엔진 배포: 헤더 + lib/dll + CMake 패키지 설정(`find_package(ProjectE)`), 게임 프로젝트는 게임 모듈만 빌드
- [ ] 게임 모듈 핫 리로드 (에디터 실행 중 게임 DLL 재빌드·교체)
- [ ] 패키징 고도화: 쿠킹 DXIL만 배포(셰이더 소스/DXC 제거), 런타임 파일 로그

진행 순서 (2026-09-30 사용자 결정): Phase 8 → 9 → 10을 먼저 하고, 위 Phase 7 남은 항목(설치형 배포, 게임 모듈 핫 리로드, 패키징 고도화)은 그 뒤에 진행한다.

## Phase 8 — 물리 품질

**DoD**: `Demo_Physics.escene` 플레이 시 물체의 무게감·미끄러짐·튀는 정도·굴러가다 멈추는 모습이 자연스럽다고 사용자가 확인한다. 조정한 기본값/설정은 단위 테스트로 고정하고, 기존 PhysicsTests는 계속 통과한다.

사용자 보고 증상 (2026-09-30): 떨림·뚫림·끊김이 아니라 "움직임의 느낌이 부자연스러움"(무게·마찰·탄성 쪽).

- [x] 증상 구체화: "물체끼리 부딪혀도 자연스럽게 상호작용하지 않고 어색하게 밀려남" (사용자 2026-09-30)
- [x] 원인 1 수정: 회전하는 동적 바디가 매 프레임 다시 생성되어 선속도·각속도가 0으로 초기화됨. 월드 행렬 분해 스케일의 ULP 오차가 비트 해시(`MakeShapeKey`)를 바꾸던 문제 → 생성 설정(`CreatedDesc`)을 보관하고 스케일 파생 치수는 상대 오차 1e-4로 비교(`NeedsRecreate`). 실험: 500cm/s로 굴린 공이 0.5초 만에 정지(3초간 재생성 79회) → 수정 후 계속 굴러감. 테스트 `Physics_RollingSphereKeepsMomentum`, `Physics_ScaleChangeResizesBody`
- [x] 원인 후보 조사 (2026-09-30, 데모와 같은 조건의 수치 실험): 쌓기 안정성(3초 이동 0cm)·상자끼리 충돌(비탄성 이론값 일치)·마찰 감속(이론 208cm / 실측 209cm)·솔버는 정상. 원인 = 수치 설정: ① 질량이 크기와 무관(기본 1kg, 데모 60cm 상자 5kg = 밀도 23kg/m³ 스티로폼 수준)인데 발사 공은 8kg·22m/s → 상자 더미가 4.8m 날아감 ② Jolt에 구르기 저항이 없어 공이 10초 뒤에도 130cm/s로 17m 구름
- [x] 수정 (사용자 결정 2026-09-30): `FRigidBodyComponent::Density`(kg/m³, 기본 500) + `Mass` 기본 0 = 콜라이더 부피 × 밀도 자동(값을 넣으면 직접 지정), `RollingResistance`(기본 0.05, `FPhysicsWorld`가 접촉 리스너로 이번 스텝에 닿은 동적 바디만 회전 감속 — 구는 약 계수 × g) → 공 300cm/s가 4.2m 구르고 멈춤. Lua `entity:GetMass()`(실제 바디 질량). 데모: 상자 밀도 120(약 26kg), 발사 공 5kg·15m/s → 상자 더미 흩어짐 0.9m. 테스트 `Physics_MassFromDensityOrExplicit`, `Physics_RollingResistanceStopsBall`, ScriptingTests `GetMass`
- [x] 실행 검증 (사용자 확인 2026-09-30)

## Phase 9 — 에셋 편집 창

**DoD**: 콘텐츠 브라우저에서 머티리얼/스태틱 메시/애니메이션(스킨 모델)/파티클 에셋을 더블클릭하면 각 전용 편집 창이 도킹 가능한 창으로 열린다. 창마다 자체 미리보기 뷰포트가 있고, 값을 바꾸면 미리보기에 즉시 반영되며, 저장하면 파일과 열려 있는 씬에 반영된다. 저장 안 한 변경은 창 제목에 `*`로 표시되고 닫을 때 확인한다.

결정 사항 (2026-09-30, 사용자 위임 — "Phase 9 전체를 한 번에"):
- **미리보기 렌더링**: 메인 뷰포트와 별도인 전용 `FSceneRenderer` 하나(`FAssetEditorManager` 소유, 첫 창을 열 때 초기화)로 모든 창을 그린다. 미리보기 타깃은 960x720 고정(창에는 종횡비 유지 맞춤) — 창마다 크기가 다르면 렌더러의 HDR/블룸 버퍼가 프레임마다 다시 만들어지므로. 메인 통계·자동 노출과도 분리
- **실시간 반영**: 머티리얼/파티클 편집기는 리소스 관리자 캐시의 공유 리소스를 직접 고친다 → 열린 씬에 즉시 반영(언리얼과 같은 방식). 저장하지 않고 닫으면 파일 상태로 되돌린다
- **창 안 실행 취소**: 창마다 `FUndoHistory`(에셋 JSON 스냅샷). 창이 포커스를 가지면 Ctrl+Z/Y/S와 Delete는 그 창이 처리(메인 씬에 전달 안 함)
- **glTF 편집기 선택**: 애니메이션이 있으면 애니메이션 편집기, 없으면 스태틱 메시 편집기로 연다. 모델 파일은 편집하지 않는 보기 전용(저장 버튼 없음)
- **보류: 에셋별 임포트 설정(임포트 스케일)** — 쿠킹 캐시(`FAssetCache`)가 원본 mtime + 형식 버전으로만 검사하므로, 사이드카 설정 파일을 캐시 무효화에 넣고 `ModelVersion`을 올리는 작업이 필요하다. 반쯤 구현하면 설정이 조용히 무시되므로 후속 과제로 분리. 법선 표시도 보류(와이어프레임만)
- **파티클**: CPU 시뮬레이션(월드 공간, 시드 고정 xorshift → 다시 시작하면 같은 모양), 카메라를 향한 빌보드 인스턴싱(동적 업로드 버퍼), 불투명 메시 뒤 HDR 패스에서 깊이 테스트만(쓰기 없음), 반투명은 뒤→앞 정렬, 조명·그림자·아웃라인 없음. 곡선/서브 이미터/충돌/GPU 시뮬레이션은 범위 밖

- [x] 공통 틀: `Editor/AssetEditors/` — `FAssetEditorManager`(확장자별 편집기, 경로당 창 하나, 첫 사용 시 70% 크기 가운데, 제목 `###` 고정 ID + `UnsavedDocument` 표시, 저장 안 한 변경 닫기 확인 대화상자, 셰이더 핫 리로드 연동), `FAssetEditor`(저장/되돌리기/실행 취소 도구 줄, 미리보기 | 속성 2단 레이아웃, F 화면 맞춤), `FAssetPreview`(전용 씬 + 기본 방향광 + 바닥 그리드, 좌/우 드래그 회전·가운데 드래그 이동·휠 확대), `FOrbitCamera` + 테스트 2개. 콘텐츠 브라우저 더블클릭/"편집", 인스펙터 "머티리얼 편집기에서 열기", 에디터 인자 `--open-asset <Content 경로>[,...]`
- [x] 머티리얼 편집기 (`.emat`): 이름·PBR 값·텍스처 5슬롯(Content 이미지 목록에서 선택, 작은 미리보기), 구/큐브 미리보기. `FResourceManager::ApplyMaterialAsset`(로드 경로와 공용). 인스펙터의 .emat 직접 편집은 편집기 버튼으로 바꿈 → 머티리얼 편집 Undo 한계 해결 (모델 내장 머티리얼은 인스펙터에서 저장 없이 조정)
- [x] 스태틱 메시 편집기 (`.glb/.gltf`): 미리보기, 요약(노드/메시/정점/삼각형/머티리얼/텍스처/스킨·관절/애니메이션/크기), 메시·내장 머티리얼 목록, 와이어프레임(`FSceneRenderer::bWireframe`, 정적+스킨 PSO), "열린 씬에 추가". 임포트 설정/법선 표시는 보류(위 결정)
- [x] 애니메이션 편집기: 클립 목록, 재생/일시정지/처음으로/한 프레임(1/30초), 타임라인 스크럽(`FAnimationSystem::SetTime/GetTime/GetCurrentClipDuration` + 테스트), 속도·반복, 뼈대 표시(관절을 화면에 투영한 선/점 오버레이), 와이어프레임
- [x] 파티클 시스템: `Scene/Particles.h`(`FParticleEmitterSettings` = `.eparticle` JSON, `FParticleSystemComponent`{Asset, Playing, Speed} 리플렉션 등록, `FParticleSimulation`/`FParticleSystem`), `FResourceManager::LoadParticleEmitter`(경로 캐시 + 텍스처), `FSceneAssetResolver::ResolveParticles`(경로가 바뀐 컴포넌트만, 매 프레임), 복제/플레이 시 설정 공유·입자는 새로 시작(`FSceneCloner::CopyRuntimeData`), `Renderer/ParticleRenderer` + `Particle.hlsl`(가산/반투명, 기본 부드러운 원 텍스처), 에디터(편집 모드에서도 재생)·플레이·런타임 연동, 통계 창 입자 수, 샘플 `Particles/Fire|Smoke|Sparks.eparticle` + `Demo_Particles.escene`, ParticleTests 6개
- [x] 파티클 편집기: 방출/모양/시작 값/수명 변화(크기·HDR 색)/힘(가속도·중력·감속)/렌더(블렌드·텍스처·시드) 편집, 미리보기 재생·일시정지·다시 시작·입자 수. 콘텐츠 브라우저 "+ 새로 만들기"(머티리얼/파티클 기본 파일 생성 후 편집 창), 파티클 "씬에 추가"
- [x] 자동 화면 검증 (2026-09-30): 머티리얼/애니메이션(뼈대, 와이어프레임)/스태틱 메시/파티클 편집 창 + 런타임 `Demo_Particles` 스크린샷 확인, 로그 오류·경고 0건. 전체 테스트 8묶음 통과
- [x] 수정 (2026-09-30, 사용자 보고 "머티리얼 저장 안 하고 닫으면 크래시"): 프레임 도중(UI 단계, BeginFrame 전) 지연 해제 요청이 직전 프레임 칸에 쌓여, 이번 프레임이 아직 그리는 미리보기 타깃이 먼저 해제됨(GPU 장치 제거). `FD3D12RHI`가 요청을 모아 두었다가 EndFrame에 제출 프레임 칸으로 옮기도록 수정. 자동 검증 인자 `--verify-asset-close`(값 변경 후 저장 안 함 닫기) 9회 무오류
- [x] 사용자 확인 (2026-09-30): 미리보기 조작, 값 변경 → 씬 반영
- [ ] 실행 검증 (사용자 확인): 마우스 조작(회전/이동/확대), 값 편집 → 씬 반영, 저장/되돌리기/Ctrl+Z, 닫기 확인 대화상자, 새로 만들기, 파티클 씬에 추가

## 에디터 UI 디자인 (2026-09-30, 사용자 요청 — Phase 10 전에 진행)

결정: 언리얼 5 풍(어두운 회색 + 파란 강조색 하나, 각진 모서리). 아이콘은 Font Awesome 6.7.2 Free Solid(SIL OFL) + IconFontCppHeaders(zlib), 해시 고정, 실행 파일에 바이트 배열로 내장.

- [x] `FEditorTheme`(Editor/EditorTheme.h): 색·크기 테마, 맑은 고딕 기본/굵게 + 아이콘 글꼴 병합, `PanelTitle`(아이콘 제목 + 고정 영문 창 ID `###Viewport` 등), 에셋 종류별 아이콘/색, 컴포넌트 아이콘, `ToolButton`
- [x] 적용: 패널 제목 아이콘, 메뉴 바 가운데 재생/일시정지/한 프레임/정지 아이콘 버튼, 뷰포트 도구 막대 아이콘(반투명 어두운 버튼), 계층 행 종류별 색 아이콘 + 파란 선택, 인스펙터 컴포넌트 제목(아이콘·굵게·회색 + 휴지통 제거 버튼), 콘텐츠 목록 아이콘·색·종류 이름, 에셋 편집 창 도구 줄
- [x] 기본 도킹 배치(언리얼 풍): 가운데 뷰포트, 오른쪽 계층/인스펙터, 아래 통계/포스트/그림자/출력 로그/콘텐츠. 저장된 배치에 새 창 ID가 없으면 자동 적용, "창 → 기본 레이아웃으로 되돌리기", 인자 `--reset-layout`. 같은 탭 묶음은 처음에 마지막으로 그린 창이 선택되므로 콘텐츠를 마지막에 그린다
- [ ] 사용자 확인

## Phase 10 — 콘텐츠 브라우저 개선

**DoD**: 콘텐츠 브라우저가 언리얼처럼 썸네일 타일로 에셋을 보여주고, 에셋 종류가 색과 아이콘으로 한눈에 구분된다. 윈도우 탐색기처럼 드래그 앤 드롭으로 폴더 이동, 뷰포트 배치, 인스펙터 슬롯 지정, 외부 파일 가져오기가 되고, 이름 바꾸기/이동 후에도 씬·머티리얼의 참조가 깨지지 않는다.

결정 사항 (2026-09-30, 사용자 선택 "Phase 10 전체 한 번에"):
- **썸네일**: 전용 `FSceneRenderer` 하나(256x256 고정, 그림자 512 x 2장, 하늘 없이 어두운 배경)로 보이는 타일만 프레임당 2개씩 그려 메모리에 둔다. 파일 수정 시각이 바뀌면 다시 그린다. **디스크 캐시(`Saved/`)는 보류** — 백버퍼 스크린샷 외 GPU 읽기 경로가 없고, 즉석 생성으로 충분히 빠름
- **이미지 썸네일**: 텍스처가 `_SRGB` 포맷이라 ImGui로 직접 보이면 어둡게 나와서, 발광 슬롯에 이미지를 넣은 판을 톤매핑 없이 그려 원래 색으로 표시
- **참조 유지 규칙**: `.escene`/`.eproject`는 Content 기준, `.emat`/`.eparticle`은 파일 폴더 기준 상대 경로. 확장자가 있는 문자열만 경로로 보고, JSON을 다시 쓰지 않고 문자열 토큰만 바꿔 서식 유지. 이동 후 리소스 캐시 키(텍스처/머티리얼/모델/파티클)를 새 경로로 옮겨 중복 로드를 막고, 열린 씬 문자열과 **실행 취소 기록의 스냅샷도 같이 고친다**(되돌려도 옛 경로를 찾지 않음)
- **삭제는 휴지통**(`SHFileOperation` + `FOF_ALLOWUNDO`), 참조하는 파일 수를 확인 창에 표시. 파일 조작 자체는 Ctrl+Z 대상 아님(씬 편집만)
- 플레이 중에는 이동/이름 변경/삭제/배치를 막는다. 저장 안 한 편집 창이 걸린 파일은 옮기지 않는다(먼저 저장/닫기)
- 쿠킹 캐시(.emodel/.etex)는 Content 기준 경로가 키라 옮긴 모델/텍스처는 다음 로드 때 다시 쿠킹된다 (자동)

- [x] 썸네일: `Editor/ContentBrowser/ThumbnailCache` — 모델(첫 포즈)/머티리얼(구)/파티클(1.5초 진행)/씬(전체, 조명 없으면 기본 조명)/이미지. `FSceneRenderer::bDrawSkybox` 추가
- [x] 보기 방식: 타일 ↔ 목록, 타일 크기 슬라이더, 경로 표시줄(누르면 이동, 드롭 대상), 왼쪽 폴더 트리(현재 폴더 강조·조상 자동 펼침), 검색, 종류 필터, 2초마다 자동 새로 고침
- [x] 종류 구분: 타일 아래 종류 색 띠 + 종류 이름, 썸네일 없는 종류는 큰 색 아이콘 (`FEditorTheme::GetAssetStyle`)
- [x] 드래그 앤 드롭: 페이로드 `FContentDragDrop`(여러 파일). 폴더 타일/트리/경로 → 이동, 뷰포트 → 모델·파티클은 놓은 위치(메시 표면 또는 바닥 Z=0)에 배치 / 머티리얼은 커서 아래 메시에 지정(모델 내부 메시는 거부), 계층 → 루트 또는 놓은 엔티티의 자식으로 추가, 인스펙터 에셋 칸(`FPropertyInfo::AssetFilter` — 머티리얼/파티클/스크립트/오디오 클립) → 지정, 윈도우 탐색기 → 현재 폴더로 복사(`WM_DROPFILES`)
- [x] 파일 조작: 다중 선택(Ctrl/Shift/드래그 박스, ImGui MultiSelect), 우클릭 메뉴(편집/씬 열기/씬에 추가/이름 바꾸기/복제/삭제/경로 복사/탐색기에서 보기, 빈 곳: 새 폴더/새 머티리얼/새 파티클/새로 고침/탐색기에서 열기), 단축키 F2·Delete·Backspace·Enter (`AssetFileOps`)
- [x] 참조 유지: `AssetReferenceUpdater` + `FResourceManager::OnAssetMoved` + `FEditorApplication::OnAssetsMoved`(열린 씬/실행 취소 기록/현재 씬 경로) + `FAssetEditorManager::CloseEditorsFor`. ContentBrowserTests 4개, 실제 파일 이동 자동 검증 `--verify-asset-move`(임시 복사본으로 폴더 이동 + 텍스처 이름 변경 → 씬 파일/열린 씬/머티리얼 자기 참조/캐시 중복 없음 확인 후 삭제) 통과
- [x] 부가: 에디터 인자 `--content-dir <폴더>`, 기본 레이아웃 아래 영역 38%로
- [ ] 실행 검증 (사용자 확인): 끌어서 폴더 이동, 우클릭 메뉴, 이름 바꾸기/삭제, 뷰포트·계층·인스펙터로 끌어 놓기, 탐색기에서 파일 끌어오기

후속 과제 (Phase 10): Lua 스크립트 안의 에셋 경로 문자열(예: `Launcher.lua`의 `"Materials/Orange.emat"`)은 참조 갱신 대상이 아님 → 옮기면 플레이 때 깨짐. 옮기거나 지운 파일의 썸네일이 종료 때까지 메모리에 남음. 썸네일 요청마다 파일 수정 시각을 조회(항목이 수백 개면 새로 고침 때 한 번만 읽도록). 이름이 파일 경로처럼 생긴 엔티티(예: "Fox.glb")는 같은 파일을 옮기면 이름도 바뀔 수 있음. 썸네일 디스크 캐시

## Phase 11 — 에셋 임포트 (2026-09-30, 사용자 요청: "메시, 애니메이션, 파티클 임포트", FBX 지원)

**DoD**: FBX와 glTF 모델(정적/스킨/애니메이션)을 콘텐츠 브라우저로 가져와 쓸 수 있고, 모델마다 임포트 설정(크기, 방향, 머티리얼/애니메이션/스킨 포함 여부, 법선·탄젠트 다시 계산, 추가 애니메이션 파일)을 모델 편집 창에서 바꾼 뒤 "다시 가져오기"하면 열린 씬·미리보기·썸네일에 바로 반영된다.

결정: FBX는 ufbx v0.23.1(MIT, 커밋 고정)로 읽고, 좌표는 glTF와 같은 축(오른손 Y-up, 미터)으로 먼저 맞춘 뒤 glTF 로더의 검증된 변환 함수를 그대로 쓴다. 임포트 설정은 원본 옆 사이드카 파일 `<원본>.eimport`(JSON). 쿠킹본은 원본·사이드카·추가 애니메이션 파일 중 가장 새 것보다 새로워야 유효(ModelVersion 6). 크기/방향은 새 최상위 노드("ImportRoot")의 트랜스폼으로 적용(정점·역바인드·애니메이션 키를 건드리지 않음). 추가 애니메이션 파일은 노드 이름으로 채널을 맞춘다.

- [x] ufbx 도입 + `FFbxLoader` (메시/머티리얼 부분, 텍스처(외부 파일/내장), 스킨, 애니메이션 굽기). 모델 확장자 판정은 `FModelLoader::IsModelFile`로 통일, 쿠킹 도구도 .fbx 인식
- [x] `FModelImportSettings` (.eimport 읽기/쓰기, 적용, 추가 애니메이션 병합) + ModelImportTests 4개, `FAssetCache::LoadModelSource`. 쿠킹본 유효성: 시각 비교 + 쿠킹에 쓴 설정 기록(`<쿠킹 파일>.import`)과 현재 설정 비교 — 설정 파일을 지우거나 되돌린 경우 시각만으로는 못 잡는 문제를 자동 검증에서 발견해 추가
- [x] 다시 가져오기: `FResourceManager::TakeModelResources/DestroyModelResources`, `FEditorApplication::ReimportModelAsset`(열린 씬 인스턴스 재생성, 실행 취소용 모델 템플릿 폐기, 편집 창·썸네일 갱신, 옛 GPU 리소스 지연 해제). 자동 검증 `--verify-reimport <모델>`(크기 2배 → 원복)
- [x] 편집 창 "임포트 설정" UI(크기 배율, 방향 프리셋, 포함 항목, 법선/탄젠트 다시 계산, 추가 애니메이션 파일, 적용/되돌리기), 콘텐츠 브라우저 .fbx 인식·.eimport 숨김·모델과 함께 이동/이름 변경/복제/삭제, 참조 갱신기가 .eimport(파일 폴더 기준) 처리, 탐색기에서 모델을 가져오면 편집 창 열기(최대 3개)
- [x] 검증: 언리얼 엔진 테스트 FBX(로컬, 저장소에 넣지 않음 — 임시 폴더에 복사 후 삭제)로 정적/스킨/애니메이션 화면 확인, 다시 가져오기 자동 검증 FBX 2종 + glTF 2종 통과
- [ ] 실행 검증 (사용자 확인)

## Phase 12 — 나이아가라식 파티클 (2026-09-30, 사용자 요청)

**DoD**: 파티클 에셋이 여러 이미터를 갖고, 이미터마다 단계별(이미터 시작/갱신, 입자 생성/갱신) 모듈 목록과 렌더러를 편집기에서 쌓아 만든다. 모듈 입력은 상수/무작위 범위/곡선을 고를 수 있다. 이미터마다 계산 방식 CPU/GPU를 고를 수 있고 같은 모듈이 양쪽에서 비슷하게 동작한다. 기존 .eparticle은 자동으로 새 형식으로 읽힌다.

결정: 모듈 이름·입력은 나이아가라 기본 모듈(엔진 소스 `Plugins/FX/Niagara/Content/Modules`, 282개)을 따라 이름을 맞춘다(나중에 나이아가라 에셋 변환 시 1:1 대응). 사용자 HLSL/스크래치 패드, 이벤트, 데이터 인터페이스는 범위 밖.

- [x] 데이터 구조: `FParticleSystemAsset`(.eparticle v2) → `FParticleEmitter`(계산 방식 CPU/GPU, 로컬 공간, 주기/반복, 최대 개수, 시드) → 단계 3개(이미터 갱신/입자 생성/입자 갱신) 모듈 목록 + 렌더러 목록. 동적 입력 `FParticleValue`(상수/무작위 범위/곡선). 모듈 정보 표 18종(나이아가라 모듈 이름 id + 한국어 표시 이름). "이미터 시작" 단계는 없음(주기 시작 시각 버스트로 대체)
- [x] 핵심 모듈 (CPU): SpawnRate, SpawnBurstInstantaneous, InitializeParticle, ShapeLocation(점/구/상자/원기둥/원뿔/원환, 표면만), AddVelocity, AddVelocityInCone, AddVelocityFromPoint, GravityForce, Drag, AccelerationForce, CurlNoiseForce, VortexForce, PointAttractionForce, ScaleColor, ScaleSpriteSize, SpriteRotationRate, SubUVAnimation, Collision(바닥 평면). 난수는 (생성 순번, 시드, 흐름) 해시 → 결정적
- [x] GPU 계산: `ParticleSimulate.hlsl` 컴퓨트 셰이더가 같은 모듈을 같은 식/해시로 계산. 이미터 설정을 float4 프로그램으로 구워 올림(`FParticleRenderer::BuildGpuProgram`). 입자 풀 = 최대 개수 크기 고리 버퍼(가득 차면 가장 오래된 입자를 덮어씀), 풀 전체를 그리고 죽은 입자는 정점 셰이더에서 버림. **계획과 다름**: 빈칸 목록 + 간접 그리기는 하지 않음(최대 개수만큼 항상 계산/그리기 — 수만 개 수준에선 충분, 수십만 개 이상이면 후속 과제). GPU 반투명 정렬 없음, 입자 수는 추정치 표시
- [x] 렌더러: 스프라이트(카메라/속도 정렬 + 늘임, 플립북 칸), 메시(내장 구/큐브만, 크기 X = 지름), 리본(CPU 전용, 생성 순서로 이은 띠). 렌더러마다 블렌드/텍스처
- [x] 편집기: 이미터 목록(켜기/순서/삭제/복제, 템플릿 7종으로 추가), 이미터 속성(CPU/GPU 선택), 단계별 색 띠 모듈 목록(추가 팝업/켜기/삭제, 우클릭 순서 이동), 입력마다 방식 버튼(상수/무작위/곡선), 곡선 편집기(채널별 선, 끌기/더블클릭 추가/우클릭 삭제, 선택 키 값 편집, 색은 아래 색 띠), 렌더러 목록. 샘플 `Particles/GpuShowcase.eparticle`(GPU 소용돌이 + CPU 리본)
- [x] 기존 형식 변환(버전 없는 v1 → 이미터 1개 + 대응 모듈, 끝 크기/색은 곡선 비율로) + 테스트(`ParticleTests` 9개) + 검증(편집 창 스크린샷, 저장 안 함 닫기, 플레이 모드, 썸네일 — 로그 오류 0)
- [ ] 후속(범위 밖): 나이아가라 .uasset 직접 변환, 사용자 스크립트 모듈, 이벤트, 파일 메시 렌더러, GPU 빈칸 목록/간접 그리기/정렬

## 에디터 단축키 강화 (2026-09-30, 사용자 요청: 언리얼식)

**DoD**: 뷰포트/계층에 포커스가 있을 때 Alt+기즈모 드래그 복제, End 바닥에 붙이기, Ctrl+C/V 복사·붙여넣기, Delete 삭제가 동작하고 모두 Ctrl+Z 한 단계로 되돌려진다.

- [x] `FSceneEditOps::Copy/Paste`(씬 JSON, 월드 위치 유지 루트 붙여넣기) + `FindFloorHeight`(경계 상자 기준) + 테스트
- [x] `FEditorActions::CopySelection/PasteClipboard/SnapSelectionToFloor`, 클립보드는 `FEditorContext::EntityClipboard`(씬 전환에도 유지)
- [x] Alt+드래그: 기즈모 조작 시작 프레임에 선택 복제 → 복제본 조작 (복제+이동이 Undo 한 단계)
- [x] Delete를 `HandleShortcuts`로 이동 (키보드 내비게이션 때문에 `WantCaptureKeyboard`가 항상 참이라 동작하지 않던 문제), 뷰포트/계층 창 포커스로 판정
- [x] 계층 우클릭 메뉴·편집 메뉴에 복사/붙여넣기/바닥에 붙이기
- [x] 실행 검증 (사용자 확인)

## Phase 13~16 진행 순서 (2026-09-30, 사용자 결정 — 13·14 완료, 15·16은 나중에)

Phase 11 완료 후 13 노티파이 → 14 소켓 → 15 프리팹 → 16 인게임 UI 순서. Phase 12와의 선후는 논의되지 않음 — 착수 시 확정.

## Phase 13 — 애니메이션 노티파이 (2026-09-30, 사용자 요청: 언리얼 AnimNotify/AnimNotifyState)

**DoD**: 애니메이션 편집 창 타임라인에서 클립마다 노티파이(한 시점)와 노티파이 스테이트(구간)를 추가·이동·삭제·이름 변경하고 저장하면, 재생 중 해당 시점/구간에서 Lua 스크립트와 C++ 게임 모듈이 이름으로 이벤트를 받는다. 루프·속도 변경·크로스페이드·스크럽에서 누락/중복 없이 발생한다.

결정: 전달은 언리얼의 이름 기반 방식(`UAnimInstance::TriggerSingleAnimNotify`의 `AnimNotify_<이름>` 함수 탐색)을 따라 **Lua + C++ 양쪽**에 보낸다(예: Lua `OnAnimNotify_Footstep()`, 게임 모듈 `OnAnimNotify("Footstep")`). 언리얼은 스테이트에 이름 방식이 없지만(클래스 필수) 여기서는 스테이트도 이름 방식으로 시작/진행(매 프레임)/끝 세 번 보낸다. 코드 없이 고르는 기본 종류(소리 재생/파티클 생성)는 범위 밖(후속).
결정(착수 시, 2026-09-30 사용자 선택): 저장 위치는 **별도 사이드카 `<모델>.emeta`** (`.eimport`는 바뀌면 모델을 다시 쿠킹하므로 제외). Phase 14 소켓도 같은 파일.
정한 규칙(구현): 멈춤(진행 0)이면 없음 / 역재생은 거꾸로 같은 규칙 / 루프 경계는 끝까지+처음부터로 나눠 판정 / 반복 없는 클립은 끝 시각 노티파이 한 번 / 클립 전환(크로스페이드 포함)은 이전 클립의 진행 중 스테이트를 즉시 End하고 사라지는 클립 노티파이는 발생하지 않음 / 스크럽(SetTime)은 점 노티파이 없이 스테이트를 End 후 다음 진행에서 그 시각 기준 Begin / 이벤트는 다음 프레임 전달(게임 모듈 OnUpdate 직전, Lua OnUpdate 뒤) / Lua 수신자 = 노티파이가 난 모델 루트의 스크립트, 없으면 가장 가까운 조상 스크립트 / 이름은 식별자만(`[A-Za-z_][A-Za-z0-9_]*`, 편집기가 고쳐 저장) / 게임 모듈 인터페이스 버전 2(`IGameModule::OnAnimNotify`).

- [x] 데이터: `FModelMetadata`(Scene/ModelMetadata.h) 클립별 노티파이/스테이트, JSON 읽기·쓰기(비면 파일 삭제) + 테스트
- [x] 발생 판정 (순수 함수 `AnimNotifyMath::Collect/EndAll`, Scene/AnimNotify.h): 구간 교차, 루프 경계, 역재생, 스크럽 재동기, 한 프레임 안 Begin+End + 테스트
- [x] `FAnimationSystem` 연동 → `FAnimationRuntime::PendingNotifies` → Lua(`FLuaRuntime::DispatchAnimNotifies`) / 게임 모듈(`FGameModuleHost::Update`)
- [x] 편집 창: 애니메이션 편집기 노티파이 트랙(점=마름모, 스테이트=막대·겹치면 줄 추가), 더블클릭 추가/우클릭 메뉴(추가·복제·삭제)/끌기 이동·길이, 이름·시각 편집, 실행 취소(창 Undo), 재생 중 발생 항목 강조 + 최근 발생 목록. 모델 편집기 저장 = .emeta
- [x] 검증: 테스트(AnimNotifyTests 4개 + 스크립트 1개), 샘플 `Fox.glb.emeta`(Walk: Footstep_L/R, Stride) + `Scripts/FootstepLogger.lua` + `Scenes/Demo_Sockets.escene` 플레이에서 로그 순서 확인, 편집 창 스크린샷, 저장 안 함 닫기

## Phase 14 — 메시 소켓 (2026-09-30, 사용자 요청)

**DoD**: 메시(스태틱/스켈레탈) 편집 창에서 소켓(부착 지점)을 추가·이름 변경·삭제하고 기즈모로 위치/회전을 조정해 저장한다. 스켈레탈 메시 소켓은 뼈를 부모로 가진다. 씬에서 다른 엔티티를 소켓에 붙이면 애니메이션·이동을 따라다니며, 저장/불러오기·플레이 모드 복제에서 유지된다.

결정: 범위는 "편집 + 씬에서 붙이기까지". 편집 창 안의 미리보기 부착 물체(언리얼 미리보기 에셋)는 범위 밖. 저장 위치는 Phase 13과 같은 곳(.emeta).
구현 방식: 부착 컴포넌트 `FSocketAttachmentComponent`(대상 엔티티 + 소켓 이름). 부착된 엔티티의 트랜스폼은 소켓 기준 로컬 값이 되고 계층 부모는 무시한다(소켓을 찾지 못하거나 대상이 자기 하위면 평소 계층). `FScene::UpdateTransforms`가 부착 엔티티를 대상 모델 뼈 계산 뒤로 미뤄 처리(부착의 부착 포함). 로컬↔월드 변환은 `FScene::GetParentWorldMatrix`(기즈모·물리 사용).

- [x] 데이터: `FModelSocket`(이름, 뼈(비면 모델 루트), 위치/회전/스케일), 읽기·쓰기 + 테스트
- [x] 편집 창: 스태틱/애니메이션 편집기 공통 소켓 목록(추가/삭제/이름 중복 방지), 뼈 선택, 위치·회전·스케일 입력, 미리보기 축 표시 + 기즈모(이동/회전), 실행 취소
- [x] 부착 컴포넌트 → 트랜스폼 갱신 편입, 리플렉션 등록(인스펙터 전용 UI: 대상 모델/소켓 목록 선택, 상태 표시), `FSceneCloner` 재매핑(엔티티 프로퍼티 + `FModelComponent::Runtime` 노드 참조)
- [x] Lua API: `entity:AttachToSocket(target, "이름")`(소켓 위치로 맞춤, 없으면 false), `entity:DetachFromSocket()`(월드 위치 유지)
- [x] 검증: 테스트(뼈 추적, 부착의 부착, 순환 거부, 복제, 저장/불러오기), 샘플 씬 플레이에서 여우 머리 공이 걷는 동작을 따라감(스크린샷), 인스펙터 UI 스크린샷
- [ ] 후속(범위 밖): 편집 창 미리보기 부착 물체, 계층 패널에서 끌어 붙이기, 소켓에 붙인 물리 바디 처리 정책

## Phase 15 — 프리팹 (2026-09-30, 사용자 요청: 유니티 프리팹 수준)

**DoD**: 계층 패널의 엔티티 묶음을 콘텐츠 브라우저로 끌어 놓으면 프리팹 파일이 된다. 스크립트 프로퍼티에 프리팹을 끌어 넣고 Lua에서 복제 생성한다. 씬에 놓인 인스턴스는 원본과 연결되어 원본 변경이 반영되고, 인스턴스에서 바꾼 값(오버라이드)은 유지되며 되돌리기/원본에 적용이 된다. 프리팹 안에 프리팹을 넣을 수 있고(중첩), 프리팹 전용 편집 창에서 고친다.

결정: 3단계(유니티 수준) — 복사 + 연결 유지(오버라이드) + 중첩 + 전용 편집 창. 기존 `FSceneEditOps::CloneSubtree`와 `FSceneSerializer`(현재 씬 전체 단위만) 기반.

추가 결정 (2026-09-30): 인스턴스는 씬에 일반 엔티티로 저장하고 연결 컴포넌트(`FPrefabInstanceComponent` 루트 / `FPrefabLinkComponent` 엔티티마다)를 단다. 오버라이드는 "바꾼 항목의 키 목록"을 씬에 저장(값은 컴포넌트에). 엔티티 ID는 프리팹 생성 시 부여하는 고정 번호, 중첩 안쪽은 "바깥/안쪽"(`"3/2"`).

- [x] 서브트리 직렬화 + 프리팹 파일 형식 + 테스트 — `Scene/EntityJson`(씬·프리팹 공용 엔티티 목록), `.eprefab` = `{Version, NextId, Entities}`, `FPrefabLibrary`(Scene 모듈)
- [x] 계층 → 콘텐츠 브라우저 드롭으로 생성 (폴더/빈 공간/경로 표시줄이 엔티티 페이로드를 받음, 선택된 최상위 엔티티마다 한 파일). 원래 엔티티는 그 인스턴스가 된다
- [x] 스크립트 프로퍼티 에셋 타입 `EScriptValueType::Asset` (Lua `Prefab("…")`/`Asset("…", ".ext")`, 오버라이드 JSON `{"Asset": "…"}`) + 인스펙터 드롭 칸 + `Scene.SpawnPrefab(prefab, position?, onSpawned?)`. 계획과 다름: 생성은 반환값 대신 콜백 — 그 프레임 스크립트 갱신이 끝난 뒤 만들고 콜백에 루트를 넘긴다(지연 생성)
- [x] 인스턴스 연결: 오버라이드 기록(편집 커밋·씬 저장 직전 원본과의 차이), 원본 변경 반영(씬 로드·편집 창 저장·파일 감시), 프로퍼티/컴포넌트/전체 되돌리기, 원본에 적용, 연결 해제, 실행 취소(스냅샷 복원 시 동기화)·플레이 모드 복제(리플렉션 Entity 재매핑)·복제(Ctrl+D) 연동, 원본 이동/이름 변경 시 씬·프리팹·스크립트 오버라이드(JSON 안 JSON) 참조 갱신
- [x] 중첩 프리팹 (로드 시 안쪽 원본 + 저장된 중첩 오버라이드로 다시 맞춤, 순환은 저장/드롭 시 거부 + 로드는 멈추지 않음)
- [x] 프리팹 편집 창 (`FPrefabEditor`: 계층 + 인스펙터, 중첩 프리팹 드롭, 저장 → 열린 씬 인스턴스 반영). 계획과 다름: 미리보기 기즈모 편집은 없음(값은 인스펙터로)
- [x] 샘플: `Prefabs/Lamp`·`LampPost`(중첩 + 중첩 오버라이드)·`Ball`, `Scenes/Demo_Prefabs.escene`(오버라이드된 인스턴스, `Scripts/PrefabSpawner.lua`)
- [x] 자동 검증: 단위 테스트(`EditorTests/PrefabTests`, `ScriptingTests/PrefabSpawnTests`), 에디터 화면(인스턴스/오버라이드 표시, 프리팹 편집 창, 플레이 중 Lua 생성) 디버그 레이어 오류 0건
- [ ] 실행 검증 (사용자 확인): 계층 → 콘텐츠 드롭으로 만들기, 뷰포트/계층 드롭 배치, 인스턴스 값 바꾸기 → 하늘색 표시·우클릭 되돌리기·원본에 적용, 편집 창 저장 후 반영, Ctrl+Z, 스크립트 칸에 프리팹 끌어 넣기

알려진 제한: 인스턴스 안에서 원본 엔티티의 부모를 바꾸면 다음 동기화 때 원본 계층으로 돌아간다. 모델(`FModelComponent`) 경로가 원본에서 바뀌면 이미 만든 모델 하위 노드는 다시 만들지 않는다. 변형(Variant) 프리팹(루트가 다른 프리팹의 인스턴스)은 지원하지 않는다. 열린 다른 프리팹 편집 창은 안쪽 원본 저장을 즉시 반영하지 않는다(다시 열면 반영).

## Phase 17 — 멀티플레이 (2026-09-30, 사용자 요청)

진행 순서 (2026-09-30 사용자 결정): Phase 8 물리 품질 마무리 → Phase 17 → Phase 16 인게임 UI. (물리 복제가 붙기 전에 물리 조정을 끝낸다)

**DoD**: 전용 서버(`ProjectEServer.exe`, 창·GPU 없음) 또는 리슨 서버에 클라이언트가 LAN으로 접속해, 서버가 시뮬레이션한 씬(스크립트·물리 포함)이 클라이언트에 보간되어 보이고, 각 플레이어가 자기 캐릭터를 입력으로 조작하며, Lua/C++ RPC가 오간다. 에디터에서 "리슨/전용 서버 + 클라이언트 N개"로 플레이할 수 있다. 1인용(Standalone)은 기존과 똑같이 동작한다. 지연/손실 시뮬레이션에서 크래시·불일치 없음.

결정 (2026-09-30):
- 서버 권한형. 리슨 서버 + 전용 서버 둘 다. P2P/락스텝 아님
- 범용 기반만: 복제/RPC/연결/소유권 + 스냅샷 보간. 클라이언트 측 예측·서버 보정·랙 보상은 후속 과제(훅만 남긴다)
- 전송 계층: GameNetworkingSockets(암호화 BCrypt). protobuf 때문에 빌드가 무거워도 GNS 유지(필요하면 vcpkg 등 도입 검토). 전송 계층은 인터페이스로 분리(GNS 구현 + 테스트용 루프백)
- 1차 범위: Lua 네트워크 API, 물리 복제, 에디터 다중 클라이언트 플레이, LAN 로비/세션. 온라인 서비스(스팀 등)는 범위 밖
- **넷 모드**: `Standalone`(기본, 1인용) / `ListenServer` / `DedicatedServer` / `Client`. Standalone은 네트워크 드라이버·소켓을 만들지 않고 로컬 프로세스가 서버이자 클라이언트로 취급된다(UE `NM_Standalone`과 같음) → `Net.IsServer()`=true, 모든 엔티티 `IsLocallyOwned`=true, `ServerOnly` 스크립트 실행, RPC는 즉시 로컬 호출. 기존 1인용 게임/스크립트는 수정 없이 동작
- 스크립트 실행 위치: `FScriptComponent`에 `ServerOnly`(기본) / `ClientOnly` / `Both`. 클라이언트는 복제 결과만 받고 연출용 스크립트만 `ClientOnly`
- 복제 단위: `FReplicatedComponent`가 붙은 엔티티만(소유 연결, 복제 주기). 그 엔티티의 리플렉션 등록 프로퍼티는 기본 복제, `PF_NoReplicate`로 제외
- 엔티티 식별: `FNetIdComponent`. 씬에 원래 있는 엔티티는 양쪽이 같은 씬을 로드한 순서로 결정적 ID, 동적 생성은 서버가 부여
- 틱: 서버 시뮬레이션 60Hz, 전송 30Hz(설정). 클라이언트는 약 100ms 뒤 스냅샷 보간
- 입력: 클라이언트가 틱마다 입력 커맨드 전송. 서버에서 Lua `Input`은 해당 엔티티 소유 플레이어의 입력을 돌려준다

- [x] 1. GNS 도입 시험 (2026-09-30): GNS v1.6.0 정적(BCrypt, ICE 끔) + protobuf v21.12(abseil 비의존 마지막 버전, `OVERRIDE_FIND_PACKAGE` + 리디렉트 `protobuf-extra.cmake`로 `protobuf_generate_cpp` 제공), 서드파티는 `/W0`(엔진 `/WX`와 격리), GNS 디렉터리만 `WIN32_LEAN_AND_MEAN` 제거, 프로젝트에 C 언어 활성화. `Tests/NetworkTests` `Gns_LocalhostEcho` 통과(Debug/Release). Release 전체 빌드 163초(엔진 재빌드 포함). 주의: GNS 리슨 소켓은 포트 0(자동 할당)을 받지 않는다
- [x] 2. 공용 월드 틱 (2026-09-30): 새 `World` 모듈 `FGameWorld`(엔진 DLL) — `BeginPlay/EndPlay`, `TickGameplay`(스크립트 → 에셋 해석 → 게임 모듈 → 물리 → 트랜스폼), `TickPresentation`(애니메이션 → 트랜스폼 → 파티클, 에디터는 편집 중에도), 스크립트 물리 훅 연결도 한곳으로. 런타임/`FPlayMode`/에디터가 사용(중복 제거). `FPhysicsSystem::SetInterpolation(false)` = 최신 스텝 값. 테스트 `GameWorld_LifecycleAndPhysicsHooks`, `Physics_InterpolationCanBeDisabled`, 전체 9묶음 통과, Verify 4건(런타임 물리/애니메이션, 에디터 플레이/파티클) 오류 0건·화면 동일
- [x] 3. 창 없는 실행 (2026-09-30): `FApplicationDesc::bHeadless`(창 없이 고해상도 대기 타이머로 고정 간격 틱, `--exit-after`는 틱 수, 콘솔 Ctrl+C → `RequestExit`) + `Server/` → `ProjectEServer.exe`(콘솔, `--project --scene`, `FGameWorld` + 물리 보간 끔, Resources 없음). 180틱 = 3.00초(60Hz), 데모 씬 8개 오류 0건, 런타임/에디터 Verify 오류 0건. `--port`는 4단계에서 추가. 알려진 제한: 서버는 GPU 리소스가 없어 에셋 해석을 안 하므로 모델 하위 노드(뼈대)가 생기지 않는다 → 서버 애니메이션/소켓이 필요해지면 CPU 전용 모델 로드 경로 필요
- [x] 4. Network 모듈 기반 (2026-09-30): `ENetwork`(엔진 DLL, GNS 헤더는 모듈 .cpp에서만) — `INetTransport`(신뢰/비신뢰 메시지, 이벤트는 Poll로만) + `CreateGnsTransport`/`FLoopbackTransport`(테스트용 메모리), `ENetMode`, 메시지 `[uint8 종류][본문]`(`NetProtocolVersion`), `FNetDriver`(Hello → 프로토콜/엔진 버전·프로젝트·씬 확인 → Welcome(플레이어 ID, 호스트 0·원격 1부터) 또는 Reject 사유 후 끊기, 5초 핸드셰이크 시간 초과, 입장/퇴장 콜백), `FNetLaunchOptions`(`--host`/`--connect ip:port`/`--port`, 기본 7777). 서버·런타임 연결. `NetworkTests` 8개, 종단 검증: 전용 서버 + 런타임 클라이언트 입장/퇴장, 씬 불일치 거부 사유 전달, 리슨 서버 + 클라이언트
- [x] 5. 복제 핵심: 리플렉션 바이너리 직렬화(`EntityJson` switch 본뜸), NetId 매핑, 생성/파괴 메시지(프리팹 경로 또는 컴포넌트 스냅샷), 연결별 확인 기준 델타, 클라이언트 보간, 접속 시 `.eproject` `PlayerPrefab` 생성 + 소유권
  - [x] 5a (2026-09-30): `ReplicatedComponent`(OwnerPlayerId) + 런타임 `FNetIdComponent`, 정적 NetId(로드 순서, 양쪽 동일)/동적(서버, 1<<20부터), `FReplicationServer`(30Hz, 컴포넌트 단위 변경 감지 → 신뢰 채널, 생성: 엔티티=이름+부모 / 프리팹=경로+링크 ID→NetId, 파괴, 늦은 입장자 전체 상태), `FReplicationClient`(적용, 리소스 핸들 비워 재해석). 제외 규칙 `TF_NoReplicate`(계층/생성됨/프리팹 연결)·`PF_NoReplicate`·리소스 핸들. 클라이언트는 게임 로직을 돌리지 않는다. 테스트 4개 + 종단 검증(`Demo_Multiplayer.escene` 리슨 호스트/클라이언트 화면 일치)
  - [x] 5b (2026-09-30): 트랜스폼은 생성/입장 때만 신뢰, 이후 비신뢰 `TransformSnapshot`(서버 시각, 멈춘 뒤 1초 재전송) → `FReplicationClient::Update`가 서버 시각 추정 − 0.1초 시점 보간(외삽 없음). `.eproject` `"PlayerPrefab"` + `FNetPlayerSpawner`(입장 → 생성 + `OwnerPlayerId`, `PlayerStart` 이름 엔티티 순환, 퇴장 → 제거, 리슨 호스트도 플레이어 0). 테스트 `Replication_TransformInterpolatesBehindServer`, `Replication_PlayerPawnSpawnOwnershipAndLeave`, 종단 검증 화면 일치. 알려진 제한: 생성 후 부모 변경은 복제 안 됨, 리슨 호스트는 물리 보간 값을 보낸다(6단계)
- [x] 6. 물리 복제 (2026-09-30): `EWorldRole`(Authority/Client) — 클라이언트 역할은 스크립트/게임 모듈 없이 물리만 돌리고, `FPhysicsSystem::SetKinematicOverride`로 복제 엔티티(NetId)의 동적 바디를 키네마틱으로 만들어 보간된 복제 트랜스폼을 따르게 한다(로컬 물체와 충돌, 레이캐스트 가능, 속도는 키네마틱 이동에서 계산 — 별도 전송 없음). 복제 안 된 로컬 동적 물체는 클라이언트가 시뮬레이션. 테스트 `GameWorld_ClientRoleMakesReplicatedBodiesKinematic`, 종단 검증. 알려진 제한: 리슨 호스트는 렌더 보간된 물리 값을 보낸다(최대 한 스텝 뒤)
- [x] 7. Lua API: `Net.IsServer/IsClient/LocalPlayerId`, `entity:IsLocallyOwned()`, RPC(`Server_*`/`Client_*`/`Multicast_*` 접두사, `self:CallServer("Name", ...)` 등), `OnPlayerJoined/Left`, 스크립트 실행 위치
  - [x] 7a (2026-09-30): `FScriptComponent::ExecutionLocation`(0 ServerOnly 기본 / 1 ClientOnly / 2 Both, 인스펙터), `FScriptNetHooks`(Scripting은 Network 비의존, `FGameWorld`가 연결), `FGameWorld::BeginPlay(Scene, ENetMode)` — 필터: Standalone/리슨 전부, 전용 서버 S/B, 클라이언트 C/B (클라이언트 역할도 스크립트는 돈다, 게임 모듈은 서버만). Lua `Net.IsServer/IsClient/GetMode/GetLocalPlayerId`(전용 서버 -1), `entity:GetOwner()`(가장 가까운 복제 조상), `entity:IsLocallyOwned()`(서버 소유는 서버에서, 플레이어 소유는 그 기계에서). 테스트 2개
  - [x] 7b (2026-09-30): RPC — `entity:CallServer/CallClient/CallMulticast(이름, 인자...)` → 그 엔티티 스크립트의 `Server_/Client_/Multicast_<이름>` (계획의 `self:CallServer` 대신 엔티티 메서드: 모든 스크립트에서 같은 형태). 라우팅은 `FGameWorld`(`FScriptNetHooks::SendRpc`): Server = 서버에서 바로 / 클라이언트는 전송(서버는 보낸 플레이어가 소유자일 때만 실행), Client = 소유자(호스트면 로컬), Multicast = 서버 로컬 + 모든 클라이언트, Standalone = 전부 로컬. 인자 nil/bool/숫자/문자열/Vector3/에셋/엔티티(NetId). 메시지 `ScriptRpc`(신뢰), 프로토콜 4. 테스트 2개(Standalone, 전용 서버 ↔ 클라이언트 루프백: 소유자 검증·엔티티 인자·클라이언트 쪽 오류)
  - [x] 7c (2026-09-30): 입력 커맨드 `PlayerInput`(클라이언트 → 서버, 비신뢰, 매 게임플레이 틱 상태 전체 + 순번) → 서버는 플레이어별 `FInput`(`SetState`, 틱마다 `EndFrame`). 스크립트 인스턴스마다 `FScriptNetHooks::ResolveInput` — 서버에서 Lua `Input`은 엔티티 소유 플레이어의 입력(서버 소유/호스트 소유 = 로컬, 전용 서버의 서버 소유 = 없음), 클라이언트는 자기 입력. `OnPlayerJoined(id, pawn)`/`OnPlayerLeft(id)`를 서버 스크립트 전체에 전달(`BroadcastMethod`). 프로토콜 5. 테스트 `NetInput_ServerScriptsSeeOwnersInput`
- [x] 8. 게임 모듈 C++ API (2026-09-30): `Scene/GameRpc.h`(`EGameRpcKind`, `FGameRpcValue`, `IGameNet`) — Lua와 C++이 같은 RPC 경로(스크립트 쪽 7b 타입을 이것으로 교체). `IGameModule::OnPlayerJoined/OnPlayerLeft/OnRpc`(스크립트 메서드와 함께 호출, 서버에서만) + `GetNet()`(OnBeginPlay~OnEndPlay, `FGameWorld`가 `IGameNet` 구현 → `CallRpc`/소유권/모드). 계획의 "RPC 등록" 대신 `OnRpc` 한 곳에서 이름으로 분기(등록 없이). `FGameModuleHost::Attach`(DLL 없이 모듈 연결, 테스트용). `GameModuleApiVersion` 3. `FGameWorld` 네트워크 부분은 `GameWorldNet.cpp`로 분리. 테스트 `NetRpc_GameModuleReceivesEventsAndSendsRpc`
- [x] 9. LAN 로비/세션: Winsock UDP 브로드캐스트 방 목록(이름/인원/맵), Lua `Net.FindSessions/Host/Connect`, 에디터 ImGui 로비 창(게임 내 로비 UI는 Phase 16에서 이 API 위에)
  - [x] 9a (2026-09-30): `FLanDiscovery`(Winsock UDP, 비차단) — 질의(브로드캐스트 + 127.0.0.1, 프로젝트 필터) ↔ 응답(방 이름/프로젝트/엔진 버전/씬/게임 포트/인원/최대 인원/호스트 ID, 같은 호스트는 ID로 하나·LAN 주소 우선), 탐색 포트 7778. 전용 서버·리슨 호스트가 알림, 런타임 `--join-lan`(최대 2초 검색 → 첫 세션 접속), `FNetDriver::MaxPlayers`(기본 16, 가득 차면 거부). 테스트 2개 + 종단 검증(전용 서버를 LAN 주소로 찾아 입장)
  - [x] 9b (2026-09-30): Lua `Net.FindSessions/GetSessions/Host/Connect/Disconnect/GetState/GetFailureReason` — `FGameWorld`가 LAN 검색과 세션 전환 요청 큐(`ConsumeSessionRequest`) 보유, 런타임이 프레임 끝에 처리(`RuntimeSession.cpp`: `LoadScene/StartSession/EndSession`, Host = Standalone이면 재시작 없이 `SetNetMode(ListenServer)`, Connect/Disconnect = 씬 재로드 후 새 세션). 버그 수정: 정적 NetId를 엔티티 인덱스 순이 아니라 `ReplicatedComponent` 풀 추가 순서로 — 씬을 비우고 다시 열면 인덱스가 역순 재사용되어 어긋났다(테스트 `Replication_StaticNetIdsSurviveSceneReload`). 테스트 `GameWorld_LuaSessionApi` + 종단 검증(스크립트가 단독 실행 중 Net.Connect → 전용 서버 입장, 화면 일치)
  - [x] 9c (2026-10-01): 에디터 네트워크 패널의 LAN 세션 목록(찾기 → 방 이름/인원/주소, "런타임으로 접속"은 그 세션의 씬으로 런타임 클라이언트 실행)
- [x] 10. 에디터 다중 클라이언트 플레이 (2026-10-01): `FPlayInEditorNet`(Editor) + 네트워크 패널(창 메뉴, 기본 닫힘) — 모드 1인용/리슨(에디터 호스트)/전용 서버(`ProjectEServer.exe` + 에디터도 클라이언트), 런타임 클라이언트 창 N개, 포트. 편집 씬을 `Saved/PlayInEditor/PIE.escene`에 저장하고 에디터 플레이 씬도 복제가 아니라 같은 JSON에서 만든다(`FPlayOptions::SceneJson`, 모든 프로세스 정적 NetId 일치, 절대 경로 = 핸드셰이크 씬 이름). 정지/종료 시 자식 프로세스 종료, 클라이언트 로그 `Saved/PlayInEditor/Client<N>.log`, 서버 로그 `Server.log`. 자동 검증 인자 `--play --play-net listen|dedicated --play-clients N [--port N]`. 종단 검증: 리슨(에디터 + 클라이언트 1), 전용(서버 + 에디터 + 클라이언트 1) 입장 확인, 디버그 레이어 오류 0건. 플레이 중 Lua 세션 전환은 에디터에서 미지원(알림)
- [x] 11. 디버그·검증 (2026-10-01): 지연/손실 시뮬레이션(`INetTransport::SetSimulation` — GNS FakePacketLag/Loss_Send, 인자 `--net-lag <ms> --net-loss <%>`, 네트워크 패널 슬라이더는 플레이 중 즉시 적용·띄우는 프로세스엔 인자로), 연결 통계(`GetStats`: 핑/품질/송수신 KB/s, 패널에 플레이어별), `Verify.ps1 -Multiplayer [-Clients N] [-Scene] [-Port] [-ExtraArgs]`(전용 서버 + 런타임 클라이언트, 입장 여부·로그 오류 요약), 샘플: `Prefabs/Player.eprefab`(복제 + `Scripts/PlayerController.lua` WASD — 서버에서 소유자 입력으로 이동), `Demo_Multiplayer.escene` `PlayerStart` 4개, `Sample.eproject` `"PlayerPrefab"`, 발사대 공 복제. 검증: `-Multiplayer` 기본·지연 100ms/손실 5% 모두 클라이언트 2개 입장·오류 0건, 화면이 서버 상태를 따라감. `NetworkTests`/`ScriptingTests`는 루프백 중심(계획 그대로)
- [ ] 실행 검증 (사용자 확인)

단계 2~3은 멀티플레이 없이도 의미 있는 리팩터라 먼저 커밋. 4 이후 6/7/9는 worktree 병렬 트랙 후보.

## Phase 16 — 인게임 UI (2026-09-30, 사용자 요청: UMG처럼)

**DoD**: 에디터의 UI 디자이너에서 위젯(패널/이미지/텍스트/버튼 등)을 끌어다 배치·앵커·스타일을 편집해 UI 에셋으로 저장하고, 런타임/플레이 모드 화면에 그려지며 입력(클릭/호버/포커스)을 받고 Lua/C++에서 값 변경·이벤트 처리를 한다.

결정: 외부 라이브러리 없이 엔진이 직접 구현 + UMG식 비주얼 디자이너. ImGui는 에디터 전용으로 유지(게임 UI에 쓰지 않음).

결정 (착수 시, 2026-09-30 사용자 선택):
- 진행 순서 변경: Phase 17(멀티플레이, 다른 에이전트가 메인 트리에서 진행)과 **병렬**. worktree 브랜치 `phase16-ui`. 1단계 = 멀티플레이와 겹치지 않는 부분(UI 모듈/렌더러/에셋/디자이너), 2단계 = 17-2 `FGameWorld`가 master에 들어온 뒤 그 위에 런타임·플레이 연결(옛 중복 루프에 붙이지 않음). 전용 서버(`bHeadless`)에서는 UI를 만들지 않는다. 게임 내 로비 UI는 17-9 이후
- 텍스트: **SDF 글꼴 아틀라스** (stb_truetype — imgui 동봉 `imstb_truetype.h` 재사용, 추가 다운로드 없음). 필요한 글자만 처음 쓸 때 굽는 동적 아틀라스(R8, 1024 → 최대 4096). 기본 글꼴은 Windows 맑은 고딕(배포용 한글 글꼴 번들은 다운로드 승인 후 후속)
- 레이아웃: **캔버스(앵커/오프셋/피벗) + 가로/세로 박스(자동/채우기) + 오버레이 + 균일 그리드 + 스크롤 박스**, Slate식 원하는 크기 → 배치 2단계. 해상도는 설계 해상도 + 배율 규칙(높이/너비 맞춤, 전체 보이기, 채우기)
- 모듈: `UI`(Core만 의존, 순수 로직 — 엔진 DLL 포함), 그리기는 `Renderer/FUIRenderer`(UI.hlsl 인스턴스 사각형: 둥근 모서리/테두리 SDF + 글꼴 SDF 외곽선/그림자)

1단계 (worktree, 2026-09-30):
- [x] 위젯 트리 + 레이아웃(앵커/정렬/크기) — `FUIWidget`/`FUILayout`, 테스트 9개 (캔버스 점/늘이기 앵커, 박스 자동/채우기, 접힘/숨김, 줄바꿈 텍스트, 오버레이/보더/그리드, 스크롤 제한/잘림, 배율, 슬롯 편집 역산)
- [x] UI 렌더러 (사각형/이미지/텍스트, 해상도 스케일) — `FUIPainter`(그리기 목록, 텍스처·잘림별 묶음) + `FUIRenderer`, `FResourceManager::CreateTexture(Raw)`(R8 아틀라스)
- [x] 입력 라우팅 — `FUIInputRouter`(맞히기/호버/눌림/클릭/포커스/Tab/휠, 입력 차단 여부) + 테스트 3개, 디자이너 "미리보기 입력"에서 동작. 게임 입력과의 우선순위는 2단계에서 연결
- [x] UI 에셋 형식 + 바인딩 — `.eui` v1(`FUIAsset`, JSON 왕복 테스트, 하위 트리 복사/붙여넣기), `FUIInstance`(런타임 인스턴스). Lua 바인딩/씬 컴포넌트는 2단계
- [x] 디자이너 편집 창 — `FWidgetEditor`: 계층(끌어서 부모 변경/순서/복제/복사·붙여넣기/삭제), 팔레트(끌어 놓기/클릭 추가), 캔버스(실제 UI 렌더러, 휠 확대, 가운데·오른쪽 드래그 이동, 선택·이동·8방향 크기, 스냅, 앵커 표시, 미리보기 해상도 프리셋), 속성(UI 설정/공통/슬롯(부모별)/종류별/브러시·텍스처 드롭/글꼴), 앵커 프리셋 4x4. 콘텐츠 브라우저 "새 UI", 아이콘, 참조 갱신(Content 기준). 샘플 `UI/SampleHUD.eui`
- [x] 1단계 검증: 단위 테스트 전체 통과(UITests 17개 포함), `Verify.ps1 --open-asset UI/SampleHUD.eui [--ui-select PlayButton --ui-zoom 1]` 스크린샷 확인(한글 SDF/둥근 모서리/테두리/선택 표시), `--verify-asset-close`로 UI 편집 창 저장 안 함 닫기 무오류, 디버그 레이어 오류 0건

2단계 (2026-09-30, master의 17-1~4 머지 후 `FGameWorld` 위에):
- [x] 씬 컴포넌트 `FUIComponent`(에셋/Z 순서/보임/입력 받기/키보드 포커스) + `RegisterUITypes()`(에디터/런타임/서버 — 서버는 타입만) + 런타임 상태 `FUIComponentRuntime`(복사하면 비워짐 → 플레이 복제/엔티티 복제가 인스턴스를 공유하지 않음), `FUIAssetLibrary`(경로 캐시, 수정 시각이 바뀌면 다시 읽음), 뷰포트 끌어 놓기/콘텐츠 브라우저 "씬에 추가", 계층/인스펙터 아이콘
- [x] 런타임/플레이 모드 그리기: `FUISystem::Paint` → `FUIRenderer` (런타임 = 백버퍼, 에디터 = 플레이 중 뷰포트 타깃, 씬·그리드·아웃라인 위). 픽셀 아트 모드도 최종 해상도에 그려진다
- [x] 입력 우선순위: `FUISystem::Update`(Z 순서 큰 UI부터, 위 UI가 가져가면 아래는 포인터 없음)가 `FGameWorld::TickGameplay` 전에 돌고, 포인터를 가져가면 게임에는 `FInput::WithoutMouseButtons()`(키보드 유지). UI 버튼을 누른 채 끌어 나가도 뗄 때까지 가져간다. 에디터는 뷰포트 이미지 위 포인터만, 가져간 프레임에는 클릭 선택 안 함. 키보드 포커스(Tab/Enter/Space)는 컴포넌트 `KeyboardFocus`를 켠 UI만
- [x] Lua 바인딩(`Scripting/ScriptUIBindings.cpp`): `entity:GetWidget("이름")` → `UIWidget`(Name/Type/Text/Percent/Visible/Visibility/Enabled/Opacity/Color/Texture/FontSize), `entity:IsPointerOverUI()`, 이벤트 `OnUIClicked_/OnUIPressed_/OnUIReleased_/OnUIHoverBegin_/OnUIHoverEnd_<이름>`(UI 엔티티 또는 가장 가까운 조상의 스크립트). C++ 게임 모듈은 `FUIComponent::Runtime`(Instance/Events)을 직접 읽는다 — **`IGameModule` API는 17-8과 겹치지 않도록 바꾸지 않음**
- [x] 검증: UITests 20개(UI 시스템 Z 순서/입력 끔/숨김/복제/다시 읽기/입력 사본 추가), ScriptingTests `UIScript_WidgetValuesAndClickEvent`, 전체 10묶음 통과. 샘플 `Demo_UI.escene` + `Scripts/HudController.lua`(체력/점수 갱신, 버튼으로 메뉴 닫기, M 키로 다시 열기): 런타임·에디터 플레이 스크린샷 확인, 디버그 레이어 오류 0건, 전용 서버 60틱 로드 정상
- [ ] 실행 검증 (사용자 확인): 에디터 플레이에서 버튼 호버/클릭, M 키, 디자이너에서 편집 → 저장 → 다시 플레이 반영

멀티플레이와의 연결 (17단계 쪽에서 할 일): HUD 스크립트는 `ClientOnly`가 맞다(17-7 스크립트 실행 위치 도입 시 샘플 `HudController`를 ClientOnly로). UI는 복제 대상이 아니다(클라이언트 로컬). 게임 내 로비 UI는 17-9 API 위에.

후속 과제 (2026-10-01 진행, 사용자 선택: Noto Sans KR 번들 / UMG식 타임라인):
- [x] 글꼴 성능: 원인은 SDF가 아니라 13MB 글꼴 파일 바이트 단위 읽기(Debug 853ms → 일괄 읽기 15ms). 인스턴스 생성 시 에셋 텍스트 글자 미리 굽기. 굽기 속도 로그 테스트
- [x] 기본 글꼴 Noto Sans KR Regular 번들 (`Engine/Content/Fonts`, notofonts/noto-cjk `Sans2.004`, SHA256 고정, OFL 동봉, Package.ps1이 Engine/Content 복사). 순서: 프로젝트 지정 → 번들 → 맑은 고딕. **발견**: stb_truetype SDF는 3차 곡선(CFF)을 버려 OTF가 깨짐 → 4배 래스터 + Felzenszwalb 거리 변환 + 축소(`UI/UISdf`, 테스트)로 교체 (글자당 약 3ms, Debug)
- [x] 9-slice 브러시 (`EUIBrushDrawAs::NineSlice`, Margin = 텍스처 비율, 두께 = Margin × 원본 크기, 작으면 비율 축소), 디자이너 편집/"텍스처 크기로", 샘플 로그 패널 틀(`UI/PanelFrame.png`)
- [x] 텍스트 상자 `TextBox` (+ Core 문자 입력 `EWindowEventType::Char`, `FInput::GetTypedText/IsKeyRepeated/WithoutKeyboard`): 클릭/Tab 포커스, 입력/지우기/화살표/Home/End, 최대 길이, Enter 확정·Esc 해제·포커스 잃으면 확정, 클릭 위치 캐럿, 가로 스크롤, 안내 문구. 입력 중에는 게임에 키보드를 넘기지 않고 ESC도 UI가 받는다(`FUIInputResult`). Lua `OnUITextChanged_/OnUITextCommitted_`. 샘플 채팅 입력칸. 한계: IME 조합 중인 글자는 완성될 때까지 보이지 않음(WM_CHAR만 받음), 선택/복사/붙여넣기 없음
- [x] UMG식 애니메이션: 렌더 변환(이동/배율/피벗, 레이아웃 영향 없음, 자식 누적 → `State.Visual*`로 그리기/맞히기), `.eui` v2 `Animations`(트랙 = 위젯 이름 + 속성: 불투명도/이동/배율/색 RGBA/진행률, 키마다 보간 선형/계단/이징), `FUIInstance` 재생(반복/속도/거꾸로, 끝 이벤트), Lua `entity:PlayUIAnimation/StopUIAnimation/IsUIAnimationPlaying` + `OnUIAnimationFinished_<이름>`. 디자이너 타임라인(애니메이션 추가/이름/삭제/길이, 재생·반복, 재생 헤드, 키 추가(현재 값), 키 끌기/값/보간/삭제, 트랙 삭제, 재생 헤드 시점 미리보기 — 복제본이라 기본값 불변). 샘플 `MenuIntro`/`ScorePulse`. 자동 검증 인자 `--ui-animation <이름> --ui-anim-time <초>`. 한계: 회전 없음
- [ ] 남은 후속: 로컬라이즈(문자열 표), 텍스트 상자 선택/클립보드/IME 조합 표시, 애니메이션 녹화 모드(속성 편집 → 자동 키)

## Phase 18 — AI: 비헤이비어 트리 + 내비게이션 (2026-09-30, 사용자 요청: 언리얼 비헤이비어 트리처럼)

**DoD**: 에디터의 노드 그래프 편집 창에서 비헤이비어 트리(컴포지트/데코레이터/서비스/태스크)와 블랙보드 키를 편집해 `.ebt`로 저장하고, 엔티티에 `FBehaviorTreeComponent`를 달면 플레이/런타임에서 실행된다. 블랙보드 값이 바뀌면 조건 데코레이터가 실행 중인 가지를 중단(abort)한다. Recast로 구운 내비메시 위에서 `MoveTo`가 장애물을 돌아 목표까지 이동한다. 노드는 C++(엔진/게임 모듈)과 Lua 스크립트 둘 다로 만들 수 있다. 플레이 중에는 편집 창에 실행 중인 노드가 강조되고, 뷰포트에 내비메시/경로 디버그 표시를 켤 수 있다.

결정 (2026-09-30):
- 길찾기: **Recast/Detour 도입** (`ThirdParty.cmake`에 커밋 고정, `ThirdParty::recast`). Recast 헤더는 AI 모듈 내부 .cpp에서만 포함한다. 좌표·단위 변환은 `FNavMesh` 경계에서만 한다(엔진 Z-up 왼손 ↔ Recast Y-up: 축을 바꾸면 반사가 생기므로 삼각형 인덱스 순서도 함께 뒤집는다. 변환 규칙은 테스트로 고정)
- 그래프 편집기: **외부 라이브러리 사용** (에디터 전용). 1순위는 `imgui-node-editor`(thedmd)이고, 현재 ImGui 1.93 WIP(docking)와 호환되지 않으면 `imnodes`로 대체한다. 착수할 때 호환성부터 시험한다
- 노드 작성: **C++과 Lua 둘 다 지원**. C++ 노드는 `FBehaviorTreeNodeRegistry`에 등록한다(엔진 기본 노드 + 게임 모듈 `OnLoad`). Lua 노드는 스크립트 에셋이 `Properties` + `OnExecute/OnTick(dt)/OnAbort`(태스크), `CanExecute`(데코레이터), `OnTick`(서비스)을 정의하고 `"Running"|"Success"|"Failure"`를 반환한다. 노드 파라미터는 기존 리플렉션 프로퍼티 타입(`EPropertyType`)으로 기술해 편집 창이 자동으로 그린다
- **멀티플레이(Phase 17)**: AI는 서버에서만 실행한다(Standalone = 서버). 클라이언트는 결과(트랜스폼 등)를 복제로만 받는다. 블랙보드는 복제하지 않는다
- 모듈: 새 `AI` 모듈(`EAI`, 엔진 DLL에 포함, 의존 AI → Scene → Core). 컴포넌트는 `RegisterAITypes()`로 등록하고 앱이 씬 로드 전에 호출한다. 실행 상태(노드 인스턴스, 블랙보드 값, Detour 쿼리)는 `FAISystem`이 엔티티별로 가진다(컴포넌트에는 런타임 상태 없음, 물리와 같은 방식)
- 실행 모델: UE식 이벤트 기반. 매 프레임 루트부터 다시 평가하지 않고 실행 중인 태스크만 틱한다. 데코레이터 중단 모드는 `None/Self/LowerPriority/Both`. 갱신 순서: 스크립트 → 게임 모듈 → **AI** → 물리
- 블랙보드: 키 정의는 `.ebt` 안에 둔다(UE의 별도 블랙보드 에셋은 필요할 때 확장). 키 타입은 Bool/Int/Float/Vector/Entity/String
- 이동: `MoveTo`는 Detour 경로를 따라 이동한다. 강체가 있으면 속도를 설정하고, 없으면 트랜스폼을 직접 옮긴다. 무리 회피(DetourCrowd)는 후속 과제
- 내비메시 굽기: 에디터에서 씬의 정적 메시/정적 콜라이더를 모아 굽고 `<씬>.enav`(씬 옆, 원본 데이터)로 저장한다. 설정(에이전트 반경/높이/경사/계단)은 씬의 `FNavMeshSettingsComponent`에 둔다. 런타임은 씬을 로드할 때 `.enav`를 읽는다(패키징에 포함)

- [x] 1. 외부 라이브러리 도입 시험 (2026-09-30): Recast/Detour v1.6.0(Recast/Detour/DetourCrowd 소스만 `ThirdParty::recast` 정적 라이브러리)과 imgui-node-editor master `021aa0e`(`ThirdParty::imgui_node_editor`, EEditor에 PRIVATE 링크)를 `/W0`로 추가. ImGui 1.92.0부터 `operator*(float, ImVec2)`가 중복 정의되어 `CMake/Patches/ImGuiNodeEditor.cmake`(PATCH_COMMAND)로 `IMGUI_VERSION_NUM < 19200`일 때만 쓰도록 감쌈. 호환성은 화면 없이 노드/핀/링크를 3프레임 그리는 `EditorTests` `NodeEditor_DrawsNodesWithEngineImGui`로 확인했다(ImGui 버전을 올리면 이 테스트가 먼저 깨진다). Recast는 컴파일만 확인했고 엔진 DLL 링크는 단계 3에서 AI 모듈과 함께 한다
- [x] 2. BT 핵심 (2026-09-30): `AI/BehaviorTree/` — `FBlackboard`(키 타입 검사, 값이 실제로 바뀔 때만 관찰자 호출), `FBehaviorTreeAsset`(`.ebt` JSON v1, 파라미터는 순서 있는 `[{Name, Value}]`, 모르는 노드 타입이면 로드 실패), `FBehaviorTreeNodeRegistry::Get()`(소유자 태그 일괄 해제, 서비스는 Interval/RandomDeviation 자동 추가, 기본 노드 Owner "Engine"), 노드 인터페이스 `FBTCompositeNode/FBTTaskNode/FBTDecoratorNode/FBTServiceNode`(`BehaviorTreeNode.h`, Lua 노드도 이것을 구현), 실행기 `FBehaviorTreeInstance`(이벤트 기반 + 요청 대기열/Serial 무효화, 틱당 방문 예산 1000, 관찰 키 변경은 다음 Tick 시작에 평가하고 조건 결과가 바뀔 때만 중단 — Self/LowerPriority/Both, 디버그 `GetActiveNodeIds`). 기본 노드 Selector/Sequence/SimpleParallel, Blackboard/Cooldown/Loop/TimeLimit/Inverter/ForceSuccess, Wait/SetBlackboard/Log. `BehaviorTreeTests` 31개. 한계: LowerPriority는 부모가 SimpleParallel이거나 실행이 부모 컴포지트 밖이면 적용 안 함, 태스크 중단은 즉시(UE의 Aborting 지연 없음), Entity 키는 IsSet/IsNotSet만, 레지스트리 해제 전 인스턴스를 먼저 파기해야 함(단계 6)
- [x] 3. 내비게이션 (2026-09-30): `AI/Navigation/NavMesh`(pimpl, Recast Solo 굽기 → Detour 경로 `FindPath`(Complete/Partial/Failed, 직선화) / `ProjectPoint` / `GetDebugTriangles`), `NavCoordinates.h`(recast (x,y,z) = (X,Z,Y)×0.01, 반사라 삼각형 (I0,I1,I2) → (I0,I2,I1)), `.enav`(매직 ENAV + 버전 + 설정 원본 바이트 + Detour 타일 + FNV-1a 해시, 로드 시 타일 크기 검증 — **`FNavMeshBuildSettings` 필드를 바꾸면 `NavFileVersion`을 올린다**). `NavMeshTests` 12개. 한계: 단일 타일(4096×4096셀/정점 65535 초과 시 실패), Crowd·동적 장애물·off-mesh 링크·영역 비용 없음, 경로 폴리 512개, 스레드 안전하지 않음(쿼리 공유), 시작/끝이 검색 범위 밖이면 Failed(단계 4에서 목표를 `ProjectPoint`로 보정)
- [x] 4. 씬 연동 (2026-09-30): 컴포넌트 `FBehaviorTreeComponent`(Asset .ebt, AutoStart) / `FNavAgentComponent`(속도, 도착 반경, 회전 속도, 이동 방향 회전) / `FNavMeshComponent`(구운 .enav 경로 + 굽기 설정 — 계획의 "씬 옆 .enav" 대신 컴포넌트가 경로를 가진다. 콘텐츠 이동/프리팹과 같은 경로 규칙을 따르게 하려고), `RegisterAITypes()`(에디터/런타임/서버). `FAISystem`(엔티티별 트리 + 이동 상태, 새/파괴 엔티티·컴포넌트 변경 동기화, 에셋 캐시, `ReloadBehaviorTree`, `RequestMove/GetMoveStatus/StopMove/FindPath`, `TurnTowards`) — `FGameWorld`가 소유, 순서 스크립트 → 게임 모듈 → **AI** → 물리, Client 역할은 AI 없음, BeginPlay는 스크립트(Lua 상태 생성) 다음, EndPlay는 스크립트보다 먼저(Lua 노드 OnAbort). 이동: 동적 강체는 물리에 수평 속도(Z 속도 유지), 그 밖은 트랜스폼을 경로 높이를 따라 직접 옮김. 내비메시가 없으면 직선 이동(경고 한 번). 태스크 `MoveTo`(Vector/Entity 키, 목표가 RepathDistance 넘게 움직이면 재탐색)/`RotateTo`/`PlayAnimation`. `AISystemTests` 7개. 남은 것: 에디터 `.ebt` 변경 감지 → `ReloadBehaviorTree` 연결(단계 7), FGameWorld 수준 Client 역할 테스트
- [x] 5. Lua (2026-09-30): Scripting은 AI에 비의존 — `FScriptAIHooks`(블랙보드/이동/경로/트리 제어)와 범용 스크립트 객체 API `FScriptSystem::CreateObject/CallObject/DestroyObject`(세션 번호가 든 핸들, 지난 세션 핸들은 무시)를 두고, AI는 `FAIScriptHooks`로 스크립트 객체를 부른다. 둘을 `FGameWorld::ConnectScriptsAndAI`가 연결. Lua API: `entity:GetBlackboard()` → `Get/Set/IsSet/Clear`(값 bool/number/string/Vector3/Entity, Set(nil) = Clear), `entity:MoveTo(pos, 반경?)`/`GetMoveStatus()`/`StopMove()`/`StartBehaviorTree()`/`StopBehaviorTree()`, `AI.FindPath(a, b)`, `AI.MoveTo(entity, pos, 반경?)`. Lua 노드(`AI/LuaNodes.h` 머리 주석이 규칙): `LuaTask`(OnExecute/OnTick/OnAbort, "Running"/"Success"/"Failure" 또는 bool), `LuaDecorator`(CanExecute + ObservedKeys/AbortMode), `LuaService`(OnBecomeRelevant/OnTick/OnCeaseRelevant), 파라미터 Script + Properties(JSON 오버라이드). 새 바인딩은 `Scripting/LuaAIBindings.cpp`(LuaRuntime.cpp 변경 최소화). 수정: 트리를 등록한 뒤 Start(시작 중 노드가 블랙보드를 못 찾던 문제), FGameWorld 순서 BeginPlay 스크립트 → AI / EndPlay AI → 스크립트. `AIScriptTests` 5개(ScriptingTests)
- [x] 6. 게임 모듈 C++ API (2026-09-30): 게임 모듈은 `OnLoad`에서 `FBehaviorTreeNodeRegistry::Get().Register`(Owner를 비우면 `FTypeRegistry`의 현재 등록 소유자 = 모듈 이름)로 C++ 노드를 등록한다. 언로드는 `FGameModuleHost::AddUnloadCleanup`(Scene 밖 모듈의 소유자별 정리 콜백, `RegisterAITypes`가 등록)으로 모듈 노드를 해제한다. `IGameModule` 가상 함수는 바뀌지 않아 `GameModuleApiVersion`은 그대로 3. `GameModuleNodeTests`(AITests)
- [x] 7. 에디터 (2026-10-01): `FBehaviorTreeEditor`(`Editor/AssetEditors/BehaviorTreeEditor`, imgui-node-editor) — 위→아래 그래프, 데코레이터(◆, 노드 위)/서비스(⚙, 노드 아래)는 노드 안 줄, 자식 실행 순서 = X 좌표(UE와 같음, 노드 제목에 순서 번호), 우클릭 메뉴(자식/데코레이터/서비스 추가, 삭제), 출력 핀을 끌어 부모 변경, Delete = 하위 트리째 삭제(루트/링크만 삭제는 거부). 오른쪽: 블랙보드 키(이름/타입/추가/삭제), 선택 항목 파라미터(레지스트리 `FBTParamDesc`: Options → 콤보, 이름이 …Key면 블랙보드 키 콤보, Script는 .lua 드롭, Properties는 여러 줄), 데코레이터/서비스 목록(순서/삭제/추가). 노드 위치는 `FBTNodeDesc::EditorPosition`으로 .ebt에 저장(없으면 자동 배치). 열 때 루트 + 첫 단계 자식에 맞춤, F = 전체 보기. 플레이 중 이 에셋을 쓰는 엔티티(선택 엔티티 우선)의 실행 노드 강조 + 새로 실행된 링크 흐름 + 블랙보드 현재 값. `FAssetEditor::UsesPreview/DrawPreviewArea`(3D 미리보기 없는 편집기), 콘텐츠 브라우저 "새 비헤이비어 트리", `.ebt` 아이콘, 파일 감시 `.ebt` 변경 → `ReloadBehaviorTree`. 샘플 `AI/Patrol.ebt`. Verify: `--open-asset AI/Patrol.ebt`(스크린샷 확인, 디버그 레이어 오류 0), `--verify-asset-close` 통과. 플레이 중 강조 화면 확인은 단계 9 데모 씬에서
- [x] 8. 에디터 내비메시 (2026-10-01): 도구 → 내비메시 굽기(`FNavMeshBaker`: 보이는 정적 메시, 동적 강체/AI 에이전트와 하위 제외, 거울상 변환은 와인딩 뒤집기 — `FStaticMesh::GetCpuPositions/GetCpuIndices` CPU 사본 추가) → 씬 옆 `<씬>.enav`(저장 안 한 씬은 `Content/NavMesh.enav`) + `NavMeshComponent`(없으면 "NavMesh" 엔티티 생성) 경로 지정, 실행 취소 한 단계. 뷰포트 툴바 내비메시 표시 토글(`FNavMeshDebugRenderer` + `NavMeshDebug.hlsl`: 씬 깊이 테스트, 면 청록/테두리 흰색은 정적 GPU 버퍼, 플레이 중 이동 경로 노란 선은 동적 버퍼 상한 4096정점). 편집 씬 `NavMeshComponent` 파일이 바뀌면(열기/Undo/굽기) 표시 자동 갱신. 인자 `--bake-navmesh`, `--show-navmesh`. Verify: 샘플 기본 씬 굽기(메시 3개 → 폴리곤 10개, 헬멧 자리 구멍) 스크린샷 확인, 디버그 레이어 오류 0. `NavMeshBakerTests`(EditorTests)
- [x] 9. 검증 (2026-10-01): 샘플 `Scenes/Demo_AI.escene` + `Demo_AI.enav`(바닥/벽/상자 → 폴리곤 15개) — 경비(`AI/Guard.ebt`: 루트 Selector + Lua 서비스 `AI/SeePlayer.lua`(가까우면 Target 설정, 멀면 비움) → [Target IsSet(Both) → MoveTo Target] / [순찰 두 지점 왕복, 벽을 돌아감]), 플레이어(`Scripts/PlayerWalker.lua` 왕복, 경비보다 빠름). Verify(`--scene Scenes/Demo_AI.escene --play --show-navmesh`, 2.5/6/12초): 벽 우회 이동 경로 표시, 발견 로그, 추적 확인. `--open-asset AI/Guard.ebt` 플레이 중 실행 가지 강조 + 블랙보드 실시간 값 확인. 디버그 레이어 오류 0. 굽기 제외 규칙에 스크립트 엔티티 추가(플레이어가 장애물로 구워지던 문제). BT 편집기 열 때 1:1 배율 고정(작은 트리 과확대 수정)
- [ ] 실행 검증 (사용자 확인)

병렬 트랙: 2(BT 핵심)와 3(내비게이션)은 서로 독립이라 worktree 두 개로 동시에 진행할 수 있다. Phase 16(인게임 UI)과도 독립적이다. 충돌 지점은 `AssetEditorManager` 확장자 등록, 콘텐츠 브라우저 새 에셋 메뉴, `SceneReflection`/Lua 바인딩 추가, `ThirdParty.cmake`, 기본 창 배치, `Plans.md`/`CLAUDE.md`이며 모두 끝에 추가하는 변경이다. 단, 단계 4는 Phase 17 단계 2(`FGameWorld` 갱신 순서 통합)와 같은 코드를 건드리므로 둘 중 먼저 끝난 쪽에 맞춘다.

## 픽셀 아트 렌더링 (2026-09-30, 사용자 요청: "모자이크가 아니라 진짜 도트 게임처럼")

**DoD**: 씬에 픽셀 아트 설정 컴포넌트를 두면 씬이 (출력 ÷ PixelSize) 해상도로 렌더되고, 정수 배율 최근접 확대로 화면에 표시된다. 직교 카메라가 움직여도 도트가 반짝이지 않고(텍셀 스냅) 움직임은 부드럽다(서브픽셀 보정). 1px 외곽선/모서리 하이라이트와 팔레트 양자화 + 디더링을 켤 수 있다. 에디터/런타임 모두 적용, 설정은 씬에 저장된다.

결정 (2026-09-30 사용자 선택): 방법 = 저해상도 렌더 + 확대(포스트 프로세스 모자이크 아님). 카메라에 직교 투영 추가. 설정 저장 = 씬 단위 컴포넌트. 범위 = 저해상도+정수 확대+스냅, 픽셀 외곽선, 팔레트/디더링 전부.

- [x] 1. 카메라 직교 투영: `FCamera`(직교 높이), `FCameraComponent::bOrthographic/OrthoHeight` + 리플렉션, `FSceneCamera::ApplyToCamera`
- [x] 2. `FPixelArtComponent`(Scene, 리플렉션) + 순수 계산 `FPixelArtMath`(저해상도 크기, 텍셀 스냅/오프셋) + 테스트
- [x] 3. 렌더러: 저해상도 HDR+깊이 → 포스트(저해상도) → 합성 패스(`PixelArt.hlsl`: 서브픽셀 보정 최근접 확대 + 깊이 기반 외곽선/하이라이트 + 양자화/Bayer 디더). 깊이 버퍼 SRV 지원(`FD3D12DepthBuffer` typeless)
- [x] 4. 샘플 씬 `Demo_PixelArt.escene`(직교 아이소 카메라 + `Scripts/CameraPan.lua` 왕복) + 자동 검증: 런타임/에디터/플레이 스크린샷, 디버그 레이어 0건, 테스트 전부 통과
- [x] 5. 편집 뷰포트 직교 보기 (2026-09-30 사용자 요청): 툴바 원근/직교 토글(전환 시 초점 거리 기준으로 같은 크기), 휠 줌, F 프레이밍, 직교 기즈모, `EditorCamera.json` 저장 + 테스트
- [ ] 실행 검증 (사용자 확인): 카메라 이동 시 반짝임 없음, 창 크기 변경
- [ ] 후속: 머티리얼 텍스처 최근접 샘플링 옵션(작은 텍스처 확대 시 번짐), 직교 카메라 스페큘러 시선 벡터(현재 원근처럼 카메라 위치 기준), 픽셀 모드 에디터 그리드, 팔레트 텍스처(LUT) 기반 색 제한

## Phase 19 — 배포: 게임 패키지 → Steam (2026-10-01, 사용자 요청: "게임을 바이너리화해서 스팀 같은 데 올릴 수 있게", 1~4 순서대로)

**DoD**: `Package.ps1` 한 번으로 `<게임 이름>.exe`(아이콘·버전 정보)를 더블클릭하면 바로 실행되는 폴더가 나오고, VC++ 런타임이 설치되지 않은 PC에서도 실행된다. 로그·크래시 덤프·설정은 사용자 폴더(`%LOCALAPPDATA%`)에 남고 PDB는 패키지 밖에 따로 보관된다. 창/테두리 없는 전체 화면을 고를 수 있다. 콘텐츠는 pak으로 묶인다. Steamworks를 켠 빌드는 Steam 초기화·업적·오버레이가 되고, 스크립트 하나로 SteamPipe에 올릴 수 있다.

결정 (2026-10-01):
- 실행 파일: 프로젝트마다 exe를 새로 빌드하지 않는다. 패키징 때 `ProjectERuntime.exe`를 `<ExecutableName>.exe`로 복사하고 아이콘(.ico 또는 .png)·버전 리소스를 `UpdateResource`로 써 넣는다(`ProjectECook --stamp-exe`)
- 패키지 배치(UE식): `<Pkg>/<Exe>.exe` + `Engine/` + `<Exe>/<프로젝트>.eproject, Content/, Cooked/, Config/`. 런타임은 `--project`가 없으면 `<exe 폴더>/<exe 이름>/`의 .eproject를 먼저 찾는다(`Run.bat` 불필요)
- 패키지 판정: `Engine/Packaged.json`(Package.ps1이 기록: 구성/시각/엔진 버전) → `FPaths::IsPackaged()`. 패키지는 Saved를 `%LOCALAPPDATA%/[<Company>/]<프로젝트>/Saved`로 옮긴다(설치 폴더는 쓰기 불가일 수 있음)
- VC++ 런타임: app-local 배포(`Microsoft.VC143.CRT` DLL 복사). 정적 CRT(/MT)는 엔진 DLL·게임 모듈·exe가 STL 객체를 주고받으므로 쓰지 않는다
- 심볼: Release에도 PDB 생성(`/Zi`, `/DEBUG /OPT:REF /OPT:ICF`), 패키지에는 넣지 않고 `Build/Package/<이름>-Symbols/`에 보관
- 크래시: 미니덤프 + 로그 사본을 `Saved/Crashes/<시각>/`에, 패키지 창 앱이면 알림 대화 상자
- 화면: 창 모드 / 테두리 없는 전체 화면(전용 전체 화면은 하지 않음 — flip 모델), Alt+Enter 전환, VSync. 사용자 설정 `Saved/Config/GameUserSettings.json`, 프로젝트 기본값 `Config/DefaultGameUserSettings.json`(선택)
- Steamworks SDK는 파트너 계정 로그인 후에만 받을 수 있어 FetchContent로 받지 않는다 → 3단계 착수 시 사용자와 방식 결정

1단계 — 배포 품질:
- [x] 1-1. 경로: `.eproject` 배포 필드(DisplayName/Version/Company/ExecutableName/Icon, 비면 `Get*` 대체값), `FPaths::IsPackaged` + 사용자 Saved(`MakeUserSavedDirectory`, 폴더 이름 정리), exe 이름 폴더 프로젝트 탐색, 기본 로그 파일(`<Saved>/Logs/<exe 이름>.log`, 이전 로그 `-backup` 1개, 같은 앱이 쓰는 중이면 `-<pid>`) + 로그 첫머리 엔진/프로젝트 요약, 런타임 창 제목 = DisplayName. `PathsTests` 확장
- [x] 1-2. 크래시: `<Saved>/Crashes/<시각>/` Minidump.dmp(스택 간접 메모리 + 스레드 정보) + CrashReport.txt + 로그 사본, Fatal 로그도 사용자 예외로 같은 경로(`FCrashHandler::ReportFatal`), 재진입 방지, 패키지 창 앱이면 알림 대화 상자. 검증 인자 `--crash-test`(30프레임 뒤 액세스 위반). Release PDB(`/Z7` + `/DEBUG /OPT:REF /OPT:ICF`)
- [x] 1-3. `FExecutableResources`(Core/Platform: .ico 파싱, RGBA → 256/64/48/32/16 BMP 아이콘, VS_VERSIONINFO 생성, `UpdateResource` 기록) + `ProjectECook --stamp-exe`, 창 클래스 아이콘 = 리소스 그룹 1. Package.ps1: `<Exe>.exe` + `<Exe>/` 프로젝트 폴더, `Engine/Packaged.json`, Run.bat 제거, VC++ 런타임(vswhere → `Microsoft.VC143.CRT`) 동봉, `<이름>-Symbols/`(PDB + 같은 바이너리), dumpbin 종속 DLL 검사(CRT는 패키지 안에 있어야 통과), ExecutableName = 게임 모듈 이름이면 거부. 샘플 `Icon.png`. `ExecutableResourcesTests` 4개(복사한 exe에 스탬프 → `GetFileVersionInfo`/`LoadImage`로 확인). 확인: Release 패키지 51.8MB, 인자 없이 `Sample.exe` 실행 스크린샷 정상, 로그/덤프 `%LOCALAPPDATA%\ProjectE\Sample\Saved`, 탐색기 버전 정보
- [x] 1-4. `FGameUserSettings`(Core: 창 모드/창 크기/VSync JSON, 프로젝트 기본값 ← 사용자 파일, 자동 검증은 사용자 파일 안 씀) + `FWindow::SetBorderlessFullscreen`(모니터 전체 팝업, 이전 위치·최대화 복원, Alt+Enter 경고음 억제) + `FApplication::OnConfigureWindow`(창 만들기 전 크기). 런타임: 저장된 모드/VSync 적용, Alt+Enter 전환 후 저장, 종료 시 창 크기 기억, `--window-mode`, **패키지에서는 ESC 종료 없음**. Lua `Game` 테이블(`FScriptAppHooks`, `ScriptGameBindings.cpp`): Quit/GetName/GetVersion/Get·SetWindowMode/IsVSync·SetVSync — 에디터는 Quit = 플레이 정지. 테스트 `GameUserSettingsTests` 2개, `GameScript_AppHooks`. Verify `--window-mode BorderlessFullscreen` → 2560x1440 스크린샷, 디버그 레이어 오류 0, 에디터 Verify 정상
- [x] 1-5. 검증 (2026-10-01): Release 패키지 52.2MB — `Sample.exe` 인자 없이 실행(프로젝트 자동 탐색, DXC 없이 쿠킹 셰이더) 스크린샷 정상, 일반 실행 → 창 닫기 시 `%LOCALAPPDATA%\ProjectE\Sample\Saved\Config\GameUserSettings.json` 저장, 로그 `Sample.log` + `Sample-backup.log`, `--crash-test` 덤프가 사용자 폴더에 기록, 탐색기 버전 정보/아이콘, 종속 DLL 검사 통과. 남은 사용자 확인: 크래시 대화 상자(자동 검증에서는 끔), Alt+Enter 실제 전환, VC++ 런타임이 없는 PC에서 실행

2단계 — pak (2026-10-01):
- [x] `FFileSystem`(Core: pak → 디스크 조회, 키 = 마운트 루트 기준 소문자 경로, pak 항목 시각 = pak 파일 시각, 스레드 안전 읽기, 항목별 FNV-1a 해시 검사) + `FPakWriter`(`.epak` v1, 압축 없음 — BC 텍스처가 대부분이라 이득이 작고 의존성 추가 없이). 콘텐츠 읽기 통합: `ReadFileBytes`(쿠킹 모델/텍스처/.enav), 씬/머티리얼/파티클/프리팹/UI/BT/모델 사이드카 JSON, Lua, 글꼴, 이미지, 쿠킹 셰이더(+srchash), 에셋 캐시 신선도 판정, UI 에셋 캐시 시각, 오디오(miniaudio VFS). 디스크 직접 유지: 쓰기, Shaders.json/.eproject/Config(파일로 남김), glTF/FBX 원본 임포트(패키지에는 원본 없음). `FPaths`가 패키지일 때 프로젝트 폴더 `*.epak`을 패키지 루트 기준으로 마운트. `ProjectECook --make-pak`(키 정렬 → 같은 입력이면 같은 pak), Package.ps1 5단계(검사 후 `<Exe>/Content.epak` 생성, 원본 폴더 삭제, `-NoPak`). 테스트 `FileSystemTests` 2개(키 정규화, 마운트/읽기/대소문자/디스크 폴백/손상 감지/잘못된 파일). 확인: pak 93개 파일 32.1MB, 패키지 52.3MB — 기본/`Demo_UI`(글꼴·9-slice·Lua)/`Demo_AI`(.enav·.ebt)/`Demo_Audio`(사운드) 모두 디스크 Content 없이 정상, 경고·오류 0
- 한계: 암호화 없음(내용을 숨기는 목적이 아님 — 필요하면 후속), 압축 없음, 패치용 추가 pak 우선순위 없음(먼저 마운트한 pak이 이긴다)

3단계 — Steamworks (2026-10-01, 사용자 결정: ProjectSC의 SDK 1.53a 경로 사용, App ID = ProjectSC `SteamDevAppId` 2905830):
- [x] Online 모듈(`EOnline`, 엔진 DLL) `FSteamSubsystem`: Init(App ID, 패키지 재실행 `SteamAPI_RestartAppIfNecessary`, 개발은 환경 변수 `SteamAppId`), RunCallbacks/Shutdown, 사용자 이름/SteamID/언어, 업적(1.53 `RequestCurrentStats` → 수신 전 요청은 대기열, `StoreStats`), 오버레이 열기/활성 상태(`GameOverlayActivated_t`). SDK 없이 빌드하면 같은 API의 빈 구현. CMake: `E_STEAMWORKS_SDK_DIR`(루트 `CMakeLocal.cmake` gitignore / 환경 변수), `ThirdParty::steamworks` IMPORTED, `steam_api64.dll` Bin 복사, Package.ps1 동봉. `.eproject "SteamAppId"`, 런타임이 렌더러 전에 Init. Lua `Steam` 테이블(`FScriptSteamHooks`, `FGameWorld::Init` 연결). 테스트 `Steam_DisabledWithoutInit`, Lua Steam 기본값. 확인: Steam 클라이언트가 꺼진 상태에서 경고 후 정상 실행(Verify 오류 0)
- [ ] 실제 Steam 확인 (사용자): Steam 로그인 상태에서 런타임 실행 → 초기화 로그(사용자 이름), Shift+Tab 오버레이, 업적(Steamworks에 등록된 API 이름)

4단계 — SteamPipe (2026-10-01):
- [x] `Scripts/SteamUpload.ps1`: Package.ps1 → `Build/SteamPipe/<프로젝트>/app_build_<AppId>.vdf` + `depot_build_<DepotId>.vdf`(App ID = .eproject SteamAppId, Depot 기본 App ID + 1 = 2905831 — ProjectSC 기존 스크립트와 일치, `*.pdb` 제외) → `steamcmd +login <계정> +run_app_build`(비밀번호는 명령줄에 넣지 않고 steamcmd가 직접 묻는다). 옵션 `-SetLive <베타 브랜치>`(default 거부 — 웹에서 공개), `-Preview`, `-SkipPackage`, `-GenerateOnly`, `-Description`. steamcmd 찾기: `-SteamCmd` → `STEAMCMD` → CMakeLocal SDK `tools/ContentBuilder/builder`. 패키지에 steam_appid.txt가 있으면 거부. 확인: `-GenerateOnly`로 Release 패키지 52.6MB(18개 파일, steam_api64.dll 포함) + vdf 생성, 패키지 실행(Steam 꺼짐 → 경고 후 정상), 에디터 Verify 오류 0
- [ ] 실제 업로드 (사용자): `.\Scripts\SteamUpload.ps1 -Account <빌드 계정> [-SetLive beta]` → Steamworks 웹에서 빌드 확인, 실행 옵션 실행 파일 `Sample.exe`

후속 과제: 크래시 덤프 업로드(현재 로컬 보관만), pak 암호화/압축·패치 pak 우선순위, Steam 클라우드 저장·리치 프레즌스·통계, 스토어용 버전 번호 자동 증가, 설치형 엔진 배포(Phase 7 남은 항목)

## Phase 20 — 프로젝트 설정 / 에디터 환경설정 (2026-10-01, 사용자 요청: "언리얼의 Project Settings, Editor Preferences처럼")

**DoD**: 에디터 메뉴 "편집 → 프로젝트 설정 / 에디터 환경설정"이 언리얼처럼 왼쪽 카테고리 + 오른쪽 속성 창을 연다. 프로젝트 설정(커밋 대상, `Config/<섹션>.json`)에서 에디터 시작 맵·게임 기본 맵·서버 기본 맵·플레이어 프리팹, 프로젝트 정보(표시 이름/버전/회사/아이콘/실행 파일/Steam), 패키징, 물리, 네트워크, 화면 기본값을 고치면 에디터·런타임·서버·패키징이 그 값을 쓴다. 에디터 환경설정(개인, 커밋 안 함)에서 시작 시 마지막 씬 열기, 자동 저장, 뷰포트 카메라/스냅 기본값을 고칠 수 있다. 새 설정 항목은 구조체 필드 + 리플렉션 등록만으로 창·저장에 나타난다.

결정 (2026-10-01 사용자 선택):
- 프로젝트 설정 = `<프로젝트>/Config/<섹션 Id>.json` (언리얼 Config/Default*.ini 역할, 섹션마다 파일 — diff가 작다). `.eproject`에는 Name/EngineVersion/GameModule만 남기고, 기존 필드(DefaultScene, PlayerPrefab, DisplayName, Version, Company, Icon, ExecutableName, SteamAppId)는 로드할 때 설정으로 옮겨 읽는다(Config 파일이 우선). `Config/DefaultGameUserSettings.json`은 `Config/Display.json`이 없을 때 읽는다
- 에디터 환경설정 = 전역 개인 설정 `%LOCALAPPDATA%/ProjectE/EditorPreferences/<섹션 Id>.json` + 프로젝트별 개인 상태 `<Saved>/Config/EditorPerProjectUserSettings.json`(마지막으로 연 씬 — 창에 표시하지 않음)
- 설정 섹션 = 리플렉션 타입 + 객체 + 범위(Project/EditorUser) + 파일. 창은 리플렉션 속성을 인스펙터와 같은 위젯으로 그린다. 값이 바뀌면 바로 저장(언리얼과 같음)
- 리플렉션 확장: enum 프로퍼티(4바이트 enum → Int32 + 선택지 이름), 툴팁

- [x] 20-1. 기반: 리플렉션 enum(4바이트 enum → Int32 + `.Enum`, JSON은 이름)/`.Tooltip`, `FReflectionJson`(값 타입, 없는 키 유지·모르는 키 무시·타입 틀림 경고·범위 자르기), `FSettingsRegistry`/`FSettingsSection`(섹션 전용 FTypeInfo — 전역 FTypeRegistry에 넣지 않음, 범위 Project/EditorUser/ProjectUser, 숨김), `FProjectSettings`(Project/Maps/Packaging/Physics/Network/Display) — `FPaths::SetProject`가 로드: 기본값 → `.eproject` 예전 필드(`FProjectLegacyFields`) → `Config/<Id>.json`, `DefaultGameUserSettings.json`은 Display.json이 없을 때. `.eproject`는 Name/EngineVersion/GameModule만 저장. 테스트 `SettingsTests` 3개 + 기존 테스트 갱신
- [x] 20-2. 소비자 연결: 맵(에디터 `GetEditorStartupMap`, 런타임 `GameDefaultMap`, 서버 `GetServerDefaultMap`, 플레이어 프리팹 — 런타임/서버/PIE), 프로젝트 정보(창 제목, Lua `Game.GetName/GetVersion`, exe 스탬프, Steam App ID, 사용자 Saved 회사 폴더), 물리(`FPhysicsSystem::Begin`이 중력·고정 스텝·서브스텝), 네트워크(`GetConfiguredNetPort/LanDiscoveryPort`, `MaxPlayers` 0 = 설정), 화면 기본값(`FGameUserSettings::Load` 바탕). `Scripts/ProjectSettings.ps1`(`Read-ProjectSettings`) → Package.ps1(구성/pak/원본 포함 기본값 + 추가 폴더)·SteamUpload.ps1(App/Depot ID). Sample: `Config/Project.json`, `Config/Maps.json`, `.eproject` 정리
- [x] 20-3. 에디터 환경설정 `FEditorPreferences`: 일반(시작 시 마지막 씬, 자동 저장 — 저장 안 한 변경이 있을 때 간격마다 `<Saved>/Autosaves/<씬>_<시각>.escene`, 씬마다 N개 보관, 원본은 건드리지 않음), 뷰포트(기본 카메라 속도, 마우스 감도, 시야각, 스냅 켜기/값 — 설정 창 변경 즉시 반영, 툴바에서 바꾼 스냅은 종료 시 저장) + 프로젝트별 상태 `EditorPerProjectUserSettings`(마지막 씬, 열기/다른 이름 저장 시 기록). 자동 검증은 개인 설정을 읽고 쓰지 않음
- [x] 20-4. `FSettingsWindow`(편집 → 프로젝트 설정 / 에디터 환경설정): 왼쪽 검색 + 카테고리별 섹션, 오른쪽 이름/값 표(툴팁, enum 콤보, 에셋 경로 칸은 콘텐츠 브라우저 드롭), 검색 시 모든 섹션 일치 항목, 섹션 기본값으로, 저장 위치 표시, 재시작 필요 표시, 값이 바뀌면 `OnChanged` 즉시 + 조작이 끝나면 저장. 값 위젯을 `FPropertyWidgets`로 분리해 인스펙터와 공유(인스펙터도 enum 콤보·툴팁 지원). 자동 검증 `--open-settings project|editor [--settings-section <Id>]`
- [x] 20-5. 검증 (2026-10-01): Debug/Release 단위 테스트 전부 통과, Verify — 프로젝트 설정(맵 & 모드)/에디터 환경설정(뷰포트) 창 스크린샷, 에디터·런타임 디버그 레이어 오류 0, 런타임/패키지가 `Config/Maps.json`의 게임 기본 맵으로 시작, 전용 서버 서버 기본 맵·LAN 포트, 패키지 exe 스탬프가 `Config/Project.json`을 읽음. 덤으로 실제 Steam 연결 확인(사용자 이름 로그 — 통계는 Steamworks에 등록된 것이 없어 실패 경고 한 번)
- [ ] 실행 검증 (사용자): 설정 창에서 값 바꾸기 → 파일 저장/에디터 반영, 마지막 씬으로 다시 열기, 자동 저장
- 후속: 속성별 기본값 되돌리기 화살표, 배열/구조체 프로퍼티, 입력 매핑(키 바인딩) 섹션, 렌더링 기본값 섹션, 게임 모듈의 자체 설정 섹션 등록 예시

## Phase 21 — 멀티플레이 플레이어 캐릭터 (2026-10-01, 사용자 요청: "Demo_Multiplayer에서 클라이언트마다 WASD 이동, 마우스 시점, 스페이스 점프")

**DoD**: Demo_Multiplayer를 리슨 서버 + 클라이언트 2개로 플레이하면 창마다 자기 캐릭터(캡슐)가 생기고, WASD로 보는 방향 기준 이동, 마우스로 3인칭 시점 회전, 스페이스로 점프한다. 다른 플레이어 캐릭터와 상자가 모든 창에서 같은 위치로 보이고, 캐릭터가 상자를 밀 수 있다.

결정 (2026-10-01):
- 권한: 서버 권위(기존 복제 모델 그대로) — 이동/점프는 서버가 물리로 계산하고 위치는 복제. 시점 방향만 클라이언트가 정해 입력과 함께 보낸다(언리얼 ControlRotation). 클라이언트 예측은 후속(원격 클라이언트는 왕복 지연 + 보간 0.1초만큼 늦게 움직인다)
- 캐릭터 = 동적 강체 캡슐(회전 고정) + 자식 메시(서버가 몸 방향만 돌림, 프리팹 자식으로 복제). 이동은 수평 속도 지정, 점프는 바닥 레이캐스트 후 위쪽 속도
- 카메라: 각 창의 로컬(비복제) 카메라 엔티티를 스크립트가 만든다. `FCameraComponent::Priority`가 높은 주 카메라가 이긴다
- 시점 입력: 마우스 원시 입력(WM_INPUT). 런타임은 클릭하면 커서 잠금(ESC/포커스 잃으면 해제), 에디터 플레이 뷰포트는 우클릭 드래그

- [x] 21-1. 엔진: 원시 마우스(`EWindowEventType::RawMouseMove`, `FInput::GetLookDelta`, Lua `Input.GetLookDelta`), 커서 잠금(`FWindow::SetCursorLocked` — 숨김 + 창 가운데에 가둠, 포커스 잃으면 해제, 런타임 ESC는 잠금 먼저 해제, `FScriptAppHooks::SetMouseLocked/IsMouseLocked` → Lua `Game.*`), 시점 방향(`FGameWorld` 로컬/원격 ControlRotation, 입력 커맨드에 yaw/pitch, `NetProtocolVersion` 6, Lua `Net.SetControlRotation`/`entity:GetControlRotation`), `FCameraComponent::Priority`, `FRigidBodyComponent::bLockRotation`(Jolt AllowedDOFs), `primitive:capsule`(`FPrimitiveShapes::MakeCapsule`), Lua `entity:FindChild`, 정적 메시 경로 변경 재해석(`ResolvedMeshAsset/ResolvedMaterialAsset`, Lua 에셋 경로 쓰기 → 재해석), 런타임 Steam 종료를 렌더러 앞으로 + 자동 검증은 Steam 끔(`--steam`/`--no-steam`). 테스트: 캡슐 와인딩/모양, 카메라 우선순위, 회전 고정, 네트워크 시점 방향
- [x] 21-2. 내용: `Scripts/PlayerCharacter.lua`(Both — 서버: 소유자 입력 + 시점 yaw로 수평 속도, 바닥 레이캐스트 점프, 몸 회전, 플레이어 번호별 색 / 소유 클라이언트: 클릭 커서 잠금·우클릭 시점, `Net.SetControlRotation`, 로컬 3인칭 카메라(Priority 10, 벽 레이캐스트)), `Prefabs/Player.eprefab`(캡슐 콜라이더 35/55 + 동적 강체 80kg 회전 고정 + 복제, 자식 Body(복제, 몸 방향) → Mesh(복제, 캡슐) + Visor), 머티리얼 Green/Purple/Yellow/Visor, Demo_Multiplayer PlayerStart 높이 100. 예전 `PlayerController.lua` 제거
- [x] 21-3. 검증 (2026-10-01): 단위 테스트 전부 통과(`PlayerCharacter_MovesJumpsAndFaces`: 착지 높이 90, W 1초 ≈ 450cm, 시점 90도 → +Y 이동·몸 방향, 점프·재착지, 캡슐 회전 고정), `Verify.ps1 -Multiplayer` — 클라이언트 2개 각자 자기 캐릭터 뒤 카메라, 상대 캐릭터·색·바이저 보임, 오류 0·라이브 객체 0, 에디터 리슨 서버 + 클라이언트 2 — 호스트 캐릭터 카메라. 발견/수정: 프리팹 자식 복제 표시, 소유자 복제 타이밍, 머티리얼 경로 변경 재해석, Steam 오버레이 라이브 객체
- [x] 사용자 피드백 수정 (2026-10-01): ① 에디터가 리슨 서버일 때 클라이언트 조작 안 됨 → 에디터 게임 월드에 네트워크 드라이버가 연결되지 않아 원격 입력 메시지를 버리고 있었음(`FGameWorld::SetNetDriver`, `FPlayInEditorNet` Prepare/Stop — 에디터 클라이언트 모드의 입력 전송·RPC도 같은 원인). ② 자기 캐릭터가 떨림 → 카메라를 `OnUpdate`에서 물리 전 월드 위치로 놓아 한 프레임 늦게 따라감(프레임 시간 12/18ms 교대 시 표시 속도 1.5배/0.67배 교대) → 스크립트 `OnLateUpdate` 단계 추가(물리·트랜스폼 뒤), 카메라를 거기서. 측정: 수정 후 프레임당 이동량/프레임 시간 비율 평균 1.00 표준편차 0.03. 자동 검증 입력 `--hold-keys`, `--play-client-hold-keys`. 테스트 `GameWorld_LateUpdateSeesPhysicsResult`
- [ ] 실행 검증 (사용자): 직접 조작(WASD/마우스/스페이스), 상자 밀기, 서로 보이는지
- 후속: 클라이언트 예측/보정(원격 클라이언트는 왕복 지연 + 보간 0.1초만큼 늦게 움직임), 경사·계단(Jolt CharacterVirtual), 공중 제어 감소, 애니메이션 캐릭터 모델

## Phase 22 — 캐릭터 이동 컴포넌트 + 클라이언트 예측 (2026-10-01, 사용자 요청: "원격 클라이언트 조작이 한발 느림 → 위치 예측", 엔진 C++ 컴포넌트로)

**DoD**: `FCharacterMovementComponent`(엔진 C++, 언리얼 CharacterMovement 역할)를 단 엔티티는 Jolt CharacterVirtual로 걷기·점프·중력·경사 제한·계단 오르기·벽 미끄러짐을 하고, 부딪힌 동적 물체를 민다. 스크립트/게임 모듈은 이동 방향과 점프만 넘긴다. 멀티플레이에서 소유 클라이언트는 입력 즉시 자기 캐릭터를 움직이고(예측), 서버가 같은 입력으로 계산한 결과와 다르면 보정한다. 원격 클라이언트에서도 에디터 호스트처럼 즉시 반응한다.

결정 (2026-10-01):
- 시뮬레이션: Jolt CharacterVirtual(쿼리 기반, 되감아 다시 돌릴 수 있음) + 내부 키네마틱 바디(다른 캐릭터/물체가 부딪히게). 동적 바디 밀기 = CharacterVirtual MaxStrength
- 이동 = "무브" 단위 (순번, dt, 월드 XY 입력, yaw, 점프). 같은 무브 → 같은 결과를 내는 순수 경로 `FPhysicsSystem::SimulateCharacter`를 서버·클라이언트·Standalone이 공유
- 네트워크: 소유 클라이언트가 무브를 만들어 즉시 적용하고 기록 + 서버로 전송(비신뢰, 최근 무브 여러 개를 겹쳐 보내 손실 대비). 서버는 소유자 확인 후 순서대로 적용, 처리한 순번과 상태를 소유자에게 ack. 클라이언트는 ack 상태로 되돌린 뒤 남은 무브를 다시 적용(재조정). 관찰자 클라이언트는 기존 스냅샷 보간
- 제어 주체: 소유 플레이어(클라이언트/호스트), 서버 소유(owner < 0, AI 등)는 서버
- 갱신 순서: 스크립트(입력) → 캐릭터 이동 → 게임 모듈 → AI → 물리 → 트랜스폼 → LateUpdate

- [x] 22-1. 물리: `FCharacterMovementComponent`(Physics, 리플렉션 "캐릭터 이동"), `CharacterMovementMath::ComputeVelocity`(순수: 바닥/점프/공중 제어/중력, dt 상한 0.1초), `FPhysicsWorld` 캐릭터(CharacterVirtual Z-up 캡슐 + 내부 키네마틱 바디, 발밑 판정 평면 = 아래 반구 중심, MaxStrength로 동적 물체 밀기, ExtendedUpdate 계단/바닥 붙기), `FPhysicsSystem::SyncCharacters/AddMovementInput/RequestJump/ConsumePendingMove/SimulateCharacter/Get·SetCharacterState/FollowTransform/IsGrounded`(콜라이더가 있어도 캐릭터 우선, 스크립트 순간이동 감지), `FGameWorld::TickCharacters`(스크립트 직후: 조종하는 쪽은 무브 시뮬레이션, 클라이언트의 다른 플레이어는 복제 위치 따라가기), Lua `entity:AddMovementInput/Jump/IsGrounded`. 테스트 `CharacterMovementTests` 7개(속도 규칙, 착지·걷기·몸 방향, 벽 미끄러짐, 계단 30cm 오름/60cm 막힘, 경사 30도 오름/70도 못 오름, 점프 높이 ≈138cm·착지, 상자 밀기·캐릭터끼리 통과 안 함)
- [x] 22-2. 네트워크 예측 (`World/GameWorldCharacter.cpp` 머리 주석이 규칙): 소유 클라이언트는 무브에 순번을 붙여 바로 시뮬레이션·기록하고 최근 8개를 겹쳐 전송(`CharacterMoves`, 비신뢰), 서버는 소유자 확인 후 순번 순서로 적용(dt 0.1초 상한, 입력 길이 제한, 겹친 무브 거르기, 무브가 없으면 그 자리)·소유자에게 `CharacterAck`(순번 + 위치/속도/바닥), 클라이언트는 ack 상태로 되돌리고 남은 무브를 다시 적용(1cm 넘게 바뀌면 보정 횟수). `FReplicationClient::SetTransformFilter` — 런타임/에디터 클라이언트가 예측 캐릭터에 스냅샷 보간을 쓰지 않음, `FGameWorld::IsPredicted`. `NetProtocolVersion` 7. 테스트 `CharacterPrediction_ImmediateMatchAndCorrection`(루프백: 틱 한 번에 7.5cm 즉시 이동·서버는 그대로, 1초 이동 후 서버와 1cm 이내·보정 0회, 서버 순간이동 → 클라이언트가 300cm 보정)
- [x] 22-3. 내용·검증: `Player.eprefab` 루트를 캡슐 콜라이더+강체 → `CharacterMovementComponent`로, `PlayerCharacter.lua`는 조종하는 쪽에서 입력→`AddMovementInput`/`Jump` + 시점/카메라만(서버 이동 코드 제거). 측정(런타임 리슨 서버 + 클라이언트, `--hold-keys W --hold-keys-delay 5`): 키를 누른 다음 프레임(≈20ms)에 이동 시작 — 예측 전 ≈130ms. 프레임당 이동량이 프레임 시간과 일치. 서버가 시뮬레이션하는 공/상자와 부딪힐 때 생기는 보정은 화면 오프셋으로 0.1초에 걸쳐 흡수(프레임당 최대 튐 29cm → 13cm), 150cm 넘으면 순간이동. 테스트 `PlayerCharacter_MovesJumpsAndFaces` 갱신, 전체 통과. Verify: 전용 서버 + 클라이언트 2, 에디터 리슨 + 클라이언트 2(클라이언트 이동·상자 밀기) 오류 0
- [x] 예측 옵션 (2026-10-01 사용자 요청): 캐릭터 이동 컴포넌트 "클라이언트 예측"(`bClientPrediction`) + 프로젝트 설정 네트워크 "클라이언트 예측" — 둘 다 켜져 있어야 예측(`FGameWorld::UsesClientPrediction`). 끄면 소유 클라이언트는 무브를 보내기만 하고 자기 캐릭터도 스냅샷 보간(재조정 없음). 테스트에 끔 경우 추가
- [ ] 실행 검증 (사용자): 원격 클라이언트 반응 속도, 상자 밀기/캐릭터끼리 부딪힘, 계단·경사, 예측 끄고 비교
- 후속: 서버 측 속도 조작 방지(무브 dt 합 vs 실제 시간), 움직이는 발판 위 캐릭터(ground velocity), 웅크리기/수영 등 이동 모드, 캐릭터 애니메이션 연동

## Phase 23~26 병렬 진행 (2026-10-01, 사용자 결정: "추천 순서대로")

트랙(각자 git worktree 브랜치, 메인이 머지): A = Phase 23 조명 → 이어서 Phase 26 성능, B = Phase 24 입력, C = Phase 25 게임플레이. 애니메이션 캐릭터(Phase 27)는 B 머지 후 시작.
공통 규칙: `Plans.md`/`CLAUDE.md`는 메인만 고친다(트랙은 커밋 메시지에 규칙 기록). 등록부(`SceneReflection.cpp`, `ProjectSettings.cpp`, Lua 바인딩, `Tests/CMakeLists.txt`)는 끝에 추가만. `NetProtocolVersion`/`GameModuleApiVersion`은 필요하면 올리고 머지 때 메인이 최종값을 정한다. `Scripts/PlayerCharacter.lua`·`Prefabs/Player.eprefab`·`Demo_Multiplayer.escene`은 트랙 B만 고친다.

## Phase 23 — 점광원·스포트라이트 + 그림자 (트랙 A)

**DoD**: `FPointLightComponent`/`FSpotLightComponent`(색, 세기, 반경, 스포트 내/외 원뿔각, 그림자 여부)를 씬에 놓으면 PBR로 조명되고(정적·스킨 메시), 수십~수백 개여도 클러스터드 라이트 컬링으로 버틴다. 그림자를 켠 스포트는 그림자 맵, 포인트는 큐브 그림자를 그린다. 예제 씬 `Demo_Lights.escene`(밤/실내, 횃불형 포인트 다수 + 그림자 스포트) Verify 스크린샷·디버그 레이어 오류 0.

- [x] 23-1. 컴포넌트(점광원/스포트 + 하늘광 `FSkyLightComponent` — 밤 씬용 환경광 배율) + `LightMath.h`/`Lighting.hlsli` 식 + `LightTests` 9개, 에디터 아이콘·반경/원뿔 표시·통계
- [x] 23-2. 클러스터드 라이팅(`FLocalLightRenderer`, 최대 1024 라이트, `ClusterCulling.hlsl` 16x9x24, 클러스터당 63개), 정적·스킨·픽셀 아트 경로
- [x] 23-3. 그림자 타일 배열(스포트 1장/점광원 큐브 6장, 3x3 PCF, 스킨 캐스터), `Demo_Lights`(포인트 80 + 그림자 스포트 2·점광원 1), Verify 오류 0 (2026-10-01 master 머지)

## Phase 24 — 입력 매핑 + 게임패드 (트랙 B, 언리얼 Enhanced Input 방식)

**DoD**: 프로젝트 설정 "입력"에 액션(버튼/1D/2D 축)과 바인딩(키, 마우스, 게임패드 버튼·스틱·트리거, 부정/축 바꿈/데드존, WASD 합성)이 있고, Lua·C++는 `Move`/`Jump`/`Look` 같은 액션을 읽는다. 플레이어 재지정은 `<Saved>/Config/` 사용자 파일. XInput 게임패드가 동작한다. 멀티플레이에서 서버 스크립트가 소유 플레이어의 액션 값을 읽는다. 샘플 `PlayerCharacter.lua`가 액션으로 동작(키보드/게임패드 모두).

- [x] 24-1. `Core/InputActions` 순수 로직(Button/1D/2D, Negate/Swizzle/DeadZone/Scale/ScaleByDeltaTime) + XInput 동적 로드(`Core/GamepadInput`)
- [x] 24-2. 설정 "Input"(`RegisterCustom`, `Config/Input.json`, 설정 창 전용 UI), `<Saved>/Config/InputBindings.json` 재지정, Lua/C++/게임 모듈 API, 입력 커맨드에 액션 값(`NetProtocolVersion` 8)
- [x] 24-3. `PlayerCharacter.lua` Move/Look/Jump(키보드 + 게임패드), `--hold-gamepad`, Verify 멀티플레이 오류 0 (2026-10-01 master 머지)
- [ ] 실행 검증 (사용자): 실물 XInput 게임패드(연결/해제, 데드존, 오른쪽 스틱 시점 속도, A 점프), 설정 창 바인딩 편집, `Input.Rebind` 유지

## Phase 25 — 게임플레이 기본 틀 (트랙 C)

**DoD**: `FHealthComponent`(서버 권위, 복제) + 데미지 API(Lua/C++, 서버만) → 사망/리스폰 이벤트, 게임 모드(규칙·점수·승패, 복제되는 게임 상태), 세이브 게임 API(`<Saved>/SaveGames/`, Lua/C++). 예제 씬 `Demo_Gameplay.escene`, 단위 테스트.

- [x] 25-1. `FHealthComponent` + 데미지/회복(서버만) + 이벤트(Lua/게임 모듈 `GameModuleApiVersion` 5) + 리스폰(폰 유지, PlayerStart)
- [x] 25-2. `FGameModeComponent`(규칙 저장 + 상태 Transient 복제, 메시지 변경 없음), Lua `GameMode.*`
- [x] 25-3. `FSaveGame` + Lua `SaveGame.*`, `Demo_Gameplay`(포탑·표적·HUD·최고 기록), Verify 오류 0 (2026-10-01 master 머지)
- [x] 통합: `Player.eprefab`에 `HealthComponent`, 사망 중 입력 막기, Demo_Multiplayer 게임 모드 (Phase 27-3)

## Phase 26 — 성능: 인스턴싱 / LOD / 오클루전 (트랙 A, Phase 23 뒤)

**DoD**: Demo_Stress 기준 측정치(프레임 시간 CPU/GPU, 드로우 콜 수)를 기록하고, 같은 메시·머티리얼은 인스턴싱으로 묶고(정적 메시 + 섀도우/로컬 그림자/아웃라인 패스), LOD(모델 임포트 생성 또는 수동 + 화면 크기 전환), 오클루전 컬링(HZB)로 개선 후 다시 측정

- [x] 26-1. 측정 수단(`FD3D12GpuTimer`, `--perf-capture`, 구간별 CPU/GPU, 통계 창) + `Demo_StressStatic`(정적 2400개 — Demo_Stress는 전부 스킨이라 인스턴싱 대상 아님)
- [x] 26-2. GPU 인스턴싱(메인/CSM/로컬 그림자/아웃라인): StressStatic Debug 57.3 → 12.1ms, Release CPU 1.73 → 0.89ms, 드로우 2313 → 10
- [x] 26-3. LOD(직접 구현 QEM, 임포트 설정, `ModelVersion` 7): StressStatic Release GPU 1.71 → 0.81ms, 삼각형 189만 → 31만
- [x] 26-4. HZB 2단계 오클루전: 68% 가림이지만 LOD와 함께면 GPU 0.76 → 0.85ms로 손해라 기본 끔 (2026-10-01 master 머지, 회귀 Verify 오류 0)
- 후속: 스킨 메시 비용(Demo_Stress — 팔레트 수집 CPU 0.37ms, CSM 스킨 그림자 821드로우), 동적 업로드 버퍼 용량(4MB 중 ≈3.7MB 사용), LOD 히스테리시스/크로스페이드, 헬멧 LOD 25%에서 멈춤

## Phase 27 — 애니메이션 캐릭터 (트랙 D, Phase 24 뒤)

**DoD**: 플레이어 캐릭터가 캡슐 대신 스켈레탈 모델을 쓰고, 이동 속도에 따라 대기·걷기·뛰기가 블렌드되며 점프/낙하 상태가 있는 간단한 상태 머신으로 재생된다. 멀티플레이에서 각 클라이언트가 복제된 속도/바닥 상태로 같은 애니메이션을 낸다. 플레이어에 체력(Phase 25) 통합.

- [x] 27-1. `.eanimgraph` + `FAnimGraphComponent`, 1D 블렌드 스페이스·크로스페이드 상태 머신(순수 `FAnimGraphInstance`), Lua `SetAnimParam/GetAnimParam/GetAnimState`, `AnimGraphTests` 7개
- [x] 27-2. `bUseCharacterMovement` → Speed/VerticalSpeed/Grounded 자동 공급(원격은 보간 위치 변화), 애니 상태 비복제
- [x] 27-3. `Player.eprefab` Fox + `FoxCharacter.eanimgraph`, 플레이어 색 = 발밑 Marker, `HealthComponent` + 사망 중 입력 차단·숨김, Demo_Multiplayer 게임 모드(리스폰 3초), Demo_Animation `Fox_Graph`. Verify 멀티플레이(불량 망 포함) 오류 0 (2026-10-01 master 머지)
- [ ] 사용자: 점프/낙하 애니메이션 에셋(현재 Jump = Run 0.35배속, Fall = Walk 0.3배속 대체), 걷기/뛰기 발 미끄러짐(블렌드 위치 Walk 120 / Run 450)
- 후속: `.eanimgraph` 편집기·핫 리로드, 리스폰 순간 원격 보간 이동, 전용 서버 노티파이

## Phase 28 — 물리 물체 로컬 예측 (2026-10-01, 사용자 요청: "원격 클라이언트에서 물리 물체와 상호작용하면 드득거리고 반응이 늦음", 결정: 로컬 예측 = 언리얼 Predictive Interpolation식)

원인: 원격 클라이언트의 예측 캐릭터는 현재 시간, 복제 동적 물체는 키네마틱(`SetKinematicOverride`)으로 스냅샷 보간(편도 지연 + 0.1초 과거)을 따른다 → 클라이언트에선 과거 위치의 밀리지 않는 상자에 막히고 서버에선 밀려서 ack마다 보정이 반복되고, 상자 반응은 왕복 + 0.1초 늦게 보인다.

**DoD**: 원격 클라이언트(`--net-lag 100` 포함)에서 상자/공을 밀 때 캐릭터 보정이 거의 없고(측정: 보정 횟수·프레임당 최대 튐) 상자가 즉시 반응한다. 서버 권위 유지(최종 상태는 서버), 다른 클라이언트 화면은 기존처럼 보간. 프로젝트 설정으로 끌 수 있다.

- [x] 28-1. 진단 확인(끔: 과거 위치 키네마틱 상자에 막혀 ack마다 보정, 화면 튐 최대 1.3m, 불량 망 상자 반응 0.25초) + 대상 선정·동적 전환(`World/GameWorldPhysicsPrediction.cpp`), 측정 인자 `--net-physics-stats`, Demo_Multiplayer 끝에 PushCrate/PushBall
- [x] 28-2. ack↔스냅샷 시각 맞추기 + 기록 비교 수렴(τ 0.15초, 100cm 스냅) + 해제 블렌드 0.25초 + 설정 `PhysicsPrediction`/`PhysicsPredictionRadius`. 서버·메시지 변경 없음
- [x] 28-3. 재조정 다시 적용 개선(밀기 끔, 기록 위치 두 방식 비교), 테스트 2개. 측정 중앙값(끔 → 켬): 정상 망 보정 121 → 16회·캐릭터 최대 튐 118 → 8.8cm, 불량 망 보정 84 → 25회·상자 반응 0.25초 → 1프레임 이내 (2026-10-01 master 머지, Verify 오류 0)
- [ ] 사용자: 실제 원격 환경 손맛, PushCrate/PushBall 유지 여부, 가벼운 공 올라탈 때 보정(20~30cm)
- 후속(롤백 없음의 한계): 서버의 무브 몰아 적용(1~5cm 보정), 가벼운 공 올라타기 판정 차이, 과거 보간 키네마틱(원격 캐릭터·서버 발사체)과 부딪힐 때 드문 스냅

## Phase 29 — 성능 2차: 스킨 메시 / 컬링 보강 / LOD 전환 (2026-10-01, 사용자 요청: "다음 할 일 진행 + 프러스텀 컬링 빠진 곳도")

현황(메인 확인): 프러스텀 컬링은 메인 패스·CSM 캐스케이드·로컬 라이트·로컬 그림자 장에 적용됨. 빠진 곳 = 파티클 렌더(이미터 경계 없음), 스킨 팔레트(`FSkinnedMeshPalette::Build`가 화면/그림자에 안 보이는 스킨 메시까지 매 프레임 계산·업로드).

**DoD**: Demo_Stress(스킨 228) Release/Debug CPU·GPU 개선을 `--perf-capture`로 측정·기록. 화면 밖 캐릭터를 비추는 그림자는 그대로 나온다. 동적 업로드 버퍼가 차도 깨지지 않는다.

- [x] 29-1. 스킨 팔레트 가시성 컬링(메인 ∪ CSM 캐스케이드 ∪ 로컬 그림자 장, 그림자 렌더러 Prepare 분리 — 먼 군중에서 업로드 372 → 4KB, 전부 보일 땐 CPU +0.05ms) + 동적 업로드 버퍼 넘침 Fatal → 페이지 덧붙이기/합치기(상한 256MB)
- [x] 29-2. 스킨 인스턴싱(t15 `SkinBones` + `BoneOffset`, 메인/CSM/로컬 그림자/아웃라인): Demo_Stress 드로우 145/822 → 2/8, Release 프레임 3.53 → 2.23ms, Debug 29.4 → 23.1ms
- [x] 29-3. 파티클 이미터 경계(`ParticleBounds`, `bFixedBounds`) + 컬링, 화면 밖 GPU 계산은 미뤘다 다시 보일 때 따라잡기 (시험 씬 GPU 1.68 → 1.03ms)
- [x] 29-4. LOD 히스테리시스(±10%). 화면 밖 애니메이션 갱신 생략은 Demo_Stress에서 이득 0이라 안 함 (2026-10-01 master 머지, Verify 오류 0)
- 후속: 스킨 가시성 판정 비용(조인트 행렬 캐시), 화면 밖 애니메이션 갱신 생략(렌더러 → Scene 가시성 경로, 소켓 대상 제외), GPU 이미터 추정 경계가 보수적, 오래 숨은 이미터 재등장 시 디스패치 몰림

## Phase 30~35 병렬 진행 (2026-10-01, 사용자 결정: 1·2·3순위 기능, 1단계 동시 4트랙)

결정 (2026-10-01 사용자 선택): 동시 트랙 4개 먼저(A·B·C·D) → 끝나는 순서로 E·F 투입(C 자리에 E, 다음 빈자리에 F). 안티에일리어싱 = TAA. 머티리얼 노드 편집기는 이번 범위에서 제외. 래그돌 = 사망 시 전신 래그돌까지(애니메이션·물리 혼합 제외).
공통 규칙: Phase 23~26과 같음. `Plans.md`/`CLAUDE.md`는 메인만 고친다(트랙은 커밋 메시지에 규칙 기록). 등록부(`SceneReflection.cpp`, `ProjectSettings.cpp`, Lua 바인딩, `Tests/CMakeLists.txt`, `Shaders.json`)는 끝에 추가만. `NetProtocolVersion`/`GameModuleApiVersion`은 필요하면 올리고 머지 때 메인이 최종값을 정한다.
현황(메인 확인): 렌더러는 깊이/법선 사전 패스 없이 HDR 씬 패스에 바로 그린다(`FSceneRenderer::RenderSceneColor`) → TAA/SSAO/데칼/안개/반사는 공통 기반이 필요해 한 트랙(D)에서 순서대로. 물리 충돌 이벤트 없음, Lua `Scene`은 Create/Destroy/Find/SpawnPrefab뿐(맵 전환 없음).

## Phase 30 — 충돌·영역 알림 / 관절 / 사망 래그돌 (트랙 A)

- [x] 30-1. 충돌·트리거 알림: 콜라이더 `bIsTrigger`, 강체 `bReportContacts`. Jolt 접촉 리스너는 보고 대상(트리거/ReportContacts/스크립트 엔티티/게임 모듈 `WantsCollisionEvents`) 바디가 낀 접촉만 잠금 아래 모으고, 스텝 뒤 메인 스레드에서 바디 쌍 단위로 시작 1회/끝 1회 정리(잠드는 "접촉 제거"는 끝 아님 — 깨어난 뒤 온전한 스텝 하나 동안 `WereBodiesInContact` 거짓이면 끝, 바디 삭제는 즉시 끝). 트리거 = 잠들지 않는 키네마틱 센서 + `CollideKinematicVsNonDynamic`, 전용 `Trigger` 레이어, 레이캐스트 무시. 전달은 `FGameWorld`(`World/GameWorldPhysicsEvents.cpp`)가 물리 → UpdateTransforms → 애니메이션 파라미터 뒤, `OnLateUpdate` 앞. Lua `OnCollisionBegin(other, info{Point,Normal,Impulse,Speed})`/`OnCollisionEnd`/`OnTriggerEnter`/`OnTriggerExit`(상대 파괴 시 nil), 게임 모듈 같은 이름. 클라이언트는 복제 엔티티 이벤트를 만들지 않음. 테스트 `PhysicsEvents_*` 4개, `CollisionScript_*` 2개
- [x] 30-2. 관절: `Fixed/Hinge(제한·모터·마찰)/Distance(최소·최대·스프링)/BallJointComponent(원뿔)` + 공통 Target(비면 월드)/Anchor/BreakForce(N)/CollideConnected. `FPhysicsWorld`가 Jolt 관절 소유(바디보다 먼저 제거), 바디 쌍 충돌 끄기 = 충돌 그룹(ID = 바디 ID) + 참조 계수 GroupFilter. `FPhysicsSystem::SyncJoints`(바디 동기화 뒤, 설정/바디 변경 시 현재 자세로 재생성), 구속 힘 > BreakForce → 제거 + `OnJointBreak(other, force)`. 뷰포트 관절 표시. `Demo_Joints`(경첩 문, 사슬, 스프링, 모터 풍차, 끊어지는 판 — 충격 4804 → 5134 N에서 끊어짐, 트리거 램프). 테스트 `PhysicsJoints_*` 7개
- [x] 30-3. 사망 래그돌: `FRagdollComponent`(`Physics/Ragdoll.h`), 순수 `RagdollMath::BuildLayout`(뼈 → 캡슐 + SwingTwist 관절, 짧은 뼈 합치기, 이웃/겹침/주인 바디 충돌 끄기 — CharacterVirtual 질의도 따름). `FAnimationRuntime::bPhysicsPose`면 애니메이션 갱신 생략 → 물리가 뼈 로컬 트랜스폼을 부모 먼저 씀, 끄면 원래 로컬로 복원. 사망 연동 `World/GameWorldRagdoll.cpp`(게임플레이 규칙 뒤·물리 앞, 자신/조상 `FHealthComponent` 살아 있음↔죽음 전환 때만). 래그돌은 비복제 로컬 연출(각자 복제된 체력으로 판단). Lua `entity:EnableRagdoll/DisableRagdoll/IsRagdollActive`. `Demo_Ragdoll`(Fox 뼈 24 → 캡슐 23·관절 22). 테스트 `RagdollMath_*` 2개, `Ragdoll_EnableFallsAndDisableRestores`, `RagdollScript_*` 2개. `GameModuleApiVersion` 8(머지 시 B의 7 위로) (2026-10-01 master 머지, 트랙 Debug/Release 경고 0·테스트 100%·화면 확인 오류 0)
- [ ] 실행 검증 (사용자): Demo_Joints 문/사슬/스프링 손맛, Fox 래그돌 캡슐 굵기(`RadiusScale` 0.25)·쓰러지는 모양, `Player.eprefab`에 `RagdollComponent`를 달지(지금은 사망 시 모델 숨김)
- 후속: 바디 재생성 시 쌍이 끝 → 다시 시작으로 보임, 캐릭터 ↔ 정적 벽 접촉 미보고, 예측 접촉 때문에 `CollisionBegin`이 한 스텝 먼저 올 수 있음(충격은 솔버 전 추정), 래그돌 켠 채 EndPlay 시 `bPhysicsPose` 남음(플레이 씬은 버려서 현재 안전), 래그돌 캡슐은 접촉 이벤트 없음, 전용 서버는 모델 데이터 없어 래그돌 안 만듦, 부분 래그돌/일어나기/`.emeta` 뼈별 설정 UI

## Phase 31 — 게임 중 맵 바꾸기 / 큰 맵 나눠 불러오기 (트랙 B)

- [x] 31-1. 게임 중 맵 전환: Lua `Game.OpenScene/GetCurrentScene`, C++ `IGameNet::OpenScene`(서버·Standalone만, 클라이언트는 경고 + false). 처리는 `World/GameWorldTravel.h` `FGameWorldTravel::ConsumePending/Travel` 하나(런타임/서버/에디터 플레이/테스트 공용): 프레임 끝 → EndPlay → 정리 → 씬 비우기 → 로드 → 에셋 해석 → 정적 NetId → BeginPlay, 씬 객체는 유지하고 내용만 교체. 멀티플레이 = ServerTravel식(연결 유지, Travel/TravelAck, 이동 중 플레이어는 `FRemotePlayer::bInScene`으로 송수신 제외, Ack 후 `OnPlayerJoined` 재호출 → 폰 재생성·전체 상태). 씬 간 값 `Game.SetPersistent/GetPersistent/ClearPersistent`(프로세스 메모리만, 에디터는 정지 시 비움). 런타임 로딩 = 검은 화면 1프레임. `FPlayMode::Travel`(정지하면 편집 씬 복원). 샘플 `Demo_Travel_A/B`, `TravelPortal.lua`. 테스트 `SceneTravel_*` 4개
- [x] 31-2. 서브 씬 스트리밍: `SubSceneVolumeComponent`(경로, HalfExtents, UnloadMargin) + `StreamingSourceComponent`(기준 = 주 카메라/캐릭터 이동/이 컴포넌트), Lua `Scene.LoadSubScene/UnloadSubScene/IsSubSceneLoaded/GetSubSceneRoot` + `OnSubSceneLoaded(path)`, `IGameNet` 3개. 파일 읽기·JSON 파싱만 `std::async`(`FSceneSerializer::ParseFile`), 엔티티 생성·프리팹 동기화·GPU는 다음 게임플레이 틱 메인 스레드(`AppendDocument`). 루트 엔티티 하나 아래(`FTransientComponent` — 메인 씬 저장 제외), 내릴 때 루트째 지연 파괴. 멀티플레이 SubSceneLoad/Unload, NetId = `1<<26 + (번호-1)*16384 + 하위 트리 순서`, 늦은 입장자에게 목록 먼저. 측정: 엔티티 1000개 Release 파싱 4.0ms + 붙이기 2.1ms, Demo_Streaming 붙이기 2.0~2.6ms. 데모 `Demo_Streaming` + `Streaming/Area1~3`. 테스트 `SubScene_*` 3개. `NetProtocolVersion` 10, `GameModuleApiVersion` 7 (2026-10-01 master 머지, 트랙 Debug/Release 경고 0·테스트 100%·화면 확인 오류 0)
- [ ] 실행 검증 (사용자): **pak 패키지에서 포털·스트리밍**(`.ps1` 막혀 미확인), 직접 조작으로 포털 진입, 인스펙터에서 서브 씬 볼륨 추가·편집, 실제 PC 2대로 전환 중 입장/퇴장
- 후속: `FResourceManager` 메시/텍스처/머티리얼 캐시 해제 없음(맵마다 에셋이 다르면 메모리 누적), 이동 전 비신뢰 스냅샷이 이동 뒤 도착하는 경우(시퀀스 번호 없음), 클라이언트 서브 씬은 동기 처리, 서브 씬 붙일 때 씬 전체 에셋 해석 재실행, 파싱 중 내리면 대기, 같은 서브 씬 볼륨 2개/중첩 서브 씬 미정, 에디터 볼륨 표시·미리보기 없음, 로딩 화면 글자 없음

## Phase 32 — 다국어 / 입력창 마무리 (트랙 C)

- [x] 32-1. 다국어: `.estrings`(한 파일에 모든 언어 `{Version, Languages, Strings:{키:{언어:값}}}` — 편집 창이 파일 단위이고 pak은 폴더 나열이 안 돼서), `FLocalization`(UI 모듈, 엔진 DLL 하나 — 게임 모듈 C++ API, `GameModuleApiVersion` 변경 없음), 형식 인자 `{0}`/`{이름}`/`{{`, 찾기 현재 언어 → 기본 언어 → 키 그대로(키마다 경고 1회), 언어 결정 `--language` > `<Saved>/Config/Language.json` > 시스템 언어(설정) > 기본 언어. 프로젝트 설정 "Localization"(DefaultLanguage/StringTables/DetectSystemLanguage — 비면 `Content/Localization/*.estrings`, pak은 설정 필수). 위젯 `TextKey`/`HintTextKey`(그릴 때마다 조회 → 즉시 반영), Lua `Loc.*` + `widget.TextKey`, 문자열 표 편집 창 `FStringTableEditor`, 디자이너 키 선택·미리보기 언어. 샘플 `Strings.estrings`(20키), HUD L 키 한/영 전환. 테스트 UITests 5개 + `UIScript_LocalizationKeysAndLocTable`
- [x] 32-2. `FUITextEdit` 순수 편집 로직(Shift 선택, Ctrl 단어 이동/지우기, Ctrl+A, 복사/잘라내기/붙여넣기 — 줄바꿈 → 공백, 최대 길이), 마우스 클릭/Shift+클릭/끌기 선택, Win32 클립보드(`UI/UIPlatformWindows.cpp`), 선택 영역 `SelectionColor`, IME 조합 밑줄(`FWindow::SetTextInput` — 켜져 있을 때 IME 메시지를 직접 처리, 후보 창은 캐럿 아래, 끄면 조합 취소; `WindowEvent::ImeComposition`). 에디터는 플레이 + 뷰포트 포커스 + 텍스트 상자 포커스일 때만 켬. 자동 검증 `--ui-text-demo select|compose`. 테스트 UITests 6개 (2026-10-01 master 머지, 트랙 Debug/Release 경고 0·테스트 100%·화면 확인 오류 0)
- [ ] 실행 검증 (사용자): 실제 한글 IME 입력(조합 밑줄, Enter 확정, 한자 후보 창 위치, 포커스 이동 시 취소, 창 재활성화 시 시스템 조합 창 깜빡임 — `WM_IME_SETCONTEXT` 미처리), Ctrl+C/X/V 실제 클립보드, Shift+클릭/끌기, L 키 언어 전환 후 다음 실행 유지, 디자이너 미리보기 언어·표 저장 즉시 반영
- 후속: 더블클릭 단어 선택, `.estrings` 이동 시 `Config/Localization.json` 경로 미갱신, 설정 창 Localization 변경은 다음 실행부터, 한글 입력 모드에서 IME가 WASD를 가로챔(포커스 없을 때 `ImmAssociateContextEx`로 IME 끄기), ImGui 창을 메인 창 밖으로 떼면 캐럿 위치 어긋남, 여러 줄 텍스트 상자

## Phase 33 — 화면 품질 (트랙 D)

- [x] 33-1. 사전 패스(깊이 + 법선 팔면체/B=거칠기 + 움직임 벡터, 정적·스킨·인스턴싱), 메인은 같은 VS로 깊이 EQUAL. 이전 World는 엔티티별 기록, 스킨 팔레트 [현재][이전] 연결, Halton(2,3) 8 지터, 이전 ViewProj 상수. HZB는 사전 패스 깊이로. Release StressStatic 메인 GPU 0.085 → 0.064ms(사전 패스 합계 +0.02~0.04ms), 업로드 376 → 734KB
- [x] 33-2. TAA(YCoCg 이웃 클립, Catmull-Rom 이력, 가장 가까운 깊이 움직임 벡터, 씬 컬러 알파 = 반응형 마스크, 톤매핑 패스 샤프닝) GPU ≈0.025ms. 픽셀 아트/와이어프레임/여러 뷰/썸네일에서 자동 끔
- [x] 33-3. SSAO(GTAO, 반해상도 + 양방향 블러, 간접광에만) 0.03~0.046ms
- [x] 33-4. 데칼(사전 패스 기반 DBuffer — 클러스터드는 바인드리스 필요), 0.005ms, 에디터 상자, `Demo_Decals`
- [x] 33-5. 해석식 높이 안개 + 볼류메트릭(160×90×64, CSM 그림자, 로컬 라이트, 시간 누적), 파티클은 정점 안개. 0.039 + 0.009ms, `Demo_Fog`
- [x] 33-6. SSR(Hi-Z 레이마칭, 거칠기 페이드, TAA 시 GGX 흔들기 누적) 0.07~0.15ms + 반사 캡처(`도구 → 반사 캡처 굽기`/`--bake-captures`, IBL 프리필터 재사용, `.ecapture`, 큐브 배열 최대 8, 상자 시차 보정), 우선순위 SSR → 캡처 → 하늘. `Demo_Reflections`
- [x] 33-7. `.hdr` → `FAssetCache` `.eenv`(EnvironmentVersion 1) → 하늘 큐브 512 + 조도/프리필터, 하늘광 환경맵 경로·회전, 쿠킹 도구 지원. `Demo_HdrSky`
- [x] 지형·식생 통합(D 브랜치에서 master 머지 후): `TerrainPrepassPS` + 세 PSO 같은 TerrainVS, 지형 픽셀 셰이더가 데칼/SSAO/`EvaluateImageBasedLighting`(캡처/SSR/하늘) 적용(알파 0), 지형 루트 시그니처 공간 0 t16~t22, 식생 PrevWorld = World. Demo_Terrain Release GPU 0.449ms(사전 패스 끔 0.385ms), `--occlusion` 지형이 정적 인스턴스 1636 중 199 가림 (2026-10-01 master 머지, 트랙 Debug/Release 경고 0·테스트 100%·쿠킹 셰이더 69개 실패 0·회귀 화면 오류 0)
- [x] SSR 반사 형태 수정(2026-10-02, 사용자 보고 Demo_Decals 금속 데칼 반사 번짐): Hi-Z 칸 매핑이 내림한 밉 크기를 써서 화면 크기가 2^밉 배수가 아니면 칸이 밀려 광선이 물체를 뚫음 → 칸 크기 2^밉 그대로. SSR 추적/흐림이 데칼 DBuffer 법선·거칠기 반영(사전 패스엔 데칼 없음), 흐림 반경을 반사된 상 깊이 기준(`ComputeSsrReflectionViewDepth`)
- [x] HDR 하늘 IBL 점박이(2026-10-02, 사용자 보고 Demo_HdrSky 금속구 해 반사·주황 구): 조도/프리필터가 원본 밉 0만 256 고정 표본으로 읽어 해가 표본마다 찍힘 → 하늘 큐브 밉 체인 + 필터드 중요도 샘플링(`ComputeFilteredSampleLod`), 거울 밉은 출력 크기에 맞는 원본 밉. 후속: 반사 캡처 원본 큐브에도 밉 체인
- [x] SSR 데칼 리벳 깜빡임(2026-10-02): 데칼 노멀 반영 후 작은 곡면 반사가 움직일 때 깜빡임 → 월드 곡률 반경 페이드(`ComputeSsrCurvatureFade`, 20→60cm). 남은 것: 반사 안 빈 영역(상자 뒤 등) 경계가 움직일 때 캡처/하늘과 바뀌는 것은 SSR 한계
- [ ] 실행 검증 (사용자): 카메라 빠르게 돌리기/캐릭터 달리기 때 TAA·SSR 잔상, 에디터 메뉴로 실제 씬 반사 캡처 굽기(경로가 채워지므로 씬 저장), 모든 데모 화면 변화(기본 켬 — 끄기 `--no-taa`/`--no-ssao`/`--no-ssr`/`--no-depth-prepass`)
- 후속: Demo_Fog 파란 금속 지붕 아래 SSR 노이즈(TAA 클립이 못 거름 — 노이즈 제거 필터), SSR 화면 밖 → 캡처 경계, 비스듬한 금속 벽 잡티, 캡처 상자 회전·확산광 미반영·최대 8, 볼류메트릭 안개 로컬 라이트 전부 순회(최대 128, 그림자 없음), SSR이 지난 프레임 안개까지 반사(안개 두 번), 데칼은 불투명만, 직교 카메라 볼류메트릭 끔, 픽셀 아트 SSR/데칼 미확인, 지형 브러시 중 움직임 벡터 없음, 지형 사전 패스 MRT 포맷 하드코딩(`FSceneRenderer` 포맷과 함께 변경), 종료 시 라이브 오브젝트 보고 미확인(디버거 출력만)

## Phase 34 — 지형 / 식생 (트랙 E, 2단계)

- [x] 34-1. `FTerrainComponent` + `.eterrain`(JSON + base64: 16비트 높이, 정점별 RGBA8 레이어 가중치 4개, `FTerrainLibrary` 경로 공유). 브러시 올리기/내리기/평탄화/부드럽게/노이즈/레이어 칠하기(크기·세기·감쇠, 지형 위 브러시 원, `[ ]` 반경, Shift 반대). 높이맵 가져오기 PNG16/8·RAW16, 내보내기 PNG16·RAW16. Undo = 컴포넌트 `EditRevision` + 바뀐 영역 전/후 기록 사슬(갈래 처리, 256MB, 스트로크 끝에만 기록). 충돌 = Jolt 높이장 정적 바디(`Physics/TerrainCollision.cpp`, 축 순환 + 표본 전치, 편집 시 재생성). `--terrain-brush-test`
- [x] 34-2. `TerrainRenderer` + `Terrain.hlsl`(Mesh.hlsl include로 조명 공유, 지형 리소스는 레지스터 공간 1): R16 높이 텍스처로 정점 생성, 64셀 청크 + 거리 LOD + 스커트, 같은 LOD 인스턴싱, 메인/그림자 장별 청크 컬링, 레이어 4개 PBR 블렌드(노멀, 반복 무늬 완화), 방향광/로컬 그림자 받기·드리우기(`FShadowCasterHook`), 선택 아웃라인, 브러시 영역만 `CopyTextureRegion`. `--terrain-lod-colors`, `--terrain-force-lod N`
- [x] 34-3. `FFoliageComponent` + `.efoliage`(타입 목록 + 인스턴스 base64): 밀도/크기 무작위/경사·높이 제한/지면 정렬/컬링·그림자 거리/충돌. 렌더 = 기존 GPU 인스턴싱(`FMeshInstanceList::AddExternal`), 20m 셀 컬링, 끝 15% 크기 페이드, 화면 크기 LOD, 그림자 거리 밖 제외. 내장 절차 메시(풀/덤불/활엽수/침엽수/바위), 나무 충돌 = 컴포넌트당 캡슐 StaticCompound. 폴리지 창 칠하기/지우기 + Undo. `--foliage-brush-test`, `--generate-terrain-demo`. 데모 `Demo_Terrain`(513², 폴리지 3.6만, 에셋 5.2MB). 측정 Release CPU 0.41ms / GPU 0.35ms, 드로우 14(그림자 36). 테스트 Terrain_* 12개 + TerrainRenderer/Foliage_* 6개(Renderer), TerrainCollision_* 3개 + FoliageCollision 1개(Physics), TerrainEditHistory_* 3개(Editor) (2026-10-01 master 머지, 트랙 Debug/Release 경고 0·테스트 100%·화면 확인 오류 0. 트랙 D 사전 패스 통합은 D 머지 때)
- [ ] 실행 검증 (사용자): 브러시 손맛(세기 1 = 초당 10m, 평탄화/부드럽게/칠하기 초당 8, 노이즈 800cm), 실제 마우스 스트로크, 풀 밀도(100m²당 150), 나무 캡슐(반지름 22cm)에 부딪히며 걷기, 그림자 거리 기본값, 지형 선택 표시
- 후속: 지형이 로컬 라이트 그림자를 드리우는 것은 미확인(데모에 로컬 라이트 없음), 픽셀 아트 경로 미확인, 지형 그림자 바이어스 고정, 와이어프레임 무시, 일반 메시 위 폴리지는 경계 상자 윗면 근사, 인스턴스 개별 선택 없음, 폴리지 LOD 히스테리시스 없음, 머티리얼 이동 시 메모리 사본 경로 미갱신, 씬 열기 시 저장 안 한 지형/폴리지 편집 버림, 해상도 셀 수는 64 이하 2의 거듭제곱으로 나뉘어야 함(129/257/513/1025)

## Phase 35 — 편집 도구 (트랙 F, 2단계)

- [x] 35-1. 애니메이션 그래프 편집기 `FAnimGraphEditor`(imgui-node-editor): 상태(클립/1D 블렌드)·"어느 상태든" 노드, 핀 끌어 전이, 우클릭 메뉴(추가/복제/시작 상태/우선순위), 파라미터(이름 바꾸면 참조 갱신)·상태·블렌드 축 위젯·전이 조건 속성, 저장 전 편집 상태 미리보기, 플레이 중 디버그(현재 상태 초록/섞이는 상태 노랑, 가중치, 실제 파라미터). `.eanimgraph` v2(`EditorPosition`, `Editor{PreviewModel, AnyStatePosition}` — 읽기 v1/v2, 쓰기 v2; `FoxCharacter.eanimgraph`는 v1 유지). 핫 리로드 = `FAnimGraphLibrary` 세대 번호 + `Invalidate(경로)` → 바뀐 컴포넌트만 다시 묶기(파라미터 유지, 같은 이름 상태에서 이어감), 편집기 저장·파일 감시 공용. 테스트 `AnimGraphTests` 3개 추가
- [x] 35-2. 컷신 시퀀서: `.esequence` v1(`Scene/Sequence.*`, `SequencePlayer.*`) 트랙 Transform/Property(숫자·색·bool·정수)/CameraCut/Animation(클립 구간)/Event, 보간 계단/직선/곡선(3차 에르미트). `FSequencePlayerComponent`(자동 재생, 반복, 속도, RestoreState, 비복제). 카메라 컷 = 대상 카메라 bPrimary + Priority(1<<20), 끝나면 두 값만 복원. 갱신 순서 스크립트 → **시퀀스** → 물리 예측…. Lua `entity:PlaySequence/StopSequence/PauseSequence/...`, `OnSequenceEvent_<이름>`/`OnSequenceFinished`(다음 틱). 편집기 `FSequenceEditor`(타임라인 스크럽/확대, 키·구간 끌기 + 프레임 맞춤(Alt 끔), K/더블클릭 현재 값 키, 복사/붙여넣기, 뷰포트·카메라 미리보기 — 닫기/플레이/씬 열기 시 복원, 저장·자동 저장·Undo에는 원래 값). 멀티플레이는 로컬 연출(맞추려면 Multicast RPC로 PlaySequence). 대상 바인딩 = 엔티티 이름 경로(재생 엔티티 하위 → 씬 전체). 데모 `Demo_Cinematic` + `Sequences/Intro.esequence`(11초). 테스트 `Sequence_*` 7개, `SequenceScript_PlayEventsAndFinish`. 버전 변경 없음 (2026-10-01 master 머지, 트랙 Debug/Release 경고 0·테스트 100%·화면 확인 오류 0)
- [ ] 실행 검증 (사용자): 그래프 노드/핀 끌기·블렌드 마름모, Demo_Animation 플레이 중 Fox 그래프 저장 → 핫 리로드 상태·파라미터 유지, 시퀀서 키/구간 끌기·Alt·K, 미리보기 켠 채 Undo/저장 시 원래 값 유지, Demo_Cinematic 연출 타이밍·구도·R/P 키
- 후속: 소리 트랙, 애니메이션 그래프가 붙은 모델엔 애니메이션 트랙 미적용(경고), 복제 엔티티를 시퀀스로 움직이면 클라이언트 보간과 충돌, 미리보기 중 직접 고친 값은 다음 평가에 덮임, 회전 오일러 성분 보간(±90° 피치 어색), 편집기가 매 프레임 에셋 JSON 재생성, 링크 위 우선순위 번호 표시·키 여러 개 선택 없음

## Phase 36~40 — 1순위 기능 + 반사 품질 (2026-10-02, 사용자 결정: "1순위 차례대로 → Demo_Reflections 반사 개선")

결정 (2026-10-02 사용자 선택, 모두 권장안): 머티리얼 = 블렌드·양면·인스턴스(노드 편집기는 후속), 리소스 = 도달성 수거 + 비동기 업로드(밉 스트리밍은 후속), 디버그 = 자체 CVar/콘솔 + Tracy, GI = 구운 프로브 볼륨, 반사 = 원인 분석 → SSR 수정 + 필요 시 DXR(도입 여부는 분석 후 다시 묻기). 진행 = 병렬 트랙: 1단계 A(36)·B(37)·C(38) worktree 동시 → 2단계 39(GI, A 머지 후 — Mesh.hlsl/SceneRenderer 충돌) → 3단계 40(반사).
공통 규칙: Phase 30~35와 같음(`Plans.md`/`CLAUDE.md`는 메인만, 등록부는 끝에 추가만, 트랙은 커밋 메시지에 규칙 기록).

## Phase 36 — 머티리얼 블렌드 모드 / 양면 / 인스턴스 (트랙 A)

**DoD**: `.emat`에 `BlendMode`(Opaque/Masked/Translucent/Additive)·`AlphaCutoff`·`TwoSided`·`Parent`(인스턴스: 부모 값 위에 지정한 값만 덮어씀)가 있고 머티리얼 편집기에서 바꾸면 즉시 반영. Masked는 메인/사전 패스/그림자(방향광·로컬)에서 잘리고, Translucent/Additive는 불투명 뒤 정렬된 전방 패스(방향광·로컬 라이트·IBL·안개, 깊이 쓰기 없음, TAA 반응형 마스크). glTF `alphaMode/alphaCutoff/doubleSided` 반영. 데모 씬(철망/잎/유리) 화면 확인, 디버그 레이어 0건.

- [x] 36-1. 에셋/런타임: `FMaterialAsset` 필드 + 인스턴스 해석(부모 체인, 순환 검출) + `FMaterial` 렌더 상태, glTF 로더
- [x] 36-2. 렌더: 배치 키 파이프라인 비트에 변형(블렌드·컬), PSO 변형, Masked clip(메인/사전/그림자 — 그림자 배치 키에 머티리얼), 반투명 정렬 패스
- [x] 36-3. 편집기/데모/테스트 (2026-10-02 머지: `Demo_Materials`, `MaterialTests` 10개, `ModelVersion` 8, 화면·디버그 레이어 0건)
- 후속: Masked 밉 알파 커버리지 감소(먼 잎이 얇아짐 — 커버리지 보존 밉/알파 투 커버리지), 반투명 정렬은 인스턴스 중심 기준(교차·자기 정렬 없음, 움직임 벡터 없음), `OnAssetMoved`가 `ParentChain`/편집 중 원본 키 미갱신, `EditedMaterialSources` 비우지 않음(37 수거와 연결), 선택 아웃라인이 컷아웃 무시, FBX 알파 모드, 통계 창 반투명 드로우, Demo_Terrain 폴리지 Masked 잎 적용

## Phase 37 — 리소스 수거 / 비동기 업로드 / VRAM 예산 (트랙 B)

**DoD**: 맵 전환·서브 씬 언로드·에디터 씬 열기 뒤 어디서도 참조하지 않는 메시/텍스처/머티리얼/모델/파티클이 지연 해제된다(도달성 표시 → 수거, 썸네일·편집기·고정 목록은 루트). 텍스처/버퍼 업로드는 복사 큐 + 업로드 링으로 펜스 대기 없이 진행하고 디코드/BC 압축은 백그라운드 스레드. VRAM 사용량(`QueryVideoMemoryInfo`)과 리소스 바이트 통계 표시. 맵 전환 반복 시 메모리 평탄 확인(테스트 + 로그).

- [x] 37-1. 수거: 참조 수집(씬·서브 씬·편집기·렌더러 캐시·UI, `AddRootProvider`) → 표시(`ResourceGc::Compute`) → `Destroy*` 지연 해제, 경로/모델/파티클 캐시와 `EditedMaterialSources` 정리, 썸네일 LRU 256. 트리거 = 맵 전환·서브 씬 내림·에디터 씬 열기/플레이 정지·썸네일 대기열 비움(2틱 지연) + `r.CollectResources`. 측정: 런타임 Demo_Travel A↔B 6회 평탄(이전 맵 머티리얼/텍스처만 오감), 에디터 썸네일 해제 VRAM 354.6 → 203.7MB, Showcase/PixelArt 오해제 0
- [x] 37-2. 비동기 업로드: `FD3D12UploadQueue`(COPY 큐 + 64MB 영구 매핑 링, 묶음 펜스, 32MB 초과는 전용 스테이징, 순수 할당 규칙 `UploadRingAllocator`), 대상 COMMON → BeginFrame이 완료 텍스처만 PIXEL_SHADER_RESOURCE로 전이 + 직접 큐가 복사 펜스 GPU Wait, `IsReady`(로딩 중 텍스처 = 기본 텍스처, 메시 = 수집 제외), 작업 큐 `FJobQueue`(디코드·BC 압축·쿠킹·모델 파싱 병렬, GPU/ECS는 메인 완료 콜백), `r.AsyncLoading`(자동 검증은 프레임마다 비우기, 테스트·도구는 동기), `--log-hitches`. 측정: Demo_PixelArt 로드 메인 스레드 821 → 약 300ms, Showcase 128 → 83ms, 쿠킹 없는 Materials 131 → 12ms. 테스트 `UploadRing_*` 5·`JobQueue_*` 4·`UploadQueue_GpuTextureAndBufferRoundtrip` (2026-10-02 master 머지: 트랙 A와 충돌 4곳 양쪽 유지, Debug 11개 묶음 통과, Release 빌드, 런타임 PixelArt/Travel/Showcase/Materials·에디터 Materials/PixelArt 플레이 화면·디버그 레이어 0건)
- 후속(37-2): 모델 비동기 로드(엔티티 지연 배치), 힙 배치(placed) 리소스, 비압축 이미지·UI 글꼴·지형 텍스처는 동기, 업로드 링 통계를 통계 창에 연결(getter 준비됨), 밉 스트리밍
- [x] 37-3. 예산/통계: `FD3D12Device::QueryVideoMemory`, `GetMemoryStats`(텍스처 `GetResourceAllocationInfo`, 메시 `GetGpuBytes`), 통계 창 "리소스 메모리"(VRAM 막대·종류별 표·수거 버튼·자동 수거), `stat memory`/`r.ResourceStats`, 예산 초과 경고. 테스트 `ResourceCollector_*` 7개 + Console `stat memory` (2026-10-02 트랙 A master 머지: Debug 11개 묶음 통과, Release 빌드, 런타임 Travel/PixelArt·에디터 Showcase 화면·디버그 레이어 0건)
- 후속(37-1/3): 직접 만든(`Create*`) 리소스는 수거 안 함, 루트 미등록 코드가 경로 핸들을 오래 들면 기본 리소스로 대체됨, 프리팹 템플릿 씬 루트 아님, Sandbox Tick 없음

## Phase 38 — CVar / 콘솔 / Tracy (트랙 C)

**DoD**: `FConsoleVariables`(이름·타입·기본값·도움말·변경 콜백)에 렌더 토글(`r.SSR`, `r.TAA` 등 기존 `--no-*`/`--debug-view`)이 등록되고 기존 명령줄 인자는 그대로 동작 + `--cvar r.SSR=0`. 에디터 콘솔 패널(자동 완성·기록), 런타임 \` 키 콘솔(게임 UI 그리기, 패키지 빌드 비활성), 명령(`stat fps` 등). Tracy CPU/GPU(D3D12) 존이 주요 구간에 있고 Tracy 끄면 비용 0.

- [x] 38-1. CVar/명령 레지스트리 + 기존 플래그 이전 + 테스트
- [x] 38-2. 에디터 콘솔 패널, 런타임 콘솔 오버레이 + 통계 오버레이
- [x] 38-3. Tracy 도입(`ThirdParty.cmake`, 옵션 `E_TRACY`) + 존 (2026-10-02 머지: Tracy v0.11.1, `ConsoleTests` 9개, 예전 플래그 결과 픽셀 차이 0)
- [ ] 실행 검증 (사용자): Tracy 뷰어(`windows-0.11.1.zip`의 tracy-profiler.exe, 127.0.0.1:8086)에서 GPU 존 표시, 에디터 출력 로그 입력 줄 자동 완성, 런타임 \` 콘솔
- 후속: 런타임 콘솔 IME 조합 글자 미표시, `r.*` 셰이더 리로드 등 명령, CVar 값 저장(ini), Lua `Console.Get/Set`, 전용 서버 콘솔 입력(`--exec`만)

## Phase 39 — 구운 프로브 볼륨 GI (36 머지 후)

**DoD**: `IrradianceVolumeComponent`(상자 + 간격) → 에디터에서 굽기(프로브마다 큐브 렌더 → SH L2 + 가시성), `.eprobes` 저장, 셰이더가 위치 기반 삼선형 보간(누수 방지)으로 하늘 조도를 대체. 정적·동적·스킨·지형 모두 받음. 실내 데모 비교 화면.

- [ ] 39-1. 굽기/파일/로드
- [ ] 39-2. 셰이더 적용 + 누수 방지 + 디버그 표시

## Phase 40 — Demo_Reflections 반사 품질 (39 후)

사용자 보고(2026-10-02): 파란 벽에 반사된 물체가 깜빡이고 모양이 이상하며 각도에 따라 안 보임.

- [ ] 40-1. 연속 프레임/카메라 이동 캡처로 원인 분리(추적·두께·이력·캡처 전환)
- [ ] 40-2. SSR 수정 → 남는 한계는 DXR 반사 도입 여부를 사용자에게 다시 묻기

## Phase 41~42 — 우선순위 1: 스크립트 편의 / 애니메이션 고급 (2026-10-02, 사용자 결정: "우선순위 1 병렬 진행")

결정 (2026-10-02 사용자 승인): 상용 엔진 비교 조사의 우선순위 1 두 묶음을 병렬 트랙 2개로(A = 41, B = 42). 트랙 A를 셋으로 쪼개지 않는다(셋 다 `Scripting/ScriptSystem.h` 훅에 추가하므로 충돌). 트랙 B 안의 기능 4개는 최종 자세 계산을 함께 고치므로 순서대로. Phase 37/39/40은 대기 유지(이번 두 트랙과 대상 파일이 거의 겹치지 않음).
권장안 적용(트랙 내부 설계): 몽타주는 애니메이션 파라미터와 같이 로컬 전용(비복제 — Phase 27과 같은 입장), 발 IK 바닥 탐색은 World(`FGameWorld`, 물리·UpdateTransforms 뒤)가 기존 `FPhysicsSystem::Raycast`로 하고 훅으로 `FAnimationSystem`에 목표를 넘긴다(Scene → Physics 의존 금지, 트랙 B는 새 물리 질의 함수를 만들지 않음), 3D 디버그 선은 기존 에디터 선 그리기 경로(`FNavMeshDebugRenderer`/그리드)를 먼저 확인해 재사용.
공통 규칙: Phase 30~35와 같음(`Plans.md`/`CLAUDE.md`는 메인만, 트랙은 커밋 메시지에 규칙 기록, 등록부 `SceneReflection.cpp`·Lua 바인딩·`Tests/CMakeLists.txt`·`Shaders.json`은 끝에 추가만, 버전 값은 머지 때 메인이 최종 결정).

## Phase 41 — 스크립트 편의: 타이머·코루틴 / 범위 검사 / 3D 디버그 선 (트랙 A)

**DoD**: Lua에서 타이머(`Timer.After/Every/Cancel` 류)와 코루틴 대기(`Wait(초)`, 다음 프레임 대기)를 쓸 수 있고 스크립트 인스턴스별로 관리되어 OnDestroy·핫 리로드·EndPlay에서 정리된다(플레이 정지 후 편집 씬에 남는 것 없음). `FPhysicsWorld`에 구/상자/캡슐 겹침 검사와 쓸어 보기(sweep)가 있고(트리거 레이어 제외, 충돌 끄기 그룹 존중, cm↔m은 경계에서만) Lua는 `FScriptPhysicsHooks`로만 부른다. 스크립트/C++에서 월드 3D 선·상자·구를 지속 시간과 함께 그릴 수 있고 에디터 뷰포트와 런타임 둘 다 보이며 GPU 없는 서버에서는 무시된다. 순수 로직은 테스트, 화면 확인 + 디버그 레이어 0건.

- [x] 41-1. 타이머 / 코루틴 대기 (Lua) + 테스트 — `Timer.*`, `Coroutine.*`, `Wait/WaitFrames/WaitUntil`, 인스턴스 소유(`FInstanceScope`)
- [x] 41-2. 겹침 검사 / 쓸어 보기 (C++ + Lua) + 테스트 — `FPhysicsWorld::Overlap/Sweep`, Lua `Physics.Overlap*/…Cast`, 게임 모듈 `GetPhysics()`(API 9)
- [x] 41-3. 3D 디버그 선 그리기 (C++ + Lua, 에디터/런타임) + 데모 씬 — `FDebugDraw`/`FDebugDrawRenderer`(NavMeshDebug.hlsl 재사용), `Demo_ScriptUtils` (2026-10-02 master 머지: 11개 테스트 묶음 통과(ScriptingTests 64·PhysicsTests 49·RendererTests 196), 런타임/에디터 플레이 화면 확인, 디버그 레이어 0건)
- [ ] 실행 검증 (사용자): Demo_ScriptUtils 직접 플레이(코루틴 순찰, 상자 위에서 탐지 구가 빨강), Lua에서 Timer/Wait 실사용 손맛
- 후속: 타이머·코루틴은 Update에서만 진행(LateUpdate 없음), `Every`는 밀린 횟수를 몰아 부르지 않음, 핫 리로드 시 타이머 취소(OnStart 반복 타이머 재생성 안 됨), 사용자 `coroutine.wrap` 안 `Wait`는 사용자 코루틴이 받음, `Physics.Raycast` ignore 인자 없음, 쓸어 보기는 첫 닿음만, 플레이 밖 C++ 디버그 선은 Clear 전까지 남음, `FNavMeshDebugRenderer`와 선 파이프라인 중복(통합 후속), 런타임 셰이더 리로드 경로 미연결

## Phase 42 — 애니메이션 고급: 2D 블렌드 / 레이어·본 마스크 / 몽타주 / IK (트랙 B, 순서대로)

**DoD**: 애니메이션 그래프에 2D 블렌드 스페이스 상태, 본 마스크로 부분만 섞는 레이어(예: 상체 따로), 그래프와 별개로 한 번 재생하는 몽타주(슬롯·블렌드 인/아웃·Lua `PlayMontage` 류, 로컬 전용)가 있고 그래프 편집기에서 편집된다. 2본 IK(발 바닥 맞춤 — 탐색은 World, 시선 Look-At)가 래그돌(`bPhysicsPose`) 중에는 꺼진다. `.eanimgraph` 형식이 바뀌면 버전을 올리고 이전 파일을 읽는다. `Scene/AnimGraph.h` 머리 주석(노티파이 판정 포함)과 `AnimGraphTests`를 함께 갱신. 화면 확인 + 디버그 레이어 0건.

- [x] 42-1. 2D 블렌드 스페이스 (순수 식 + 테스트) — 상태 `BlendParameterY` + 샘플 `PositionY`, 그래디언트 밴드 `AnimGraphMath::ComputeBlendSpace2DWeights`, 편집기 점 배치
- [x] 42-2. 레이어 블렌드 / 본 마스크 — `FAnimStateMachine`(에셋 = 기본 레이어) + `Layers`(마스크, `Weight × WeightParameter`), `FAnimBoneMask{Bone, Weight, BlendDepth}`, 로컬 공간 `BlendMasked`, 노티파이 진행 `FAnimNotifyTrack`
- [x] 42-3. 몽타주 (슬롯, Lua) — `Scene/AnimMontage.h`, 슬롯 = `.eanimgraph` `Slots`, `FAnimationSystem::PlayMontage/StopMontage/IsMontagePlaying`, Lua `entity:PlayMontage/StopMontage/IsMontagePlaying` + `OnMontageEnded(clip, interrupted, slot)`, 로컬 전용
- [x] 42-4. 2본 IK(발) + Look-At — `Scene/AnimIK.h`, `FFootIkComponent`/`FLookAtComponent`, 바닥 탐색 `FGameWorld::UpdateFootIkProbes`(기존 Raycast, 한 프레임 늦음), Lua `SetLookAtTarget/ClearLookAtTarget`, `Demo_AnimAdvanced` (2026-10-02 master 머지: `.eanimgraph` v3, 새 테스트 12개, 11개 테스트 묶음 통과, 런타임/에디터 플레이 화면·디버그 레이어 0건, `GameModuleApiVersion` 9 유지 — A의 9에 함께 포함, 미배포)
- [ ] 실행 검증 (사용자): Demo_AnimAdvanced 직접 플레이(E 키 몽타주, 계단/경사 발 IK, 공을 따라가는 시선), 그래프 편집기 2D 점 끌기·레이어·마스크·슬롯 편집 손맛
- 후속: Fox에 방향 전환 클립이 없어 2D Direction 축은 보폭 속도만 바뀜(연출용 아님), 몽타주 이름 섹션/섹션 점프 없음, 게임 모듈 몽타주 끝 콜백 없음(`IsMontagePlaying` 폴링), 골반은 내리기만, IK 균등 스케일 가정, 발 탐색 한 프레임 늦음

## 쇼케이스 씬 — Demo_Showcase (2026-10-02, 사용자 요청: "지금까지 나온 기능들을 한 씬에서 최대한 기술 데모처럼")

결정 (2026-10-02 사용자 선택, 모두 권장안): 보는 방식 = 시작 시 시퀀서 자동 투어(구역별 카메라 컷) → 끝나거나 키를 누르면 여우 캐릭터 직접 조작. 배경 = 야외(지형 + 식생 + HDR 노을 하늘 + 안개)와 가운데 평평한 광장, 광장 둘레에 기능별 구역. 제외: 픽셀 아트(화면 전체 모드), 멀티플레이, 맵 이동/스트리밍, 스트레스 씬.
**DoD**: `Scenes/Demo_Showcase.escene` 하나에서 구역별로 캐릭터·애니메이션(그래프/몽타주/발 IK/시선), 물리(쌓기·관절·래그돌·겹침 검사 디버그 선), 머티리얼·반사(PBR 헬멧, 크롬, 유리/반투명, 마스크 잎, 반사 캡처), 조명·안개·데칼(점광원/스포트 그림자, 볼류메트릭 안개, 발광), 파티클(모닥불 + 소리), AI(순찰 경비 BT + 내비메시), 프리팹(가로등), 게임 UI(다국어 HUD)가 보인다. 투어 → 조작 전환이 동작하고, 런타임/에디터 플레이 화면 확인 + 디버그 레이어 0건, Release 프레임 시간 기록.

- [x] S-1. 지형/식생 배경 + 광장 + 하늘/안개 + 구역 배치 — `Terrain/ShowcaseTerrain.eterrain`(320m, 가운데 반경 44m Z=0), `Foliage/ShowcaseFoliage.efoliage`(약 1.6만), 생성은 에디터 `--generate-showcase-terrain`
- [x] S-2. 구역별 기능 배치 (기존 에셋·스크립트 재사용) — 광장 중심 27m·60도 간격 6구역(캐릭터·애니메이션 / AI / 물리 / 모닥불 / 머티리얼·반사 / 조명·안개·데칼) + 광장 프리팹 가로등·화살표 데칼
- [x] S-3. 투어 시퀀스 + 조작 전환 + HUD 안내 — `Sequences/ShowcaseTour.esequence` 63초(구역별 컷 + `Zone_<이름>` 이벤트 → `ShowcaseDirector.lua`), Space/Enter 건너뛰기, `UI/ShowcaseHUD.eui`(`Showcase.*` 문자열 ko/en, L 전환)
- [x] S-4. 굽기(내비메시, 반사 캡처) + 검증 + 성능 기록 — `.enav`/`.ecapture` 커밋, Release 1280x720 투어 1.55ms(GPU 1.21) / 조작 1.93ms(GPU 1.68 — 볼류메트릭 안개 0.43·사전 패스 0.40·SSR 0.30) (2026-10-02 master 머지: Debug/Release 빌드·11개 테스트 묶음 통과, 투어/조작/에디터 플레이 화면·디버그 레이어 0건). 엔진 수정: 입력 안 받는 HUD도 UI 애니메이션 진행(`FUISystem::Update`, 테스트 `UISystem_AnimationTicksWithoutInput`), 런타임 종료 시 GPU Flush 후 렌더러 해제
- [ ] 실행 검증 (사용자): 플레이어 E 몽타주, 3D 모닥불 소리(자동 검증은 음소거), 경비가 플레이어 추적, 플레이어 발 IK, 마우스 시점 손맛
- 후속: 조명 구역 볼류메트릭 빛줄기 약함(안개 밀도 올리면 골짜기 전체가 바램), 높은 소개 컷에서 광장 돌판 하늘 반사가 밝음, 가까운 컷 흙 지형 텍스처 거침, HUD 패널이 밝은 하늘 위에서 회색, 투어는 실제 시간 기준(자동 검증 프레임과 어긋남)

## 픽셀 아트 — 움직이는 물체 도트 스냅 (2026-10-02, 사용자 요청: "카메라가 이동할 때 자글자글하지 않게, 옵션으로")

결정 (2026-10-02 사용자 선택): 넣을 것 = 움직이는 물체 스냅만(원근 근사 스냅·회전 계단화는 제외), 옵션 위치 = `FPixelArtComponent`(기존 `SnapCamera` 옆). 카메라 스냅은 이미 있음(직교 전용).
설계(권장안): 렌더 때만 적용하고 끝나면 원래 값으로 되돌린다(게임·물리 위치 불변). 씬에 정적/이동 구분 플래그가 없으므로 "한 번이라도 움직인 최상위 루트"만 스냅(멈춰도 계속 — 멈출 때 반 도트 튐 방지). 정적 물체는 건드리지 않아 타일 이음새가 생기지 않는다. 하위 트리 전체에 루트의 같은 이동량(스킨 뼈 포함), 소켓 부착은 대상 모델 루트의 이동량. 직교 카메라만(원근은 SnapCamera와 같이 건너뜀).

- [x] P-1. `bSnapMovingObjects`(기본 켬, 인스펙터 "움직이는 물체 도트 스냅") + `FPixelArtMath::ComputeObjectSnapDelta` + 테스트 `PixelArt_ObjectSnapLandsOnCameraGrid`/`PixelArt_ObjectSnapOnlyMovedRootsAndRestores`
- [x] P-2. 렌더 적용/복원(`FPixelArtObjectSnap`, `FSceneRenderer::RenderFrame` 픽셀 아트 분기) (2026-10-02: Debug/Release 빌드·11개 테스트 묶음 통과, Demo_PixelArt 런타임/에디터 플레이 화면·디버그 레이어 0건, 엔진 DLL 내보내기 57817개)
- [ ] 실행 검증 (사용자): Demo_PixelArt에서 옵션 켜고/끄고 움직이는 물체 테두리 자글거림 비교 (정지 화면으로는 판단 불가)
- 후속: 물체별 제외 표시(지금은 전체 옵션만), 부모가 멈춰 있고 자식만 움직이는 경우(문, 풍차 날개)는 스냅 안 됨, 원근 카메라 미지원, 렌더 중 GPU 파티클 이미터 위치도 텍셀 반 개 이하로 함께 밀림

## 픽셀 아트 — 카메라 이동 자글거림 수정 + 데모 움직임 (2026-10-02, 사용자 보고: "에디터 플레이 중 카메라가 부드럽게 움직일 때 자글거림, 직교만", 참고 https://ch5saeng.tistory.com/112 (Never's End GDC 2026))

원인 분석(실험): 카메라를 정확히 도트 5칸 옮긴 두 장면을 20px 밀어 비교 — 이론상 0%. 수정 전 0.48%(기둥 테두리 전체 등) → SSAO만 끄면 0.22%. SSAO가 반해상도(도트 한 칸 이동에 반 칸씩 어긋남) + 화면 고정 노이즈였다. 카메라 스냅·그림자 텍셀 스냅은 이미 정상.
- [x] 픽셀 아트 씬 렌더 동안 SSAO를 전체 해상도 + 월드 도트 격자 노이즈(`GridCellNoise(픽셀 + GridOrigin)`, 카메라 스냅 격자 번호)로. 메인 패스 업샘플은 버퍼 폭으로 나눗셈 1/2 판별 → 0.105%(최대 밝기 차 148 → 31). 남은 차이는 모서리 픽셀 뒤집힘(계산 정밀도 — 데모가 정수 좌표·45° 등각이라 경계가 픽셀 중심을 자주 지남)과 외곽선 깊이 비교
- [x] Demo_PixelArt: 회전 상자(Rotator), 오르내리는 기둥(Bobber), 원 도는 공(CircleMover), 원을 걸으며 진행 방향을 보는 여우(새 `Scripts/CircleWalker.lua`), 키네마틱 회전 패들 + 울타리 + 동적 공 5·상자 3, 키네마틱 엘리베이터 + 올라탄 상자, 키네마틱 왕복판, 바닥 충돌. 정지 비교용 CrateB는 그대로
- [x] 2차(사용자 보고 "여전히 자글거림"): 도트 사이 위치(0.25/0.5칸)는 순수 이동이었으나 도트 경계를 넘을 때 테두리 0.31%가 바뀜 → 외곽선/하이라이트만 끄면 0.000%. 원인 = 하이라이트 판정(깊이로 재구성한 노멀의 볼록 검사)이 같은 평면 이웃에서 정확히 0 → 미세 오차 부호가 모서리 전체를 켜고 끔(원래 보이던 하이라이트 선 상당수가 이 오차의 산물). 하이라이트를 깊이 능선(축 방향 단면 꺾임 각도 30~60도 — 깊이 2차 차분으로 재면 구 가장자리가 금 간 것처럼 잡혀 각도로 바꿈, 큰 쪽 한 픽셀, 동률은 왼쪽/위 고정, 곡면은 문턱 아래)으로 교체 → 도트 0.75/1/1.5/2.25칸 이동 모두 0.000%(런타임), 에디터 뷰포트도 정수 px 이동 0.000%
- [ ] 실행 검증 (사용자): 에디터 플레이에서 카메라 이동 시 자글거림 사라졌는지 확인, 바뀐 하이라이트 모양, 물체 스냅 옵션 켜고/끄고 비교
- 후속: 볼류메트릭 안개(화면 격자 160×90 + 시간 누적)·블룸 밉 체인도 도트 이동에 따라 달라질 수 있음(데모에 안개 없음, 미측정), 참고 글의 뼈 단위 양자화(관절 이동을 모델 공간 도트 단위로)와 깊이 대신 정렬 순서는 미도입

## Phase 43~44 + 버그 — 충돌 레이어 / 입력 모드·플레이 빙의 / 뷰포트 선택 (2026-10-02, 사용자 결정: "유니티식 레이어 + 충돌 표, 입력 모드와 플레이 중 빙의/해제 구분, 클릭하면 Ground만 선택되는 버그 — 병렬 진행")

## Phase 43 — 충돌 레이어 (유니티식, 트랙 A)

결정 (2026-10-02 사용자 선택): 언리얼식 오브젝트 채널/트레이스 채널/프로파일 대신 유니티식. 콜라이더·캐릭터 이동마다 레이어 하나(이름은 프로젝트 설정, 최대 16), 프로젝트 설정의 레이어×레이어 충돌 표 하나(대칭, 기본 전부 켬). 겹침 알림은 기존 `IsTrigger` 그대로. 레이캐스트는 선택 인자 레이어 목록(없으면 전부). 레이어 미지정 = Default → 기존 씬 동작 불변.
**DoD**: 설정 창에서 레이어 이름/충돌 표 편집·저장, 인스펙터 레이어 드롭다운, 물리(바디·트리거·캐릭터 이동·레이캐스트)가 표를 따른다, Lua `Physics.Raycast(..., {레이어})`, 테스트 추가, Demo_PixelArt에서 캐릭터끼리 통과 같은 예 확인.

- [x] 43-1. `FCollisionLayerSettings`(Core/Settings/CollisionSettings.h): 16칸(0 = Default 고정) + 대칭 행렬, 이름 기반 JSON(`Config/Collision.json` — `Layers` + `DisabledPairs`), 프로젝트 설정 "충돌 레이어" 섹션 + 전용 UI(이름 칸, 유니티식 삼각 체크 행렬). 콜라이더/캐릭터 이동 `Layer`(이름) + 인스펙터 콤보(리플렉션 `StringOptions`), 없는/빈 이름 = Default. Jolt 레이어 = 종류 | 칸 << 2, 쌍 필터가 행렬 확인(바디·캐릭터 이동/내부 바디·트리거)
- [x] 43-2. 레이캐스트 마스크(C++ `LayerMask` 기본 전체, Lua `Physics.Raycast(..., {"Ground"})`), 테스트 `CollisionLayerTests` 7개, Sample 레이어 Ground/Character/Prop(Character×Character 끔) + Demo_PixelArt 적용 (2026-10-02 master 머지: Debug 11개 묶음 통과, Release 빌드, 설정 창·Demo_PixelArt 이동/충돌·Demo_Showcase 플레이 화면·디버그 레이어 0건)
- [ ] 실행 검증 (사용자): 인스펙터 Layer 콤보로 바꿔 보기
- 후속: Overlap/Sweep 레이어 마스크, 래그돌 캡슐 레이어(항상 Default), 행렬은 플레이 시작 시 고정

## Phase 44 — 입력 모드 / 에디터 플레이 빙의·해제 (트랙 B)

결정 (2026-10-02 사용자 요청): 언리얼 GameOnly / GameAndUI / UIOnly 입력 모드. 에디터 플레이는 "빙의(게임 조작)"와 "해제(편집 — 클릭 선택·기즈모·편집 카메라)"를 구분 — 빙의 중에는 뷰포트 클릭이 선택/기즈모/아웃라인을 만들지 않는다.
**DoD**: Lua/C++ 입력 모드 API(런타임·에디터 플레이 공통), 에디터 플레이 시작 = 빙의, 단축키·툴바로 해제/재빙의, 해제 중 게임은 계속 돌고 입력은 게임에 가지 않음, 상태 표시, 자동 검증 인자, 화면 확인.

- [x] 44-1. `EInputMode` + `FInputModeState` + 순수 `SelectGameInput` + `FInput::WithoutAnyInput`(테스트 `InputMode_Routing`), Lua `Game.SetInputMode/GetInputMode`, Begin/EndPlay에 GameAndUI로 초기화, 런타임/에디터 플레이 공통(GameOnly 들어갈 때 커서 잠금, 풀리면 클릭 시 다시)
- [x] 44-2. 에디터 플레이 빙의/해제: 시작 = 빙의, F8·메뉴 바 버튼·상태 표시, 해제 시 게임은 빈 입력으로 계속 + 편집 카메라(게임 시점에서 시작, 정지 시 복원) + 선택/기즈모/아웃라인, 빙의 중 뷰포트 편집 상호작용 차단(`Context.CanEditInViewport()`), Shift+F1 커서만 해제, 에디터 `Game.SetMouseLocked`(빙의 중 뷰포트 가운데), 게임 마우스 버튼은 뷰포트에서 누른 것만. 검증 `--play-eject` + 테스트 `PlayMode_PossessEjectAndInputMode` (2026-10-02 master 머지: Debug 11개 묶음 통과, Release 빌드, 빙의/해제 화면·디버그 레이어 0건)
- [ ] 실행 검증 (사용자): 실제 마우스로 F8 / Shift+F1 / 커서 잠금
- 후속: 샘플 스크립트(PlayerCharacter/ShowcasePlayer)의 "런타임만" 커서 잠금 주석 정리(이제 에디터 플레이도 잠금), 해제 중 게임 UI 미표시

## 버그 — 뷰포트 클릭이 항상 Ground 선택 (트랙 C, 2026-10-02 사용자 보고: "어딜 클릭해도 무조건 ground가 선택돼")

- [x] 원인 파악 + 수정 + 재현 테스트 — 원인: `FMatrix4x4::TryGetInverse`가 절대 문턱(|det| ≤ 1e-8)이라 직교 뷰-투영(det ≈ 5e-12)을 특이로 보고 항등 행렬 반환 → 직교 카메라 광선이 화면 위치와 무관하게 위로 나가 Ground/없음만 잡힘(플레이 직교 0/6 일치). 상대 문턱(|det| / 행 길이 곱 ≤ 1e-6)으로 수정 — 직교에서 역행렬을 쓰던 안개/데칼/재투영/지형·폴리지 브러시도 함께 바로잡힘. 선택은 AABB 거른 뒤 CPU 정점 삼각형 판정(스킨 메시는 AABB). 검증 인자 `--verify-pick <이름,...> [--verify-pick-ortho|--verify-pick-no-focus]` → 원근 6/7, 직교 6/7, 플레이 4/5(화면 밖 제외). 테스트 5개 추가 (2026-10-02 master 머지: Debug 11개 묶음 통과, Release 빌드, Demo_PixelArt 런타임 화면·디버그 레이어 0건)
- 후속: 스킨 캐릭터는 바인드 포즈 AABB라 옆 물체를 가릴 수 있음(Barbarian ↔ barrel)

## Phase 45 — Demo_PixelArt 액션 RPG 콘텐츠 (2026-10-03, 사용자 요청: "상인, 적, 공격, 스킬, 무기, 아이템, 인벤토리, 체력바 등 게임다운 콘텐츠 + 캐릭터 모션 개선, 병렬 가능하면 병렬, 필요한 에셋은 웹에서")

설계: 씬 파일(`Demo_PixelArt.escene`) 충돌을 피하려고 트랙은 프리팹(`Prefabs/RPG/`)·스크립트(`Scripts/RPG/`)·UI(`UI/RPG/`)·자기 테스트 씬만 만들고, 씬 통합(배치·내비메시 굽기·HUD 연결)은 메인이 마지막에 한다. 트랙 간 계약: 씬의 `GameManager` 엔티티 스크립트(`Scripts/RPG/GameManager.lua`, 트랙 D 소유)가 골드/인벤토리/아이템 정의/전리품 생성/알림을 제공하고 다른 트랙은 `Scene.Find("GameManager")`가 없을 때도 동작하게 쓴다. 플레이어 스크립트(트랙 B)는 체력(`HealthComponent`)·마나/스태미나·장비 교체·물약 사용 메서드를 공개한다. 외부 에셋은 CC0만(출처·라이선스 파일 동봉).

- [x] 45-A. 엔진: `Script.Require`(상태별 캐시, 순환 오류, 핫 리로드 전파), `Camera.WorldToScreen/ScreenToWorldRay`(게임 UI 레이아웃 좌표, 픽셀 아트 포함), 위젯 `Position/Size`, `CloneWidget/RemoveWidget`. 테스트 `ScriptModule_*`/`CameraProjection_*`/UI 편집, 검증 씬 `RPG_Test_EngineAPI(_PixelArt)` (트랙 A)
- [x] 45-B. 플레이어 `Scripts/RPG/PlayerController.lua` + `Prefabs/RPG/Player.eprefab`: 가감속·몸 방향 보간(PlayerMesh 로컬 회전)·질주, 발 속도 맞춘 블렌드(Running_A 345cm/s), 3연타(AttackHit 노티파이, 달리며 상체 슬롯), Q 회전베기·R 돌진 찌르기·Space 구르기(무적)·막기(Block 레이어), 피격/사망/Respawn, 마나·스태미나, 무기·방패 소켓(KnightBare.glb), 파티클 5종, 효과음 WAV(절차 생성 — miniaudio에 Vorbis 없음) (트랙 B)
- [x] 45-C. 적 `Scripts/RPG/EnemyController.lua`(Lua 상태 머신 + `entity:MoveTo`, BT 미사용): 전사/졸개/도적(석궁)/마법사(유도탄 + 광역 예고), 등장·깨어남·순찰·발견(동료 경보)·포위 고리·피격 경직(Poise)·리시·사망 + 전리품, `EnemySpawner`, 그래프 `Skeleton(Crossbow).eanimgraph`, 스켈레톤 `.emeta` 노티파이 Hit/Shoot/Cast, 레이어 `Enemy`(Enemy×Enemy 끔) (트랙 C)
- [x] 45-D. `GameManager.lua`(아이템 17종·전리품 표·20칸 가방·장비·저장 `RPGDemo`), 줍기(`Pickup`, 자석·포물선), 상인(`Merchant`, 상점 구매/판매), HUD/가방/상점 `.eui`(`GameSystems.eprefab`), 아이콘 7Soul CC0, Kenney 효과음 CC0(wav 변환) (트랙 D)
- [x] 45-E. 통합: Demo_PixelArt에 Player/GameSystems/Merchant 프리팹(번호 32~34 유지), 던전 적 3 + 스포너, 길목·숲 스포너, `.enav` 굽기, HUD 머리 위 체력바·데미지 숫자(`GameManager:TrackEnemy` → HUD 위젯 복제 + WorldToScreen), 새 게임 기본 장비 버그 수정, 방패 방어력 1~4, 중복 데미지 숫자 제거 (2026-10-03: Debug 11개 묶음 통과, Release 빌드, 런타임 전투(발견→피격→3연타→사망→골드 줍기)·에디터 플레이 화면·디버그 레이어 0건)
- [ ] 실행 검증 (사용자): 직접 플레이 손맛(콤보·스킬·구르기·막기), 상인 거래, 가방/장비, 사망·부활, 밸런스
- 후속: 막기·방어력은 ApplyDamage 뒤 Heal로 되돌리는 방식(감소 전 피해로 죽는 일격은 못 막음), 적끼리·적과 플레이어 물리적 밀어내기 없음(포위 고리로만 간격), 다국어 키 미사용(한국어 고정), 개발 런타임 ESC = 종료라 창은 E/I/Tab으로 닫음, 효과음은 절차 생성음, 체력바 글자 720p에서 작음

## Phase 46 — 데이터 테이블 / 데이터 에셋 (2026-10-03, 사용자 결정: "구조체 정의 별도 .estruct + RPG 데이터 이전")

설계: 구조체 정의 `.estruct`(필드 이름·타입·기본값·설명 — 여러 테이블이 공유, 언리얼 Row Struct처럼), 데이터 테이블 `.etable`(구조체 + 행 이름 → 값), 데이터 에셋 `.edata`(구조체 값 하나). 필드 타입: bool/int/float/string/Vector2·3·4/색/enum/에셋 경로(필터)/행 참조(테이블+행)/배열. 읽기는 `FFileSystem`(pak), 경로 캐시 + 핫 리로드, Lua `Data.*` + C++ API. 순서: 46-A 핵심 → 46-B 편집기 · 46-C RPG 이전 병렬.

- [x] 46-A. 핵심: `Scene/DataTable.h`(FDataValue/FDataField/FDataStruct/FDataTable/FDataAsset — 편집·`Rebind` 마이그레이션·JSON 왕복), `Scene/DataLibrary.h`(`FDataLibrary::Get()` 경로 캐시·세대·Invalidate·참조 검증·`Save*`·`SaveStructAndMigrate`), `Scene/DataCsv.h`(CSV/TSV, BOM, `|` 배열), Lua `Data.GetRow/GetRows/GetRowNames/HasRow/Load/ResolveRef/GetGeneration`(매 호출 새 테이블), 에디터 핫 리로드, 참조 갱신(Content 기준), 콘텐츠 브라우저 아이콘, 예제 `Data/Samples/`, 테스트 13개 (2026-10-03 머지: 트랙 Debug 11개 묶음·Release·에디터 Verify 0건, DLL 내보내기 61282/65535)
- 후속(46-A): 중첩 Struct, Enum 값 이름 변경 마이그레이션, 숫자 Min/Max, 런타임 핫 리로드, 파일 기준 상대 경로
- [x] 46-B. 편집기: 테이블 편집기(고정 행 이름 열·스크롤·타입별 칸 위젯·RowRef 콤보·배열/긴 글 팝업·행 추가/복제/삭제/이름 변경(자기 참조 RowRef 따라감)/이동·검색·보기 정렬 + "정렬 적용"·칸별 참조 경고·CSV 내보내기/가져오기(교체/병합)), 데이터 에셋 편집기(인스펙터식, 기본값으로), 구조체 편집기(필드 표·상세·사용 파일 목록·`SaveStructAndMigrate` + 확인), 공통 `FDataEditorBase`(디스크 변경 감시·세대 재바인드), 콘텐츠 브라우저 열기/새로 만들기, 검증 인자 `--data-select/--data-popup/--verify-data-roundtrip`, 테스트 `DataTableView_*` 6개 (2026-10-03 머지: Debug 11개 묶음, Release, RPG `Items/Enemies/PlayerBalance` 편집기 화면·디버그 레이어 0건)
- 후속(46-B): 다중 선택·드래그 순서, 다른 파일이 가리키는 행 이름 자동 변경, 에셋 칸 썸네일
- [x] 46-C. RPG 이전: `Data/RPG/` — Items(17행)·LootTables+LootEntries(RowRef)·Enemies(4행, 프리팹은 `StatsRow`만)·PlayerBalance.edata·GameBalance.edata·Shops(Merchant `ShopRow`), 읽기는 `Script.Require("Scripts/RPG/RPGData.lua")`(세대별 캐시), 하드코딩 수치(치명타·스킬·콤보 배율)도 데이터로. 값 동일 확인 (2026-10-03 머지: Debug 11개 묶음, Release, Demo_PixelArt `[데이터]` 경고 0·시작 상태 동일)
- 후속(46-C): 적·플레이어 스탯은 OnStart 한 번 적용(핫 리로드는 새로 생긴 적부터), 시작 아이템은 배열 두 개 짝(중첩 구조체 없음)

## 방화벽 확인 창 (2026-10-03, 사용자 보고: "개인/공용 네트워크에서 실행 허용 창이 자꾸 뜸")

- [x] 원인: 실제 소켓 단위 테스트(GNS 서버·클라이언트, LAN UDP)가 0.0.0.0에 바인드 — 방화벽 허용은 exe 경로별이라 워크트리마다 새로 물음. 에디터/런타임 단독 실행·Tracy(localhost)는 소켓을 열지 않음. 수정: 실제 소켓 테스트 4곳은 `E_TEST_SOCKETS=1`(`Build.ps1/Build.bat -Test -SocketTests`)일 때만, 기본은 건너뜀 로그
- 남음: 에디터 네트워크 플레이/`Verify.ps1 -Multiplayer`/런타임 `--host`는 실제 LAN용이라 처음 한 번은 확인 창이 뜬다(정상)

## 서드파티 소스 캐시 손상 (2026-10-03)

- [x] 46-C 트랙이 워크트리 `Build/_deps`를 메인 `Build/_deps`에 정션으로 이었고, 워크트리를 `git worktree remove --force`로 지우며 정션 너머 메인 소스 일부가 삭제됨(표식 `.populated`는 남아 구성 실패). 복구: `Build/_deps/*-src`·`*.populated` 삭제 + 빌드 폴더 CMakeCache의 `FETCHCONTENT_SOURCE_DIR_*` 비우기 → 커밋 고정 버전 재다운로드, Debug 11개 묶음·Release 통과
