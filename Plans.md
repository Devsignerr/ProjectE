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
- [ ] 1a) (서브에이전트) 스켈레탈 애니메이션
- [ ] 1b) (서브에이전트) Lua 스크립팅 + 에디터 플레이 모드
- [ ] 1c) (서브에이전트) 에디터 편의: Undo/Redo, 그리드·축, 스냅, 복제, 다중 선택, 카메라 저장
- [ ] 2) 물리(Jolt) — 1b 머지 후
- [ ] 3) 오디오(miniaudio)
- [ ] 4) 텍스처 압축(BC7/BC5) + 에셋 최적화
- [ ] 5) 엔진 DLL화 + 게임 모듈

## Phase 7 — 게임 시스템

- [ ] 물리 (자체 충돌 감지 또는 Jolt 도입 검토)
- [ ] 오디오
- [ ] 스크립팅: Lua 스크립트 컴포넌트 (sol2 바인딩, 리플렉션 기반 자동 노출, 파일 변경 감지 핫 리로드)
- [ ] 엔진 DLL화: 엔진 모듈을 공유 라이브러리로 전환, 공개 API 내보내기 매크로(`E_CORE_API` 등)
- [ ] 게임 모듈 분리: `Projects/<이름>/Source/` → `<이름>Game.dll`, 에디터/런타임이 `.eproject` 기준으로 로드, 게임 모듈이 컴포넌트/시스템을 리플렉션에 등록
- [ ] 설치형 엔진 배포: 헤더 + lib/dll + CMake 패키지 설정(`find_package(ProjectE)`), 게임 프로젝트는 게임 모듈만 빌드
- [ ] 게임 모듈 핫 리로드 (에디터 실행 중 게임 DLL 재빌드·교체)
- [ ] 패키징 고도화: 쿠킹 DXIL만 배포(셰이더 소스/DXC 제거), 런타임 파일 로그
