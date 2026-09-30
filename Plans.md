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
- [ ] 5. 복제 핵심: 리플렉션 바이너리 직렬화(`EntityJson` switch 본뜸), NetId 매핑, 생성/파괴 메시지(프리팹 경로 또는 컴포넌트 스냅샷), 연결별 확인 기준 델타, 클라이언트 보간, 접속 시 `.eproject` `PlayerPrefab` 생성 + 소유권
- [ ] 6. 물리 복제: 서버만 시뮬레이션, 클라이언트는 복제 엔티티 바디를 키네마틱으로 두고 위치·회전·속도 보간
- [ ] 7. Lua API: `Net.IsServer/IsClient/LocalPlayerId`, `entity:IsLocallyOwned()`, RPC(`Server_*`/`Client_*`/`Multicast_*` 접두사, `self:CallServer("Name", ...)` 등), `OnPlayerJoined/Left`, 스크립트 실행 위치
- [ ] 8. 게임 모듈 C++ API: `IGameModule::OnPlayerJoined/Left`, RPC 등록, `GameModuleApiVersion` 3
- [ ] 9. LAN 로비/세션: Winsock UDP 브로드캐스트 방 목록(이름/인원/맵), Lua `Net.FindSessions/Host/Connect`, 에디터 ImGui 로비 창(게임 내 로비 UI는 Phase 16에서 이 API 위에)
- [ ] 10. 에디터 다중 클라이언트 플레이: 플레이 설정(단독/리슨/전용 서버+클라이언트, 클라이언트 수 N), 편집 씬을 `Saved/PlayInEditor/`에 저장 → `ProjectERuntime.exe --connect` N개 실행, 정지 시 자식 프로세스 종료
- [ ] 11. 디버그·검증: GNS 가짜 지연/손실 설정, 네트워크 통계 패널, `Tests/NetworkTests`(루프백: 직렬화 왕복/델타/생성·파괴/RPC/입력), `Verify.ps1 -Multiplayer`(서버 + 클라이언트 2개 스크린샷·로그), 샘플 `Demo_Multiplayer.escene` + `PlayerPrefab`
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
- [~] 입력 라우팅 — `FUIInputRouter`(맞히기/호버/눌림/클릭/포커스/Tab/휠, 입력 차단 여부) + 테스트 3개, 디자이너 "미리보기 입력"에서 동작. **게임 입력과의 우선순위 연결은 2단계**
- [~] UI 에셋 형식 + 바인딩 — `.eui` v1(`FUIAsset`, JSON 왕복 테스트, 하위 트리 복사/붙여넣기), `FUIInstance`(런타임 인스턴스). **Lua/C++ 바인딩과 씬 컴포넌트는 2단계**
- [x] 디자이너 편집 창 — `FWidgetEditor`: 계층(끌어서 부모 변경/순서/복제/복사·붙여넣기/삭제), 팔레트(끌어 놓기/클릭 추가), 캔버스(실제 UI 렌더러, 휠 확대, 가운데·오른쪽 드래그 이동, 선택·이동·8방향 크기, 스냅, 앵커 표시, 미리보기 해상도 프리셋), 속성(UI 설정/공통/슬롯(부모별)/종류별/브러시·텍스처 드롭/글꼴), 앵커 프리셋 4x4. 콘텐츠 브라우저 "새 UI", 아이콘, 참조 갱신(Content 기준). 샘플 `UI/SampleHUD.eui`
- [x] 1단계 검증: 단위 테스트 전체 통과(UITests 17개 포함), `Verify.ps1 --open-asset UI/SampleHUD.eui [--ui-select PlayButton --ui-zoom 1]` 스크린샷 확인(한글 SDF/둥근 모서리/테두리/선택 표시), `--verify-asset-close`로 UI 편집 창 저장 안 함 닫기 무오류, 디버그 레이어 오류 0건

