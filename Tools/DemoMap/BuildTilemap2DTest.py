# Tests/Tilemap2D 확인 씬 생성 (Phase 56 연결 — 타일맵 충돌 → 2D 물리, 플립북 이벤트 → Lua).
#   python Tools/DemoMap/BuildTilemap2DTest.py  →  Projects/Sample/Content/Scenes/Tests/Tilemap2D.escene
# 타일맵: 샘플 타일셋(Sprites/Samples/SampleTiles.etileset — 0~2 Full, 3 경사 다각형, 6 Full 원웨이) 셀 50cm.
#   바닥 줄(y = -1, 위 = Z 0) + 양쪽 벽 + 원웨이 발판 + 받침 발판(스크립트가 2초에 지워 위 상자가 떨어진다) + 경사.
# 타일맵은 아직 그려지지 않으므로(스프라이트 렌더링 후속) 같은 자리에 충돌 없는 안내 큐브를 둔다.
# TileData 인코딩은 Engine/Source/Scene/Sprite/TilemapData.h 머리 주석과 같다 (base64(버전 1 | 청크 수 | (Y, X) 정렬 청크 RLE)).
import base64
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

Root = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
Output = os.path.join(Root, "Projects", "Sample", "Content", "Scenes", "Tests", "Tilemap2D.escene")

CellSize = 50.0
ChunkSize = 32


def MakeCell(TileId, FlipX=False, FlipY=False, Rotate90=False):
	return (TileId + 1) | (1 << 29 if FlipX else 0) | (1 << 30 if FlipY else 0) | (1 << 31 if Rotate90 else 0)


def EncodeTilemap(Cells):
	# Cells: {(x, y): 셀 값}. 청크 키 = (y >> 5, x >> 5) 오름차순, 청크 안 (y, x) 행 우선 RLE
	Chunks = {}
	for (X, Y), Value in Cells.items():
		if Value == 0:
			continue
		Key = (Y >> 5, X >> 5)
		Chunk = Chunks.setdefault(Key, [0] * (ChunkSize * ChunkSize))
		Chunk[((Y & 31) << 5) | (X & 31)] = Value
	if not Chunks:
		return ""
	Data = bytearray(struct.pack("<BI", 1, len(Chunks)))
	for (ChunkY, ChunkX) in sorted(Chunks.keys()):
		Chunk = Chunks[(ChunkY, ChunkX)]
		Data += struct.pack("<ii", ChunkX, ChunkY)
		Index = 0
		while Index < len(Chunk):
			Run = 1
			while Index + Run < len(Chunk) and Chunk[Index + Run] == Chunk[Index]:
				Run += 1
			Data += struct.pack("<HI", Run, Chunk[Index])
			Index += Run
	return base64.b64encode(bytes(Data)).decode("ascii")


