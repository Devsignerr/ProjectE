# FarmBie 설치물 3D 모델 (절차 메시 glTF — Tools/DemoMap/HD2DMeshKit). BuildFarmBie.py가 부른다. 결과: Content/Asset/FarmBie/Build/<이름>.gltf(+.bin)
#   원점 = 칸 가운데 바닥, 엔진 좌표(cm, 왼손 Z-up)로 짓는다. 한 칸 = 100cm. 벽·문은 X축으로 길다(회전 0) — 설치할 때 Yaw 0/90.
#   KayKit 저폴리 색감에 맞춘 단색 머티리얼(텍스처 없음). 크리스탈·지뢰 불빛은 발광.
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "Tools", "DemoMap"))
from HD2DMeshKit import FMaterialDef, FMeshBuilder  # noqa: E402

FOLDER = "Asset/FarmBie/Build"

WOOD   = FMaterialDef("BuildWood", Tint=(0.3, 0.17, 0.08), Rough=0.9)
WOOD_D = FMaterialDef("BuildWoodDark", Tint=(0.15, 0.08, 0.04), Rough=0.9)
ROPE   = FMaterialDef("BuildRope", Tint=(0.42, 0.33, 0.17), Rough=1.0)
STONE  = FMaterialDef("BuildStone", Tint=(0.34, 0.29, 0.23), Rough=1.0)  # 선형 값 (sRGB로 약 0.62, 0.58, 0.52)
STONE_D = FMaterialDef("BuildStoneDark", Tint=(0.17, 0.14, 0.11), Rough=1.0)
IRON   = FMaterialDef("BuildIron", Tint=(0.09, 0.09, 0.1), Rough=0.9, Metal=0.0)  # 금속성이 높으면 위에서 하늘만 비친다
IRON_L = FMaterialDef("BuildIronLight", Tint=(0.32, 0.3, 0.27), Rough=0.8, Metal=0.1)
GLOW_R = FMaterialDef("BuildGlowRed", Tint=(0.9, 0.2, 0.15), Rough=0.4, Emissive=(3.0, 0.4, 0.2))
CRYSTAL = FMaterialDef("BuildCrystal", Tint=(0.42, 0.2, 0.72), Rough=0.2, Emissive=(0.35, 0.12, 0.6))
CRYSTAL_L = FMaterialDef("BuildCrystalLight", Tint=(0.7, 0.55, 0.95), Rough=0.15, Emissive=(0.7, 0.45, 1.1))
GLASS  = FMaterialDef("BuildGlass", Tint=(0.75, 0.9, 0.95), Rough=0.08, Blend=True, Alpha=0.28, DoubleSided=True)


class FOutBuilder(FMeshBuilder):
	# HD2DMeshKit.Box는 면 순서가 안쪽 법선을 만든다(앞면이 컬링되어 그늘진 안쪽이 보임) → 둘레 순서를 뒤집어 바깥 법선으로 쓴다.
	# (공유 키트는 Sample HD2D 에셋이 쓰므로 고치지 않고 여기서만 바꾼다)
	def Box(self, Mat, Center, Size, Yaw=0.0, Pitch=0.0, Roll=0.0, TexSize=100.0, Faces="xXyYzZ"):
		HX, HY, HZ = (V * 0.5 for V in Size)
		CY, SY = math.cos(math.radians(Yaw)), math.sin(math.radians(Yaw))

		def W(X, Y, Z):
			return (Center[0] + X * CY - Y * SY, Center[1] + X * SY + Y * CY, Center[2] + Z)
		Spec = {
			"x": [(-1, -1, -1), (-1, 1, -1), (-1, 1, 1), (-1, -1, 1)], "X": [(1, -1, -1), (1, -1, 1), (1, 1, 1), (1, 1, -1)],
			"y": [(-1, -1, -1), (-1, -1, 1), (1, -1, 1), (1, -1, -1)], "Y": [(-1, 1, -1), (1, 1, -1), (1, 1, 1), (-1, 1, 1)],
			"z": [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1)], "Z": [(-1, -1, 1), (-1, 1, 1), (1, 1, 1), (1, -1, 1)],
		}
		for Face in Faces:
			Q = [W(SX * HX, SY_ * HY, SZ * HZ) for SX, SY_, SZ in reversed(Spec[Face])]
			self.FlatQuad(Mat, *Q, TexSize=TexSize)


def _New(*Mats):
	B = FOutBuilder()
	for M in Mats:
		B.Material(M)
	return B