2단계 (17-2 `FGameWorld` 머지 후):
- [ ] 씬 컴포넌트 `FUIComponent`(에셋 경로, Z 순서, 보임) + `RegisterUITypes()` + `FSceneCloner` 런타임 인스턴스 처리, 콘텐츠 브라우저에서 씬으로 끌어 놓기
- [ ] 런타임/플레이 모드 그리기 (씬/포스트 뒤, 에디터 ImGui 앞 — 플레이 뷰포트 오프스크린 타깃 포함, 픽셀 아트 모드는 최종 해상도에)
- [ ] 입력 우선순위: UI가 포인터를 가져가면 게임 `Input`(Lua `Input`)에 전달하지 않음, 키보드 포커스
- [ ] Lua 바인딩: `self:GetUI()`/`UI.Find(entity, "이름")`, 위젯 값(Text/Percent/Visibility/Enabled/색 등), 이벤트 `OnUIClicked_<이름>`/`OnUIHover*`; C++ `IGameModule` 훅(API 버전은 17-8과 맞춤)
- [ ] 검증: 샘플 씬 `Demo_UI.escene`(HUD + 메뉴 버튼 → 스크립트), 런타임/플레이 스크린샷

후속 과제: SDF 굽기는 Debug 빌드에서 한글 글자당 수십 ms(처음 쓸 때만) — 미리 굽기 목록 또는 백그라운드 굽기. 9-slice 브러시, 텍스트 입력 위젯, 애니메이션(UMG 타임라인), 로컬라이즈, 배포용 한글 글꼴(OFL) 번들

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

- [ ] 1. 외부 라이브러리 도입 시험: Recast/Detour와 그래프 편집기 라이브러리를 `ThirdParty.cmake`에 추가하고 `/W4 /WX` 격리, ImGui 1.93 WIP 호환 확인 — 완료 기준: 두 라이브러리 링크 성공, 빈 노드 편집기 창이 뜬다
- [ ] 2. BT 핵심(순수 로직 + 테스트): 에셋 모델 `FBehaviorTreeAsset`(JSON), 블랙보드, 실행기(Selector/Sequence/Parallel(단순), 데코레이터 Blackboard/Cooldown/Loop/TimeLimit/Inverter/ForceSuccess, 서비스, 태스크 Wait/SetBlackboard/Log), 중단 규칙, `FBehaviorTreeNodeRegistry` — 완료 기준: `AITests`에서 가짜 노드로 실행 순서·중단·블랙보드 관찰 검증
- [ ] 3. 내비게이션(순수 로직 + 테스트): `FNavMesh`(Recast 굽기 + Detour 경로 쿼리, 좌표 변환), `.enav` 저장/로드 — 완료 기준: 바닥 + 장애물 지오메트리에서 경로가 장애물을 돌아가는지, 좌표 변환 왕복 테스트
- [ ] 4. 씬 연동: `FBehaviorTreeComponent`/`FNavMeshSettingsComponent` 리플렉션 등록, `FAISystem`(시작/갱신/정지, 에셋 핫 리로드), `MoveTo`/`RotateTo`/`PlayAnimation` 태스크, 플레이 모드·런타임 갱신 순서에 넣기 (Phase 17의 `FGameWorld`가 먼저 들어가 있으면 거기에 넣는다)
- [ ] 5. Lua: Lua 태스크/데코레이터/서비스 노드, `entity:GetBlackboard()`(`Get/Set`), `AI.FindPath`, `AI.MoveTo` — 완료 기준: `ScriptingTests`에 Lua 노드 실행 케이스
- [ ] 6. 게임 모듈 C++ API: C++ 노드 등록, 언로드할 때 모듈이 등록한 노드 제거 (`GameModuleApiVersion`은 Phase 17과 머지 순서에 맞춰 올린다)
- [ ] 7. 에디터: BT 편집 창(`FAssetEditor` 상속, `.ebt` 등록, 노드 팔레트/연결/순서, 블랙보드 키 패널, 노드 속성), 콘텐츠 브라우저 "새 비헤이비어 트리", 플레이 중 실행 노드 강조(선택 엔티티 기준)
- [ ] 8. 에디터 내비메시: 굽기 명령(도구 메뉴), 뷰포트 내비메시·경로 디버그 표시 토글
- [ ] 9. 검증: 샘플 `Demo_AI.escene`(순찰 → 플레이어 발견 → 추적 → 놓치면 복귀, 장애물 우회), Verify 스크린샷, 디버그 레이어 오류 0건
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
