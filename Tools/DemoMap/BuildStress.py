# 성능 기준 씬 Tests/Stress 생성: 스킨 유닛 500개가 동심원 트랙을 돈다 (게임 모듈 StressWalkerComponent) + 정적 그림자 캐스터 소품.
#   실행: python Tools/DemoMap/BuildStress.py — 측정 기준이 바뀌므로 구성을 바꾸면 Plans.md에 새 기준 프레임을 적는다
#   결정적(고정 시드). 유닛 = KayKit 캐릭터 8종(스켈레톤 관절 41, 프리미티브 최대 9) — 걷기/달리기 고리가 번갈아
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")

UNIT_COUNT = 1000
CHARACTERS = ["Knight", "Barbarian", "Mage", "Rogue", "SkeletonMinion", "SkeletonWarrior", "SkeletonRogue", "SkeletonMage"]
WALK_SPEED = 160.0  # cm/s (Walking_A)
RUN_SPEED  = 345.0  # cm/s (Running_A — 블렌드 위치와 같은 발 속도)


def Main():
	Rng = random.Random(500)
	S = FScene()
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.96, 0.9], "Intensity": 3.0}}, (0, 0, 1000), QuatFromEuler(Pitch=-40, Yaw=-30))
	S.Add("SkyLight", {"SkyLightComponent": {"Intensity": 1.0}})
	S.Add("Ground", {"StaticMeshComponent": {"MaterialAsset": "Materials/Checker.emat", "MeshAsset": "primitive:cube"}}, (0, 0, -10), None, (90, 90, 0.2))

	# 유닛: 반지름 400cm부터 120cm 간격 고리, 고리 둘레에 일정 간격(약 140cm)으로 채운다
	Placed = 0
	Ring = 0
	while Placed < UNIT_COUNT:
		Radius = 400.0 + Ring * 120.0
		Count = min(int(2 * math.pi * Radius / 140.0), UNIT_COUNT - Placed)
		bRun = Ring % 2 == 1
		Direction = 1.0 if Ring % 4 < 2 else -1.0
		Phase = Rng.uniform(0, 2 * math.pi)
		for Index in range(Count):
			Angle = Phase + 2 * math.pi * Index / Count
			Character = CHARACTERS[(Ring * 3 + Index) % len(CHARACTERS)]
			Speed = (RUN_SPEED if bRun else WALK_SPEED) * Direction
			S.Model(f"Unit_{Placed:03d}", f"Asset/KayKit/Characters/{Character}.glb",
				(math.cos(Angle) * Radius, math.sin(Angle) * Radius, 0.0), 0.0, 1.0,
				Extra={
					"AnimationComponent": {"BlendTime": 0.25, "Clip": "Running_A" if bRun else "Walking_A", "Loop": True, "Playing": True,
						"Speed": Rng.uniform(0.95, 1.05)},
					"StressWalkerComponent": {"Center": [0.0, 0.0, 0.0], "Speed": Speed, "YawOffset": 180.0},
				})
			Placed += 1
		Ring += 1
	OuterRadius = 400.0 + Ring * 120.0

	# 정적 그림자 캐스터: 바깥 둘레 기둥 + 흩어진 상자/통 무더기 (그림자 캐싱 측정용)
	Props = ["pillar", "column", "barrel_large", "crates_stacked", "rubble_large", "box_stacked"]
	for Index in range(64):
		Angle = 2 * math.pi * Index / 64
		R = OuterRadius + 250.0
		S.Model(f"Pillar_{Index:02d}", f"Asset/KayKit/Dungeon/{'pillar' if Index % 2 == 0 else 'column'}.glb",
			(math.cos(Angle) * R, math.sin(Angle) * R, 0.0), math.degrees(Angle), 1.0)
	for Index in range(150):
		while True:
			X, Y = Rng.uniform(-4300, 4300), Rng.uniform(-4300, 4300)
			if math.hypot(X, Y) > OuterRadius + 600.0:
				break
		S.Model(f"Prop_{Index:03d}", f"Asset/KayKit/Dungeon/{Rng.choice(Props[2:])}.glb", (X, Y, 0.0), Rng.uniform(0, 360), Rng.uniform(0.9, 1.3))

	S.Add("Camera", {"CameraComponent": {"Primary": True, "FovYDegrees": 60.0, "NearZ": 10.0, "FarZ": 100000.0}},
		(-OuterRadius - 900.0, -OuterRadius * 0.6, 1500.0), QuatFromEuler(Pitch=-28, Yaw=30))
	Path = os.path.join(CONTENT, "Scenes", "Tests", "Stress.escene")
	S.Save(Path)
	print(f"Stress 생성: 유닛 {Placed}, 고리 {Ring}, 바깥 반지름 {OuterRadius:.0f}cm, 엔티티 {len(S.Entities)}")


if __name__ == "__main__":
	Main()
