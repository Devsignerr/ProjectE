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
- [ ] 원인 후보 조사 (코드 확인 전 후보): 기본 질량 1kg(`FRigidBodyComponent::Mass` — 100cm 큐브 크기에 비해 가벼움), 강체 없이 콜라이더만 있는 정적 바디의 마찰/반발 값, 구르기 저항 부재(각감쇠 0.05만 있음), 마찰/반발 합성 방식(Jolt 기본: 마찰 기하평균·반발 최댓값), 솔버 반복/충돌 스텝(`Update(..., 1, ...)`)
- [ ] 조사 결과로 세부 수정 항목 확정 후 구현 + 테스트
- [ ] 실행 검증 (사용자 확인)

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

## Phase 13~16 진행 순서 (2026-09-30, 사용자 결정 — 나중에 착수, 지금은 기록만)

Phase 11 완료 후 13 노티파이 → 14 소켓 → 15 프리팹 → 16 인게임 UI 순서. Phase 12와의 선후는 논의되지 않음 — 착수 시 확정.

## Phase 13 — 애니메이션 노티파이 (2026-09-30, 사용자 요청: 언리얼 AnimNotify/AnimNotifyState)

**DoD**: 애니메이션 편집 창 타임라인에서 클립마다 노티파이(한 시점)와 노티파이 스테이트(구간)를 추가·이동·삭제·이름 변경하고 저장하면, 재생 중 해당 시점/구간에서 Lua 스크립트와 C++ 게임 모듈이 이름으로 이벤트를 받는다. 루프·속도 변경·크로스페이드·스크럽에서 누락/중복 없이 발생한다.

결정: 전달은 언리얼의 이름 기반 방식(`UAnimInstance::TriggerSingleAnimNotify`의 `AnimNotify_<이름>` 함수 탐색)을 따라 **Lua + C++ 양쪽**에 보낸다(예: Lua `OnAnimNotify_Footstep()`, 게임 모듈 `OnAnimNotify("Footstep")`). 언리얼은 스테이트에 이름 방식이 없지만(클래스 필수) 여기서는 스테이트도 이름 방식으로 시작/진행(매 프레임)/끝 세 번 보낸다. 코드 없이 고르는 기본 종류(소리 재생/파티클 생성)는 범위 밖(후속). 저장 위치는 모델 사이드카 `.eimport`가 유력(Phase 11) — 착수 시 확정.

- [ ] 데이터: 클립별 노티파이/스테이트 목록 (이름, 시각/구간), 읽기·쓰기 + 테스트
- [ ] 발생 판정 (순수 함수): 이전 시각 → 현재 시각 구간 교차, 루프 경계, 역재생/스크럽, 크로스페이드 중 처리 + 테스트
- [ ] `FAnimationSystem` 연동 → 이벤트 큐 → Lua / 게임 모듈 전달
- [ ] 편집 창: 타임라인 트랙 UI (추가/드래그/삭제/이름), 실행 취소, 미리보기 재생 중 표시
- [ ] 검증

## Phase 14 — 메시 소켓 (2026-09-30, 사용자 요청)

**DoD**: 메시(스태틱/스켈레탈) 편집 창에서 소켓(부착 지점)을 추가·이름 변경·삭제하고 기즈모로 위치/회전을 조정해 저장한다. 스켈레탈 메시 소켓은 뼈를 부모로 가진다. 씬에서 다른 엔티티를 소켓에 붙이면 애니메이션·이동을 따라다니며, 저장/불러오기·플레이 모드 복제에서 유지된다.

결정: 범위는 "편집 + 씬에서 붙이기까지". 편집 창 안의 미리보기 부착 물체(언리얼 미리보기 에셋)는 범위 밖. 저장 위치는 Phase 13과 같은 곳.

- [ ] 데이터: 소켓(이름, 부모 뼈(선택), 상대 트랜스폼), 읽기·쓰기 + 테스트
- [ ] 편집 창: 소켓 목록, 기즈모 편집, 뼈 선택, 실행 취소
- [ ] 부착 컴포넌트 (대상 엔티티 + 소켓 이름) → 트랜스폼 갱신 순서에 편입, 리플렉션 등록, `FSceneCloner` 재매핑
- [ ] Lua API (붙이기/떼기)
- [ ] 검증

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

## Phase 16 — 인게임 UI (2026-09-30, 사용자 요청: UMG처럼)

**DoD**: 에디터의 UI 디자이너에서 위젯(패널/이미지/텍스트/버튼 등)을 끌어다 배치·앵커·스타일을 편집해 UI 에셋으로 저장하고, 런타임/플레이 모드 화면에 그려지며 입력(클릭/호버/포커스)을 받고 Lua/C++에서 값 변경·이벤트 처리를 한다.

결정: 외부 라이브러리 없이 엔진이 직접 구현 + UMG식 비주얼 디자이너. ImGui는 에디터 전용으로 유지(게임 UI에 쓰지 않음). 텍스트 렌더링 방식, 레이아웃 모델 등 세부는 착수 시 결정.

- [ ] 위젯 트리 + 레이아웃(앵커/정렬/크기) — 순수 로직 + 테스트
- [ ] UI 렌더러 (사각형/이미지/텍스트, 해상도 스케일)
- [ ] 입력 라우팅 (클릭/호버/포커스, 게임 입력과의 우선순위)
- [ ] UI 에셋 형식 + Lua/C++ 바인딩 (위젯 찾기, 값 바꾸기, 이벤트)
- [ ] 디자이너 편집 창 (팔레트, 계층, 캔버스 배치, 속성)
- [ ] 검증
