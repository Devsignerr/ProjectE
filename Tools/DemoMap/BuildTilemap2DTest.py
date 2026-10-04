# Tests/Tilemap2D 확인 씬 생성 (Phase 56 — 타일맵 충돌 → 2D 물리, 플립북 이벤트 → Lua, 스프라이트·타일맵 렌더링 56-4b).
#   python Tools/DemoMap/BuildTilemap2DTest.py            →  Scenes/Tests/Tilemap2D.escene (+ Tilemap2DPixelArt.escene: 같은 맵 + PixelArtComponent)
#   python Tools/DemoMap/BuildTilemap2DTest.py --stress   →  Scenes/Tests/Tilemap2DStress.escene (성능: 스프라이트 1만 + 타일 10만 셀)
# 타일맵: 샘플 타일셋(Sprites/Samples/SampleTiles.etileset — 0 풀/1 흙/2 돌 Full, 3 경사 다각형, 4~5 물 애니메이션, 6 Full 원웨이) 셀 50cm.
#   바닥 줄(y = -1, 위 = Z 0) + 양쪽 벽 + 원웨이 발판 + 받침 발판(스크립트가 2초에 지워 위 상자가 떨어진다) + 경사(오른쪽 = 타일 3,
#   왼쪽 = 같은 타일 FlipX — 충돌도 뒤집힘) + 물웅덩이(애니메이션 타일) + 회전/반전 타일 줄(돌 타일 Rotate90·FlipY — 그리기 확인용, 충돌 없음 줄).
# 카메라: +Y에서 -Y를 보는 직교, OrthoHeight 1125 → 1280x720에서 1cm = 0.64px → 50cm 셀(16px 타일) = 32px = 텍셀당 정확히 2px
#   (셀 경계·카메라 중심이 정수 픽셀 — 도트가 고르게 보인다). 스프라이트(34px 아이콘)는 68cm = 텍셀당 2px.
# TileData 인코딩은 Engine/Source/Scene/Sprite/TilemapData.h 머리 주석과 같다 (base64(버전 1 | 청크 수 | (Y, X) 정렬 청크 RLE)).
import base64
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

Root = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
SceneDir = os.path.join(Root, "Projects", "Sample", "Content", "Scenes", "Tests")

CellSize = 50.0
ChunkSize = 32
SpriteSize = 68.0  # 34px 아이콘 × 2
Atlas = "Sprites/Samples/SampleAtlas.esprite"
Tileset = "Sprites/Samples/SampleTiles.etileset"


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


def AddCommon(Scene, OrthoHeight=1125.0, CameraPosition=(0, 2000, 300)):
	Scene.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.96, 0.9], "Intensity": 2.5}}, (0, 0, 800),
	          [-0.4030582, -0.1270838, 0.8643611, -0.272532])
	Scene.Add("SkyLight", {"SkyLightComponent": {"Intensity": 1.0}})
	# 2D 카메라: +Y 쪽에서 -Y를 본다 (yaw -90), 직교
	Scene.Add("Camera2D", {"CameraComponent": {"FovYDegrees": 60.0, "NearZ": 10.0, "FarZ": 20000.0, "Primary": True, "Priority": 0,
	                                          "Orthographic": True, "OrthoHeight": OrthoHeight}},
	          CameraPosition, QuatFromEuler(Yaw=-90.0))