def WallWood():
	B = _New(WOOD, WOOD_D, ROPE)
	for I in range(4):
		X = -37.5 + I * 25.0
		H = 140.0 + (6.0 if I % 2 else 0.0)
		B.Cylinder("BuildWood", (X, 0, 0), (X, 0, H), 12.0, 8)
		B.Frustum("BuildWoodDark", (X, 0, 0), 12.0, 1.5, 22.0, 8, Z0=H)  # 뾰족한 끝
	for Z in (45.0, 105.0):
		B.Box("BuildRope", (0, 0, Z), (100.0, 27.0, 7.0))
	return B


def WallStone():
	B = _New(STONE, STONE_D)
	B.Box("BuildStone", (0, 0, 60.0), (100.0, 46.0, 120.0))
	B.Box("BuildStoneDark", (0, 0, 126.0), (104.0, 52.0, 12.0))
	for Row, Z in enumerate((20.0, 50.0, 80.0, 110.0)):
		B.Box("BuildStoneDark", (0, 0, Z), (101.0, 47.0, 2.0))
		for K in range(3):
			X = -33.0 + K * 33.0 + (16.0 if Row % 2 else 0.0)
			if abs(X) < 49:
				B.Box("BuildStoneDark", (X, 0, Z + 15.0), (2.0, 47.0, 28.0))
	return B


def WallIron():
	B = _New(IRON, IRON_L, STONE_D)
	B.Box("BuildStoneDark", (0, 0, 10.0), (104.0, 54.0, 20.0))
	B.Box("BuildIron", (0, 0, 80.0), (100.0, 36.0, 120.0))
	for X in (-45.0, 0.0, 45.0):
		B.Box("BuildIronLight", (X, 0, 82.0), (8.0, 40.0, 124.0))
	for X in (-25.0, 25.0):
		for Z in (40.0, 80.0, 120.0):
			B.Box("BuildIronLight", (X, 0, Z), (5.0, 40.0, 5.0))
	B.Box("BuildIronLight", (0, 0, 143.0), (104.0, 40.0, 6.0))
	return B


def Gate():
	B = _New(WOOD, WOOD_D, IRON)
	for X in (-44.0, 44.0):
		B.Box("BuildWoodDark", (X, 0, 75.0), (14.0, 22.0, 150.0))
	for I in range(5):
		X = -30.0 + I * 15.0
		B.Box("BuildWood", (X, 0, 50.0), (13.0, 10.0, 90.0))
	for Z in (25.0, 75.0):
		B.Box("BuildIron", (0, 0, Z), (76.0, 12.0, 6.0))
	B.Box("BuildWoodDark", (0, 0, 146.0), (104.0, 22.0, 10.0))
	return B


def Spike():
	B = _New(WOOD, IRON_L)
	B.Box("BuildWood", (0, 0, 3.0), (92.0, 92.0, 6.0))
	for IX in range(3):
		for IY in range(3):
			B.Frustum("BuildIronLight", (-28.0 + IX * 28.0, -28.0 + IY * 28.0, 0), 7.0, 0.8, 34.0, 6, Z0=6.0)
	return B


def Mine():
	B = _New(IRON, IRON_L, GLOW_R)
	B.Lathe("BuildIron", (0, 0, 0), [(26.0, 0.0), (28.0, 4.0), (24.0, 12.0), (12.0, 16.0), (0.0, 17.0)], 14)
	B.Frustum("BuildIronLight", (0, 0, 0), 5.0, 4.0, 6.0, 8, Z0=16.0)
	B.Lathe("BuildGlowRed", (0, 0, 22.0), [(0.01, 0.0), (4.0, 1.0), (3.0, 4.0), (0.0, 5.0)], 8)
	return B


def Turret():
	B = _New(STONE, STONE_D, WOOD, WOOD_D, IRON_L)
	B.Box("BuildStone", (0, 0, 35.0), (80.0, 80.0, 70.0))
	B.Box("BuildStoneDark", (0, 0, 72.0), (86.0, 86.0, 8.0))
	B.Cylinder("BuildWoodDark", (0, 0, 76.0), (0, 0, 104.0), 9.0, 8)
	# 쇠뇌: 몸통(+X 앞) + 활 + 화살
	B.Box("BuildWood", (6.0, 0, 110.0), (70.0, 14.0, 12.0))
	B.Box("BuildWoodDark", (34.0, 0, 110.0), (8.0, 76.0, 8.0), Yaw=0.0)
	B.Box("BuildIronLight", (24.0, 0, 118.0), (48.0, 3.0, 3.0))
	return B


