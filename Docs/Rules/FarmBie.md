# FarmBie 예제 프로젝트 (농장 경영 + 좀비 디펜스)

> CLAUDE.md에서 옮긴 상세 규칙이다. 이 영역의 코드·에셋을 고치기 전에 읽는다. 기획 결정은 `Projects/FarmBie/결정사항.md`, 작업 단계는 Plans.md "FarmBie".

- 콘텐츠는 생성물: 씬·프리팹·스프라이트·지형·머티리얼·바닥 텍스처는 `python Projects/FarmBie/Tools/BuildFarmBie.py`(도트 아트 `FarmBieArt.py`)를 고쳐 다시 만든다. 범용 도구(SceneBuilder·HD2DArt의 FCanvas/FAtlas/WriteFlipbook·ModelBounds)는 `Tools/DemoMap`에서 가져다 쓴다
- 좌표·카메라: 거의 수직 탑뷰(피치 -70°, 시야각 30°) — 화면 위 = -Y. 캐릭터 스프라이트는 **전체 빌보드(Billboard 1)**(세로축 빌보드는 이 각도에서 납작해 보임), 1 도트 = 6cm, 피벗 = 발
- 농장 격자: 칸 100cm, 52×40, 원점 (-2600, -2000) — 밭·건물·벽·좀비 흐름장이 모두 이 격자를 쓴다 (`BuildFarmBie.py` 상수 = 게임 코드 상수)
- 로직 분담: 매 프레임 대량 처리(격자·좀비 이동·흐름장·크리스탈 감지·설치물)는 게임 모듈 `Projects/FarmBie/Source`(FarmBieGame.dll), 하루 진행·상점·UI 흐름은 Lua `Content/Scripts/FarmBie`
- 자동 검증: `.\Scripts\Verify.ps1 -Target Runtime -Config Release -Frames N -ExtraArgs "--project Projects/FarmBie --scene Scenes/Tests/<씬>.escene --fixed-delta 60"` → 로그 `[FarmBie] 결과: 실패 0건`. `FarmAutoPlay`(N 1500 — 이동·플립북·구르기·카메라·충돌)