def Main():
	Scene = FScene()
	Scene.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.96, 0.9], "Intensity": 2.5}}, (0, 0, 800),
	          [-0.4030582, -0.1270838, 0.8643611, -0.272532])
	Scene.Add("SkyLight", {"SkyLightComponent": {"Intensity": 1.0}})
	# 2D 카메라: +Y 쪽에서 -Y를 본다 (yaw -90), 직교
	Scene.Add("Camera2D", {"CameraComponent": {"FovYDegrees": 60.0, "NearZ": 10.0, "FarZ": 20000.0, "Primary": True, "Priority": 0,
	                                          "Orthographic": True, "OrthoHeight": 900.0}},
	          (0, 2000, 300), QuatFromEuler(Yaw=-90.0))

	# ---- 타일 배치 (셀 좌표)
	Cells = {}
	Guides = []  # (이름, 셀 사각형 MinX, MinY, MaxX, MaxY, 머티리얼)

	def Fill(MinX, MinY, MaxX, MaxY, TileId, Material, Name):
		for Y in range(MinY, MaxY + 1):
			for X in range(MinX, MaxX + 1):
				Cells[(X, Y)] = MakeCell(TileId)
		Guides.append((Name, MinX, MinY, MaxX, MaxY, Material))

	Fill(-16, -2, 15, -1, 1, "Materials/Dirt.emat", "GuideGround")      # 바닥 두 줄 (위 = Z 0)
	Fill(-16, 0, -16, 10, 2, "Materials/Checker.emat", "GuideWallL")    # 왼쪽 벽
	Fill(15, 0, 15, 10, 2, "Materials/Checker.emat", "GuideWallR")      # 오른쪽 벽
	Fill(2, 5, 7, 5, 6, "Materials/Yellow.emat", "GuideOneWay")         # 원웨이 발판 (Z 250 ~ 300)
	Fill(-11, 4, -7, 4, 0, "Materials/Grass.emat", "GuideLedge")        # 받침 발판 (Z 200 ~ 250) — 스크립트가 -10 ~ -8을 지운다
	Guides[-1] = ("GuideLedgeL", -11, 4, -11, 4, "Materials/Grass.emat")  # 안내 큐브는 지운 뒤 남는 양 끝만
	Guides.append(("GuideLedgeR", -7, 4, -7, 4, "Materials/Grass.emat"))
	# 경사 (타일 3 = 오른쪽으로 오르는 직각 삼각형) — 오른쪽 벽 앞 계단식
	for Index in range(3):
		Cells[(12 + Index, Index)] = MakeCell(3)
		for Below in range(Index):
			Cells[(12 + Index, Below)] = MakeCell(1)
	Guides.append(("GuideSlope", 12, 0, 14, 2, "Materials/Purple.emat"))

	Scene.Add("Tilemap", {"TilemapComponent": {"Tileset": "Sprites/Samples/SampleTiles.etileset", "CellSize": [CellSize, CellSize],
	                                           "Collision": True, "TileData": EncodeTilemap(Cells)}}, (0, 0, 0))

	# 안내 큐브 (충돌 없음 — 타일 자리 표시, 깊이 -40으로 상자 뒤)
	for (Name, MinX, MinY, MaxX, MaxY, Material) in Guides:
		Width = (MaxX - MinX + 1) * CellSize
		Height = (MaxY - MinY + 1) * CellSize
		Center = ((MinX * CellSize + Width * 0.5), -40.0, (MinY * CellSize + Height * 0.5))
		Scale = (Width / 100.0, 0.2, Height / 100.0)
		if Name == "GuideSlope":
			# 경사면 (셀 대각선이 이어진 45도 선) — 얇은 판을 +Pitch(오른쪽이 위)로 기울인다
			Scale = (Width * 1.41421356 / 100.0, 0.2, 0.05)
			Rotation = QuatFromEuler(Pitch=45.0)
		else:
			Rotation = None
		Scene.Add(Name, {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": Material, "Visible": True}},
		          Center, Rotation, Scale)

	# 떨어지는 2D 상자 (60cm, 보이는 건 큐브)
	def Box(Name, X, Z, Material):
		Scene.Add(Name, {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": Material, "Visible": True},
		                 "BoxCollider2DComponent": {"Size": [100.0, 100.0]},
		                 "RigidBody2DComponent": {"BodyType": 2, "FixedRotation": True}},
		          (X, 0, Z), None, (0.6, 0.6, 0.6))

	Box("BoxGround", -100.0, 600.0, "Materials/Blue.emat")   # 바닥 위 Z 30
	Box("BoxOneWay", 225.0, 700.0, "Materials/Green.emat")   # 원웨이 발판 위 Z 330
	Box("BoxLedge", -425.0, 650.0, "Materials/Orange.emat")  # 받침 위 Z 280 → 2초에 받침이 지워져 바닥 Z 30

	# 플립북 (샘플 보석, PingPong 8fps, 프레임 1 이벤트 Sparkle) + 이벤트·결과 로그 스크립트
	Scene.Add("Gem", {"SpriteComponent": {"Sprite": "Sprites/Samples/SampleAtlas.esprite", "Size": [80.0, 0.0]},
	                  "FlipbookComponent": {"Flipbook": "Sprites/Samples/SampleGems.eflipbook"},
	                  "ScriptComponent": {"ScriptAsset": "Scripts/Tests/Tilemap2DTest.lua",
	                                      "PropertyOverrides": "{\"HoleMinX\":-10,\"HoleMaxX\":-8,\"HoleY\":4}"}},
	          (0, 0, 500))

	Scene.Save(Output)
	print(f"씬 저장: {Output} (타일 {len(Cells)}개)")


if __name__ == "__main__":
	Main()