def BuildTestScene(bPixelArt):
	Scene = FScene()
	AddCommon(Scene)
	if bPixelArt:
		# 저해상도 렌더(도트 = 2px → 텍셀 1개 = 도트 1개) + 외곽선. 직교 카메라 도트 스냅
		Scene.Add("PixelArt", {"PixelArtComponent": {"Enabled": True, "PixelSize": 2, "OutlineStrength": 0.6, "HighlightStrength": 0.35}})

	# ---- 타일 배치 (셀 좌표)
	Cells = {}

	def Fill(MinX, MinY, MaxX, MaxY, TileId, **Flags):
		for Y in range(MinY, MaxY + 1):
			for X in range(MinX, MaxX + 1):
				Cells[(X, Y)] = MakeCell(TileId, **Flags)

	Fill(-16, -2, 15, -2, 1)       # 흙 (Z -100 ~ -50)
	Fill(-16, -1, 15, -1, 0)       # 풀 바닥 (위 = Z 0)
	Fill(5, -1, 9, -1, 4)          # 물웅덩이 (애니메이션 타일 4 ↔ 5, 충돌 없음 — 흙 줄이 받친다)
	Fill(-16, 0, -16, 10, 2)       # 왼쪽 벽
	Fill(15, 0, 15, 10, 2)         # 오른쪽 벽
	Fill(2, 5, 7, 5, 6)            # 원웨이 발판 (Z 250 ~ 300)
	Fill(-11, 4, -7, 4, 0)         # 받침 발판 (Z 200 ~ 250) — 스크립트가 -10 ~ -8을 지운다
	# 경사: 오른쪽 벽 앞은 타일 3(오른쪽으로 오름), 왼쪽 벽 앞은 같은 타일 FlipX(왼쪽으로 오름)
	for Index in range(3):
		Cells[(12 + Index, Index)] = MakeCell(3)
		Cells[(-13 - Index, Index)] = MakeCell(3, FlipX=True)
		for Below in range(Index):
			Cells[(12 + Index, Below)] = MakeCell(1)
			Cells[(-13 - Index, Below)] = MakeCell(1)
	# 회전/반전 확인 줄 (상자 시작 높이보다 위, Z 750 ~ 800): 경사 타일을 플래그 8조합으로 — 그리기 확인용 (충돌은 다각형도 같이 뒤집힌다)
	for Mask in range(8):
		Cells[(-4 + Mask, 15)] = MakeCell(3, FlipX=bool(Mask & 1), FlipY=bool(Mask & 2), Rotate90=bool(Mask & 4))

	Scene.Add("Tilemap", {"TilemapComponent": {"Tileset": Tileset, "CellSize": [CellSize, CellSize],
	                                           "Collision": True, "TileData": EncodeTilemap(Cells)}}, (0, 0, 0))

	# 떨어지는 2D 상자 (스프라이트 68cm, 콜라이더 = 스프라이트 크기 — 깊이 Y 10: 타일보다 카메라 쪽)
	def Box(Name, X, Z, Slice):
		Scene.Add(Name, {"SpriteComponent": {"Sprite": Atlas, "Slice": Slice, "Size": [SpriteSize, SpriteSize]},
		                 "BoxCollider2DComponent": {"Size": [SpriteSize, SpriteSize]},
		                 "RigidBody2DComponent": {"BodyType": 2, "FixedRotation": True}},
		          (X, 10, Z))

	Box("BoxGround", -100.0, 600.0, "PotionBlue")  # 바닥 위 Z 34
	Box("BoxOneWay", 225.0, 700.0, "PotionRed")    # 원웨이 발판 위 Z 334
	Box("BoxLedge", -425.0, 650.0, "Fire")         # 받침 위 Z 284 → 2초에 받침이 지워져 바닥 Z 34

	# 반전 스프라이트 (좌우·상하) + 위 레이어 순번 확인: 같은 자리 두 장 중 OrderInLayer 큰 것이 위
	Scene.Add("SpriteFlipX", {"SpriteComponent": {"Sprite": Atlas, "Slice": "Wind", "Size": [SpriteSize, SpriteSize], "FlipX": True}},
	          (-250, 10, 34))
	Scene.Add("SpriteFlipY", {"SpriteComponent": {"Sprite": Atlas, "Slice": "Wind", "Size": [SpriteSize, SpriteSize], "FlipY": True}},
	          (-170, 10, 34))
	Scene.Add("SpriteBack", {"SpriteComponent": {"Sprite": Atlas, "Slice": "Crystal", "Size": [SpriteSize, SpriteSize], "OrderInLayer": 0}},
	          (400, 10, 34))
	Scene.Add("SpriteFront", {"SpriteComponent": {"Sprite": Atlas, "Slice": "Ruby", "Size": [SpriteSize, SpriteSize], "OrderInLayer": 1,
	                                              "Color": [1.0, 1.0, 1.0, 0.85]}},
	          (434, 10, 54))

	# 플립북 (샘플 보석, PingPong 8fps, 프레임 1 이벤트 Sparkle) + 이벤트·결과 로그 스크립트
	Scene.Add("Gem", {"SpriteComponent": {"Sprite": Atlas, "Size": [SpriteSize, 0.0]},
	                  "FlipbookComponent": {"Flipbook": "Sprites/Samples/SampleGems.eflipbook"},
	                  "ScriptComponent": {"ScriptAsset": "Scripts/Tests/Tilemap2DTest.lua",
	                                      "PropertyOverrides": "{\"HoleMinX\":-10,\"HoleMaxX\":-8,\"HoleY\":4}"}},
	          (0, 10, 500))

	Name = "Tilemap2DPixelArt.escene" if bPixelArt else "Tilemap2D.escene"
	Scene.Save(os.path.join(SceneDir, Name))
	print(f"씬 저장: {Name} (타일 {len(Cells)}개)")


