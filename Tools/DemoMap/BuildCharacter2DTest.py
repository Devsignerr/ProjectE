# Tests/Character2D 확인 씬 생성 (Phase 56 후속 — 2D 캐릭터 이동기, Physics/CharacterMovement2D.h).
#   python Tools/DemoMap/BuildCharacter2DTest.py  →  Projects/Sample/Content/Scenes/Tests/Character2D.escene
#                                                   + Projects/Sample/Content/Prefabs/Tests/Character2DPlayer.eprefab (씬 전용 멀티플레이 폰)
# 타일맵: 샘플 타일셋(Sprites/Samples/SampleTiles.etileset — 0~2 Full, 3 45도 경사, 6 Full 원웨이) 셀 50cm.
#   바닥(위 = Z 0) + 양쪽 벽 + 원웨이 발판(셀 -4~2, 윗면 Z 300) + 45도 오르막(셀 20~23) → 고원(윗면 Z 200) + 65도 경사(다각형 콜라이더, X 1600).
# 캐릭터 Hero(서버 소유, 자동 입력 Scripts/Tests/Character2DPlayer.lua — 단계마다 [Character2D] 로그)와 멀티플레이 폰 프리팹(같은 스크립트, Both).
# 타일맵은 아직 그려지지 않으므로(스프라이트 렌더링은 Renderer 트랙) 같은 자리에 충돌 없는 안내 큐브를 둔다. 캐릭터는 큐브로 보인다.
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402
from BuildTilemap2DTest import CellSize, EncodeTilemap, MakeCell  # noqa: E402

Root = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
Content = os.path.join(Root, "Projects", "Sample", "Content")
Output = os.path.join(Content, "Scenes", "Tests", "Character2D.escene")
PrefabOutput = os.path.join(Content, "Prefabs", "Tests", "Character2DPlayer.eprefab")
PrefabAsset = "Prefabs/Tests/Character2DPlayer.eprefab"
ScriptAsset = "Scripts/Tests/Character2DPlayer.lua"

