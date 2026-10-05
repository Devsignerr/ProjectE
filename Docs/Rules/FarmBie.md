# FarmBie 예제 프로젝트 (농장 경영 + 좀비 디펜스)

> CLAUDE.md에서 옮긴 상세 규칙이다. 이 영역의 코드·에셋을 고치기 전에 읽는다. 기획 결정은 `Projects/FarmBie/결정사항.md`, 작업 단계는 Plans.md "FarmBie".

- 콘텐츠는 생성물: 씬·프리팹·스프라이트·지형·머티리얼·바닥 텍스처는 `python Projects/FarmBie/Tools/BuildFarmBie.py`(도트 아트 `FarmBieArt.py`)를 고쳐 다시 만든다. 범용 도구(SceneBuilder·HD2DArt의 FCanvas/FAtlas/WriteFlipbook·ModelBounds)는 `Tools/DemoMap`에서 가져다 쓴다
- 좌표·카메라: 거의 수직 탑뷰(피치 -70°, 시야각 30°) — 화면 위 = -Y. 캐릭터 스프라이트는 **전체 빌보드(Billboard 1)**(세로축 빌보드는 이 각도에서 납작해 보임), 1 도트 = 6cm, 피벗 = 발
- 농장 격자: 칸 100cm, 52×40, 원점 (-2600, -2000) — 밭·건물·벽·좀비 흐름장이 모두 이 격자를 쓴다 (`BuildFarmBie.py` 상수 = 게임 코드 상수)
- 로직 분담: 매 프레임 대량 처리(격자·좀비 이동·흐름장·크리스탈 감지·설치물)는 게임 모듈 `Projects/FarmBie/Source`(FarmBieGame.dll), 하루 진행·상점·UI 흐름은 Lua `Content/Scripts/FarmBie`
- 시간(FarmTime.lua, 관리자에 섞임): 하루 = 낮 6→20시(실제 8분) + 밤 20→26시(실제 3.5분 = 디펜스 제한 시간) → 잠(문 앞 상호작용 또는 새벽 2시) → 날짜 넘김·자동 저장. 수치는 `Data/FarmBie/Calendar.edata`(생성기 `FarmBieData.py`) — 스크립트 상수 금지. 하늘 모델은 18시쯤 해가 지므로 화면은 **게임 시각 → 하늘 시각**(`GetSkyHour`, Calendar Sky*)으로 바꿔 쓰고 `DayNightKeys.etable` 열쇠 시각도 하늘 시각이다
- 관리자 확장 규약: 새 모듈은 `FarmGame.lua`의 모듈 목록에 넣고, 저장할 상태는 `Save<이름>/Load<이름>` 쌍 + `SaveParts`에 이름 추가, 시간 훅 `OnNightStart`/`OnDayStart(bNewSeason)`/`OnSeasonChanged(Old)`, 상호작용은 `AddInteractable{Pos, Radius, Prompt, Act}`
- 자동 검증: `.\Scripts\Verify.ps1 -Target Runtime -Config Release -Frames N -ExtraArgs "--project Projects/FarmBie --scene Scenes/Tests/<씬>.escene --fixed-delta 60"` → 로그 `[FarmBie] 결과: 실패 0건`. `FarmAutoPlay`(N 1500 — 이동·플립북·구르기·카메라·충돌), `FarmTime`(N 2400 — 시계 속도·밤·등불·잠·저장·계절 경고/넘김·불러오기·연도). 확인용 시각별 화면: `BuildFarmBie.py --views` → `Scenes/_FarmShot<시각>.escene`(커밋 안 함)
