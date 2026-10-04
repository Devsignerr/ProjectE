# Crypt2D 크레딧

모든 외부 에셋은 CC0(퍼블릭 도메인)이다. 표기는 의무가 아니지만 감사의 뜻으로 남긴다.

## 그래픽 — ansimuz (Luis Zuno)

- **GothicVania Church Pack** — https://opengameart.org/content/gothicvania-church-pack (CC0)
  - 수도승 플레이어, 타락한 사제(마법사), 불타는 구울, 천사(보스), 적 사망 폭발, 불덩이, 교회 타일셋, 장식 판·기둥
- **GothicVania Cemetery Pack** — https://opengameart.org/content/gothicvania-cemetery-pack (CC0)
  - 해골 전사(솟아남 포함), 망령, 지옥 고양이, 불꽃, 묘지 소품, 타이틀 배경(하늘·산·묘지)
  - 팩에 들어 있는 음악(Pascal Belisle)은 별도 라이선스(출처 표기 조건)이라 받지도 쓰지도 않는다
- 원본은 저장소에 넣지 않는다 — `Scripts/FetchAssets.ps1`이 잠금 파일 `Scripts/Assets.json`(URL·MD5)대로 받아
  `Content/Asset/GothicVania/`에 푼다 (gitignore). 지원: https://www.patreon.com/ansimuz

## 효과음

- **Kenney** — RPG Audio, Interface Sounds (CC0, https://www.kenney.nl): 코인, 상자, 장착, 회복, UI, 문
- Projects/Sample의 RPG 데모 효과음(휘두르기·타격·피격·대시 — 프로젝트 자체 절차 생성)
- 점프·석궁·불덩이·폭발·포털·보스 포효·문 — `Tools/Crypt2DArt.py`가 합성 (자체 제작)

## 자체 제작

- 무기(검·도끼·창·석궁·지팡이), 코인, 하트, 상자, 휘두르기 호, 불꽃 튐, 먼지, 화살, 보스 탄, 포털, 횃불 빛무리, 조준점 —
  `Tools/Crypt2DArt.py`가 그린다 (`Content/Sprites/Crypt/Generated/`, UI 아이콘은 4배 확대본)
- 방 템플릿·게임 코드·UI·데이터 — ProjectE