# 캐릭터 (반지름 30, 높이 120 — 큐브 60 × 120으로 보인다)
Movement = {"CapsuleRadius": 30.0, "CapsuleHeight": 120.0}
CharacterMesh = {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Orange.emat", "Visible": True}
CharacterScale = (0.6, 0.6, 1.2)


def WritePrefab():
	Entity = {
		"Name": "Player2D",
		"Parent": -1,
		"Components": {
			"PrefabLinkComponent": {"Id": "1", "Root": -1},
			"TransformComponent": {"Position": [0.0, 0.0, 0.0], "Rotation": [0.0, 0.0, 0.0, 1.0], "Scale": list(CharacterScale)},
			"StaticMeshComponent": dict(CharacterMesh, MaterialAsset="Materials/Blue.emat"),
			"CharacterMovement2DComponent": dict(Movement),
			"ReplicatedComponent": {},
			"ScriptComponent": {"ExecutionLocation": 2, "ScriptAsset": ScriptAsset,
			                    "PropertyOverrides": "{\"AutoInput\":true,\"Label\":\"Player\"}"},
		},
	}
	os.makedirs(os.path.dirname(PrefabOutput), exist_ok=True)
	with open(PrefabOutput, "w", encoding="utf-8", newline="\n") as File:
		json.dump({"Entities": [Entity]}, File, ensure_ascii=False, indent=2)
		File.write("\n")


def Main():
	Scene = FScene()
	Scene.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.96, 0.9], "Intensity": 2.5}}, (0, 0, 800),
	          [-0.4030582, -0.1270838, 0.8643611, -0.272532])
	Scene.Add("SkyLight", {"SkyLightComponent": {"Intensity": 1.0}})
	# 2D 카메라: +Y 쪽에서 -Y를 본다 (yaw -90), 직교 — 전체(X -1000 ~ 2400)가 보이게
	Scene.Add("Camera2D", {"CameraComponent": {"FovYDegrees": 60.0, "NearZ": 10.0, "FarZ": 20000.0, "Primary": True, "Priority": 0,
	                                          "Orthographic": True, "OrthoHeight": 1300.0}},
	          (650, 3000, 400), QuatFromEuler(Yaw=-90.0))
	# 씬 전용 멀티플레이 폰 (프로젝트 설정 플레이어 프리팹 대신)
	Scene.Add("GameMode", {"GameModeComponent": {"PlayerPrefab": PrefabAsset, "RespawnDelay": -1.0}})
	Scene.Add("PlayerStart", {}, (-500, 0, 100))  # Hero와 같은 자리 (캐릭터끼리는 통과) — 같은 시간표 기대값

	Cells = {}
	Guides = []  # (이름, 셀 사각형 MinX, MinY, MaxX, MaxY, 머티리얼)

	def Fill(MinX, MinY, MaxX, MaxY, TileId, Material, Name):
		for Y in range(MinY, MaxY + 1):
			for X in range(MinX, MaxX + 1):
				Cells[(X, Y)] = MakeCell(TileId)
		if Name is not None:
			Guides.append((Name, MinX, MinY, MaxX, MaxY, Material))

	Fill(-20, -2, 47, -1, 1, "Materials/Dirt.emat", "GuideGround")      # 바닥 (위 = Z 0)
	Fill(-20, 0, -20, 12, 2, "Materials/Checker.emat", "GuideWallL")    # 왼쪽 벽
	Fill(47, 0, 47, 12, 2, "Materials/Checker.emat", "GuideWallR")      # 오른쪽 벽
	Fill(-4, 5, 2, 5, 6, "Materials/Yellow.emat", "GuideOneWay")        # 원웨이 발판 (Z 250 ~ 300)
	# 45도 오르막 (타일 3 = 오른쪽으로 오르는 직각 삼각형, 아래는 Full) → 고원 셀 24~34 (윗면 Z 200)
	for Index in range(4):
		Cells[(20 + Index, Index)] = MakeCell(3)
		for Below in range(Index):
			Cells[(20 + Index, Below)] = MakeCell(1)
	Guides.append(("GuideSlope", 20, 0, 23, 3, "Materials/Purple.emat"))
	Fill(24, 0, 46, 3, 1, "Materials/Grass.emat", "GuidePlateau")

	Scene.Add("Tilemap", {"TilemapComponent": {"Tileset": "Sprites/Samples/SampleTiles.etileset", "CellSize": [CellSize, CellSize],
	                                           "Collision": True, "TileData": EncodeTilemap(Cells)}}, (0, 0, 0))

	for (Name, MinX, MinY, MaxX, MaxY, Material) in Guides:
		Width = (MaxX - MinX + 1) * CellSize
		Height = (MaxY - MinY + 1) * CellSize
		Center = ((MinX * CellSize + Width * 0.5), -40.0, (MinY * CellSize + Height * 0.5))
		Scale = (Width / 100.0, 0.2, Height / 100.0)
		Rotation = None
		if Name == "GuideSlope":
			Scale = (Width * 1.41421356 / 100.0, 0.2, 0.05)
			Rotation = QuatFromEuler(Pitch=45.0)
		Scene.Add(Name, {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": Material, "Visible": True}},
		          Center, Rotation, Scale)

	# 65도 경사 (고원 위, 다각형 콜라이더 — 바닥이 아니라 미끄러지는 면). 안내 큐브는 기울인 판
	Scene.Add("SteepSlope", {"PolygonCollider2DComponent": {"Points": "0,0; 100,0; 100,214.45"}}, (1600, 0, 200))
	Scene.Add("GuideSteep", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Green.emat", "Visible": True}},
	          (1650, -40.0, 307.2), QuatFromEuler(Pitch=65.0), (2.366, 0.2, 0.05))

	# 서버 소유 캐릭터 (Standalone 자동 입력 — 멀티플레이에서는 서버가 조종하고 클라이언트는 복제 위치를 본다)
	Scene.Add("Hero", {"StaticMeshComponent": dict(CharacterMesh),
	                   "CharacterMovement2DComponent": dict(Movement),
	                   "ReplicatedComponent": {},
	                   "ScriptComponent": {"ExecutionLocation": 2, "ScriptAsset": ScriptAsset,
	                                       "PropertyOverrides": "{\"AutoInput\":true,\"Label\":\"Hero\"}"}},
	          (-500, 0, 100), None, CharacterScale)

	Scene.Save(Output)
	WritePrefab()
	print(f"씬 저장: {Output} (타일 {len(Cells)}개), 프리팹: {PrefabOutput}")


if __name__ == "__main__":
	Main()
