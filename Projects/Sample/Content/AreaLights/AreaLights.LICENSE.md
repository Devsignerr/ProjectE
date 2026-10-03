# AreaLights 데모 에셋 (Phase 52)

이 폴더의 파일은 모두 ProjectE에서 스크립트로 직접 생성한 합성 데이터이며 외부 출처가 없다. CC0 1.0 (퍼블릭 도메인)으로 배포한다.

| 파일 | 내용 |
| --- | --- |
| `Downlight_Narrow.ies` | IESNA LM-63-2002, 회전 대칭 좁은 빔(가우시안 반각 약 16°) + 약한 퍼짐. 최대 2460cd |
| `Linear_Batwing.ies` | LM-63-2002, 4분면 대칭(수평 0/22.5/45/67.5/90) 선형 형광등 배트윙(가로 방향 38° 봉우리) |
| `Wallwash_Asym.ies` | LM-63-2002, 수평 0..360(30° 간격) 비대칭 월 워셔 (φ = 0 쪽으로 던짐) |
| `WindowBars.png` | 창틀 쿠키 (테두리 + 3×2 창살, 흰 바탕) |
| `Flicker.png` | 깜빡임 띠 (256×4, 쿠키 배율 0 + 가로 패닝으로 빛 전체 밝기를 바꾼다) |
| `CloudShadow.png` | 방향광 구름 그림자 쿠키 (반복 가능한 값 잡음) |
| `*.emat` | 데모 머티리얼 (발광 패널, 금속/거친 바닥, 벽) |

LTC 피팅 표는 엔진 소스(`Engine/Source/Renderer/LtcTables.cpp`, BSD 라이선스 `LtcTables.LICENSE.txt`)에 있다.