def BuildStressScene(SpriteCount=10000, TileWidth=400, TileHeight=250):
	# 성능 측정: 타일 400 x 250 = 10만 셀(청크 13 x 8 = 104개, 돌·흙·풀 무늬 + 물 애니메이션 셀 약 1%) + 스프라이트 1만(아이콘 8종 격자).
	# 카메라는 맵 전체를 본다 (컬링 없이 모두 그림 — 최악)
	Scene = FScene()
	Width = TileWidth * CellSize
	Height = TileHeight * CellSize
	AddCommon(Scene, OrthoHeight=Height * 1.05, CameraPosition=(Width * 0.5, 2000, Height * 0.5))
	Cells = {}
	for Y in range(TileHeight):
		for X in range(TileWidth):
			Value = (X * 7 + Y * 13) % 17
			if (X * 31 + Y * 17) % 97 == 0:
				Cells[(X, Y)] = MakeCell(4)  # 물 (애니메이션 — 프레임마다 항목)
			else:
				Cells[(X, Y)] = MakeCell(Value % 3, FlipX=(Value & 4) != 0, Rotate90=(Value & 8) != 0)
	Scene.Add("Tilemap", {"TilemapComponent": {"Tileset": Tileset, "CellSize": [CellSize, CellSize], "Collision": False,
	                                           "TileData": EncodeTilemap(Cells)}}, (0, 0, 0))
	Slices = ["PotionBlue", "PotionRed", "PotionRed2", "Coin", "Ruby", "Crystal", "Fire", "Wind"]
	Columns = 125
	Rows = (SpriteCount + Columns - 1) // Columns
	StepX = Width / Columns
	StepZ = Height / Rows
	for Index in range(SpriteCount):
		Column = Index % Columns
		Row = Index // Columns
		Scene.Add(f"S{Index}", {"SpriteComponent": {"Sprite": Atlas, "Slice": Slices[Index % len(Slices)], "Size": [SpriteSize * 1.5, SpriteSize * 1.5],
		                                            "OrderInLayer": Index % 3}},
		          ((Column + 0.5) * StepX, 10, (Row + 0.5) * StepZ))
	Scene.Save(os.path.join(SceneDir, "Tilemap2DStress.escene"))
	print(f"씬 저장: Tilemap2DStress.escene (타일 {len(Cells)}개, 스프라이트 {SpriteCount}개)")


def Main():
	if "--stress" in sys.argv:
		BuildStressScene()
		return
	BuildTestScene(False)
	BuildTestScene(True)


if __name__ == "__main__":
	Main()