def Crystal():
	B = _New(CRYSTAL, CRYSTAL_L, STONE, STONE_D)
	B.Lathe("BuildStoneDark", (0, 0, 0), [(48.0, 0.0), (50.0, 8.0), (40.0, 18.0), (0.0, 20.0)], 10)
	# 가운데 큰 결정 + 둘레 작은 결정 (팔각 뿔 두 개 이은 모양)
	def Shard(Mat, CX, CY, Z0, R, H, Tilt):
		B.Lathe(Mat, (CX, CY, Z0), [(0.01, 0.0), (R, H * 0.35), (R * 0.85, H * 0.7), (0.0, H)], 8, Phase=Tilt)
	Shard("BuildCrystal", 0, 0, 14.0, 26.0, 150.0, 0.2)
	Shard("BuildCrystalLight", 0, 0, 40.0, 12.0, 96.0, 0.6)
	for K in range(5):
		A = K / 5.0 * math.tau + 0.4
		Shard("BuildCrystal", math.cos(A) * 32.0, math.sin(A) * 32.0, 10.0, 11.0, 50.0 + 12.0 * (K % 2), A)
	return B


def Greenhouse(W, H):
	# 온실 (W×H 칸): 나무 기둥·테 + 반투명 유리 벽·지붕 (안이 보이게 지붕 유리는 아주 옅게). 원점 = 바닥 가운데
	B = _New(WOOD_D, GLASS)
	HX, HY, Z = W * 50.0, H * 50.0, 230.0
	for X in (-HX, HX):
		for Y in (-HY, HY):
			B.Box("BuildWoodDark", (X, Y, Z * 0.5), (10.0, 10.0, Z))
	for X in range(1, W):
		for Y in (-HY, HY):
			B.Box("BuildWoodDark", (-HX + X * 100.0, Y, Z * 0.5), (6.0, 6.0, Z))
	for Y in (-HY, HY):
		B.Box("BuildWoodDark", (0, Y, Z), (W * 100.0 + 10.0, 8.0, 8.0))
	for X in (-HX, HX):
		B.Box("BuildWoodDark", (X, 0, Z), (8.0, H * 100.0 + 10.0, 8.0))
	# 유리 벽: 뒤(-Y)·옆, 앞(+Y)은 문 자리만 비운 낮은 유리
	B.Box("BuildGlass", (0, -HY, Z * 0.5), (W * 100.0, 2.0, Z))
	B.Box("BuildGlass", (-HX, 0, Z * 0.5), (2.0, H * 100.0, Z))
	B.Box("BuildGlass", (HX, 0, Z * 0.5), (2.0, H * 100.0, Z))
	B.Box("BuildGlass", (-HX * 0.5 - 50.0, HY, Z * 0.5), (W * 50.0 - 100.0, 2.0, Z))
	B.Box("BuildGlass", (HX * 0.5 + 50.0, HY, Z * 0.5), (W * 50.0 - 100.0, 2.0, Z))
	for X in range(W + 1):
		B.Box("BuildWoodDark", (-HX + X * 100.0, 0, Z + 2.0), (5.0, H * 100.0, 5.0))
	return B


def Workbench():
	B = _New(WOOD, WOOD_D, IRON_L, ROPE)
	B.Box("BuildWood", (0, 0, 78.0), (150.0, 70.0, 10.0))
	for X in (-65.0, 65.0):
		for Y in (-27.0, 27.0):
			B.Box("BuildWoodDark", (X, Y, 38.0), (10.0, 10.0, 76.0))
	B.Box("BuildWoodDark", (0, 0, 22.0), (140.0, 60.0, 6.0))
	B.Box("BuildIronLight", (-30.0, 0, 86.0), (36.0, 18.0, 6.0))   # 모루
	B.Box("BuildRope", (40.0, 8.0, 88.0), (30.0, 30.0, 10.0))      # 섬유 묶음
	B.Cylinder("BuildWoodDark", (10.0, -18.0, 84.0), (42.0, -24.0, 84.0), 2.5, 6)  # 망치 자루
	B.Box("BuildIronLight", (44.0, -24.0, 86.0), (8.0, 14.0, 8.0))
	return B


MODELS = {
	"WallWood": WallWood, "WallStone": WallStone, "WallIron": WallIron, "Gate": Gate, "Spike": Spike, "Mine": Mine, "Turret": Turret,
	"Crystal": Crystal, "Workbench": Workbench,
}


def WriteModels(Content, GreenhouseSize):
	for Name, Make in MODELS.items():
		Make().Save(FOLDER, Name, Content)
	Greenhouse(*GreenhouseSize).Save(FOLDER, "Greenhouse", Content)
