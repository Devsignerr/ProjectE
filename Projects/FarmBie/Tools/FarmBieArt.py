# FarmBie 자체 도트 아트 (CC0, 절차 생성 — 결정적). BuildFarmBie.py가 부른다.
#   농부(32x32 칸, 아래/위/옆 3방향 × 대기·걷기), 효과(그림자 원), 바닥 텍스처(풀·흙길·밭 흙·돌 — 이음매 없는 반복 PNG)
#   규약: 1 도트 = UNITS_PER_PIXEL cm (HD2DArt와 같은 6cm), 캐릭터 슬라이스 피벗 = 아래 가운데(발), 옆모습은 오른쪽을 본다(왼쪽은 SetSpriteFlip)
#   캔버스·아틀라스·플립북 쓰기는 Tools/DemoMap/HD2DArt.py를 그대로 쓴다 (FCanvas/FAtlas/WriteFlipbook)
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "Tools", "DemoMap"))
from HD2DArt import FAtlas, FCanvas, WriteFlipbook  # noqa: E402

CELL = 32

# ---- 팔레트 (sRGB) ----------------------------------------------------------------------------------------------------
OUTLINE  = (38, 26, 30)
SKIN     = (246, 204, 160)
SKIN_D   = (208, 150, 112)
EYE      = (40, 28, 40)
CHEEK    = (236, 146, 128)
HAIR     = (96, 58, 34)
HAT      = (238, 204, 112)
HAT_L    = (252, 232, 158)
HAT_D    = (190, 148, 70)
BAND     = (186, 60, 52)
SHIRT    = (208, 84, 70)
SHIRT_D  = (152, 52, 50)
OVERALL  = (70, 112, 176)
OVERALL_L = (104, 148, 208)
OVERALL_D = (46, 74, 124)
BUTTON   = (240, 210, 96)
BOOT     = (110, 70, 44)
BOOT_D   = (74, 46, 32)


# ---- 농부 ----------------------------------------------------------------------------------------------------------------
def _HatFront(C, B, bBack):
	# 넓은 밀짚모자 챙(가로 18) + 둥근 꼭대기 + 붉은 띠. 뒷모습은 챙 아래로 머리카락
	Y = 5 + B
	C.Rect(10, Y, 21, Y + 3, HAT)            # 꼭대기
	C.Rect(11, Y - 1, 20, Y - 1, HAT)
	C.Rect(12, Y, 15, Y + 1, HAT_L)
	C.Rect(10, Y + 3, 21, Y + 3, BAND)       # 띠
	C.Rect(7, Y + 4, 24, Y + 5, HAT)         # 챙
	C.Rect(6, Y + 5, 25, Y + 5, HAT_D)
	C.Rect(8, Y + 4, 12, Y + 4, HAT_L)
	if bBack:
		C.Rect(11, Y + 6, 20, Y + 9, HAIR)
		C.Rect(12, Y + 10, 19, Y + 10, HAIR)


def _FaceFront(C, B):
	Y = 11 + B
	C.Rect(11, Y, 20, Y + 4, SKIN)
	C.Rect(12, Y + 5, 19, Y + 5, SKIN)
	C.Rect(11, Y, 20, Y, SKIN_D)              # 챙 그림자
	C.Rect(10, Y + 1, 10, Y + 2, HAIR)        # 옆머리
	C.Rect(21, Y + 1, 21, Y + 2, HAIR)
	C.Px(13, Y + 2, EYE)
	C.Px(13, Y + 3, EYE)
	C.Px(18, Y + 2, EYE)
	C.Px(18, Y + 3, EYE)
	C.Px(12, Y + 4, CHEEK)
	C.Px(19, Y + 4, CHEEK)
	C.Rect(15, Y + 4, 16, Y + 4, SKIN_D)      # 입


def _BodyFront(C, B, bBack, ArmL, ArmR):
	Y = 17 + B
	# 셔츠(어깨·팔) + 멜빵바지
	C.Rect(10, Y, 21, Y + 6, SHIRT)
	C.Rect(10, Y + 5, 21, Y + 6, SHIRT_D)
	C.Rect(8, Y + 1 + ArmL, 9, Y + 6 + ArmL, SHIRT)     # 왼팔
	C.Rect(22, Y + 1 + ArmR, 23, Y + 6 + ArmR, SHIRT)   # 오른팔
	C.Rect(8, Y + 7 + ArmL, 9, Y + 7 + ArmL, SKIN)      # 손
	C.Rect(22, Y + 7 + ArmR, 23, Y + 7 + ArmR, SKIN)
	C.Rect(11, Y + 3, 20, Y + 9, OVERALL)                # 바지 앞판
	C.Rect(11, Y + 8, 20, Y + 9, OVERALL_D)
	C.Rect(12, Y + 3, 13, Y + 5, OVERALL_L)
	if bBack:
		C.Rect(12, Y, 13, Y + 3, OVERALL)                # 멜빵 (등 X자)
		C.Rect(18, Y, 19, Y + 3, OVERALL)
		C.Rect(14, Y + 2, 17, Y + 3, OVERALL_D)
	else:
		C.Rect(12, Y, 12, Y + 3, OVERALL)                # 멜빵
		C.Rect(19, Y, 19, Y + 3, OVERALL)
		C.Px(12, Y + 3, BUTTON)
		C.Px(19, Y + 3, BUTTON)
		C.Rect(14, Y + 5, 17, Y + 6, OVERALL_D)          # 앞주머니


def _LegsFront(C, Legs):
	# Legs: 0 서기, 1 왼발 앞, 2 오른발 앞 (걷기)
	LY = (0, -1, 0)[Legs]
	RY = (0, 0, -1)[Legs]
	C.Rect(12, 27 + LY, 14, 29 + LY, OVERALL_D)
	C.Rect(17, 27 + RY, 19, 29 + RY, OVERALL_D)
	C.Rect(11, 30 + LY, 14, 31 + LY, BOOT)
	C.Rect(17, 30 + RY, 20, 31 + RY, BOOT)
	C.Rect(11, 31 + LY, 14, 31 + LY, BOOT_D)
	C.Rect(17, 31 + RY, 20, 31 + RY, BOOT_D)


def _Side(C, B, Legs, ArmDX):
	# 오른쪽을 보는 옆모습
	Y = 5 + B
	# 다리 (걷기: 앞뒤로 벌림)
	Spread = (0, 2, -2)[Legs]
	C.Rect(14 + Spread, 27, 16 + Spread, 29, OVERALL_D)
	C.Rect(14 - Spread, 27, 16 - Spread, 29, OVERALL)
	C.Rect(14 + Spread, 30, 18 + Spread, 31, BOOT)
	C.Rect(14 - Spread, 30, 18 - Spread, 31, BOOT_D)
	# 몸
	C.Rect(12, 17 + B, 19, 23 + B, SHIRT)
	C.Rect(12, 20 + B, 19, 26 + B, OVERALL)
	C.Rect(12, 25 + B, 19, 26 + B, OVERALL_D)
	C.Rect(17, 17 + B, 17, 20 + B, OVERALL)                 # 멜빵
	C.Px(17, 20 + B, BUTTON)
	# 팔 (흔들림)
	C.Rect(14 + ArmDX, 18 + B, 16 + ArmDX, 23 + B, SHIRT_D)
	C.Rect(14 + ArmDX, 24 + B, 16 + ArmDX, 24 + B, SKIN)
	# 머리
	C.Rect(12, 11 + B, 20, 16 + B, SKIN)
	C.Rect(12, 11 + B, 15, 15 + B, HAIR)
	C.Rect(12, 11 + B, 20, 11 + B, SKIN_D)
	C.Px(18, 13 + B, EYE)
	C.Px(18, 14 + B, EYE)
	C.Px(19, 15 + B, CHEEK)
	C.Px(21, 14 + B, SKIN)                                  # 코
	# 모자 (챙은 앞쪽으로 길게)
	C.Rect(11, Y, 19, Y + 3, HAT)
	C.Rect(12, Y - 1, 18, Y - 1, HAT)
	C.Rect(13, Y, 15, Y + 1, HAT_L)
	C.Rect(11, Y + 3, 19, Y + 3, BAND)
	C.Rect(8, Y + 4, 24, Y + 5, HAT)
	C.Rect(8, Y + 5, 24, Y + 5, HAT_D)


def DrawFarmer(Dir, Pose, Frame):
	C = FCanvas(CELL, CELL)
	B, Legs, Arm = 0, 0, 0
	if Pose == "Idle":
		B = Frame % 2
	elif Pose == "Walk":
		Legs = (1, 0, 2, 0)[Frame % 4]
		B = (0, 1, 0, 1)[Frame % 4]
		Arm = (1, 0, -1, 0)[Frame % 4]
	if Dir == "Side":
		_Side(C, B, Legs, Arm)
	else:
		bBack = Dir == "Up"
		_LegsFront(C, Legs)
		Swing = (0, -1, 1)[Legs]
		_BodyFront(C, B, bBack, Swing, -Swing)
		if not bBack:
			_FaceFront(C, B)
		_HatFront(C, B, bBack)
	C.Outline(OUTLINE)
	return C


FARMER_POSES = {"Idle": (2, 2.5), "Walk": (4, 9.0)}  # 프레임 수, 초당 프레임

# 도구 동작 (32x40 칸 — 발은 위에서 31줄, 아래 8줄은 앞으로 내리친 도구 자리. 피벗 = 발 = 아래에서 8/40)
USE_H = 40
USE_PIVOT = (0.5, (USE_H - 32) / USE_H)
HANDLE, HANDLE_D = (150, 104, 60), (108, 72, 42)
METAL, METAL_D = (186, 192, 202), (128, 134, 146)
CAN, CAN_L, CAN_D = (86, 136, 186), (140, 186, 230), (60, 96, 140)
WATER = (150, 210, 255)


def _Hoe(C, X0, Y0, X1, Y1, BladeX, BladeY, BladeW, BladeH):
	C.Line(X0, Y0, X1, Y1, HANDLE)
	C.Line(X0 + 1, Y0, X1 + 1, Y1, HANDLE_D)
	C.Rect(BladeX, BladeY, BladeX + BladeW - 1, BladeY + BladeH - 1, METAL)
	C.Rect(BladeX, BladeY + BladeH - 1, BladeX + BladeW - 1, BladeY + BladeH - 1, METAL_D)


def _Can(C, X, Y, bTilt, Drops):
	C.Rect(X, Y, X + 6, Y + 4, CAN)
	C.Rect(X + 1, Y + 1, X + 2, Y + 3, CAN_L)
	C.Rect(X, Y + 4, X + 6, Y + 4, CAN_D)
	if bTilt:
		C.Line(X + 6, Y + 2, X + 9, Y + 5, CAN_D)
	else:
		C.Line(X + 6, Y + 1, X + 9, Y - 1, CAN_D)
	for DX, DY in Drops:
		C.Px(DX, DY, WATER)


def DrawFarmerUse(Dir, Tool, Frame):
	Body = DrawFarmer(Dir, "Walk" if Frame == 1 else "Idle", 1 if Frame == 1 else 0)
	C = FCanvas(CELL, USE_H)
	Tools = FCanvas(CELL, USE_H)
	if Tool == "Hoe":
		if Dir == "Down":
			if Frame == 0:
				_Hoe(Tools, 22, 22, 25, 9, 23, 6, 5, 3)
			else:
				_Hoe(Tools, 18, 24, 17, 35, 14, 35, 7, 3)
		elif Dir == "Up":
			if Frame == 0:
				_Hoe(Tools, 10, 22, 7, 9, 5, 6, 5, 3)
			else:
				_Hoe(Tools, 16, 16, 16, 4, 13, 1, 7, 3)
		else:
			if Frame == 0:
				_Hoe(Tools, 15, 21, 8, 9, 5, 6, 5, 3)
			else:
				_Hoe(Tools, 18, 22, 27, 30, 26, 30, 4, 6)
	elif Tool == "Can":
		if Dir == "Down":
			_Can(Tools, 19, 23 if Frame == 0 else 29, Frame == 1, [] if Frame == 0 else [(29, 36), (27, 38), (30, 39)])
		elif Dir == "Up":
			_Can(Tools, 13, 10 if Frame == 0 else 4, Frame == 1, [] if Frame == 0 else [(23, 10), (24, 12), (22, 13)])
		else:
			_Can(Tools, 19, 20 if Frame == 0 else 23, Frame == 1, [] if Frame == 0 else [(29, 30), (30, 32), (28, 33)])
	Tools.Outline(OUTLINE)
	# 위를 볼 때 도구는 몸 앞(카메라 반대쪽) → 몸을 나중에, 그 밖에는 도구를 나중에
	Layers = [Tools.P, None] if Dir == "Up" else [None, Tools.P]
	for Layer in Layers:
		if Layer is None:
			Mask = Body.P[..., 3] > 0
			C.P[:CELL][Mask] = Body.P[Mask]
		else:
			Mask = Layer[..., 3] > 0
			C.P[Mask] = Layer[Mask]
	return C


USE_POSES = {"Hoe": [0.2, 0.22], "Can": [0.16, 0.3]}  # 도구 → 프레임 길이


def DrawShadow():
	C = FCanvas(24, 10)
	C.Ellipse(12, 5, 11, 4.5, (20, 16, 28), 110)
	C.Ellipse(12, 5, 8, 3.2, (20, 16, 28), 150)
	return C


def WriteSprites(Folder):
	Atlas = FAtlas(768)
	for Dir in ("Down", "Up", "Side"):
		for Pose, (Count, _) in FARMER_POSES.items():
			for Frame in range(Count):
				Atlas.Add(f"{Pose}{Dir}{Frame}", DrawFarmer(Dir, Pose, Frame))
	for Dir in ("Down", "Up", "Side"):
		for Tool, Times in USE_POSES.items():
			for Frame in range(len(Times)):
				Atlas.Add(f"{Tool}{Dir}{Frame}", DrawFarmerUse(Dir, Tool, Frame), USE_PIVOT)
	Atlas.Save(Folder, "Farmer")
	for Dir in ("Down", "Up", "Side"):
		for Pose, (Count, Fps) in FARMER_POSES.items():
			WriteFlipbook(Folder, f"Farmer_{Pose}{Dir}", "Farmer.esprite", [f"{Pose}{Dir}{I}" for I in range(Count)], Fps)
		for Tool, Times in USE_POSES.items():
			WriteFlipbook(Folder, f"Farmer_{Tool}{Dir}", "Farmer.esprite", [f"{Tool}{Dir}{I}" for I in range(len(Times))], 0, "Once", Times)
	Fx = FAtlas(128)
	Fx.Add("Shadow", DrawShadow(), (0.5, 0.5))
	Fx.Save(Folder, "Fx")


# ---- 바닥 텍스처 ---------------------------------------------------------------------------------------------------------
# 이음매 없는 반복(주기 노이즈) 256px — 지형 레이어 머티리얼의 BaseColor. 큰 얼룩(저주파) + 잔무늬 + 점(풀잎·자갈)
def _PeriodicNoise(Size, Period, Seed):
	Rng = np.random.default_rng(Seed)
	Grid = Rng.random((Period, Period))
	Coord = np.arange(Size) * Period / Size
	I0 = np.floor(Coord).astype(int)
	F = Coord - I0
	F = F * F * (3.0 - 2.0 * F)
	I1 = (I0 + 1) % Period
	A = Grid[np.ix_(I0, I0)]
	B_ = Grid[np.ix_(I0, I1)]
	C_ = Grid[np.ix_(I1, I0)]
	D = Grid[np.ix_(I1, I1)]
	FX = F[None, :]
	FY = F[:, None]
	return (A * (1 - FX) + B_ * FX) * (1 - FY) + (C_ * (1 - FX) + D * FX) * FY


def _Fbm(Size, Seed, Periods=(4, 8, 16, 32), Weights=(0.45, 0.28, 0.17, 0.1)):
	return sum(W * _PeriodicNoise(Size, P, Seed + I) for I, (P, W) in enumerate(zip(Periods, Weights)))


def _Ramp(T, Colors):
	# T(0~1) → 색 띠 (계단 — 도트풍 몇 단계 색)
	Steps = len(Colors)
	Index = np.clip((T * Steps).astype(int), 0, Steps - 1)
	Palette = np.array(Colors, dtype=np.float32)
	return Palette[Index]


GROUND_TEXTURES = {
	# 이름: (색 단계, 점 색, 점 밀도, 시드)
	"Grass": ([(58, 92, 44), (66, 104, 48), (74, 114, 52), (84, 124, 56)], [(104, 146, 66), (48, 78, 38)], 0.06, 11),
	"Path":  ([(128, 100, 70), (144, 114, 80), (158, 128, 90), (170, 140, 100)], [(110, 86, 62), (186, 160, 120)], 0.04, 23),
	"Soil":  ([(112, 88, 62), (122, 96, 68), (132, 104, 74), (140, 112, 80)], [(96, 74, 52), (156, 128, 92)], 0.04, 37),
	"Stone": ([(106, 108, 104), (122, 124, 118), (136, 138, 130), (150, 150, 142)], [(90, 92, 90), (170, 170, 160)], 0.03, 41),
}


def WriteGroundTextures(Folder, Size=256):
	os.makedirs(Folder, exist_ok=True)
	for Name, (Ramp, Specks, Density, Seed) in GROUND_TEXTURES.items():
		T = _Fbm(Size, Seed)
		T = (T - T.min()) / max(1e-6, T.max() - T.min())
		Color = _Ramp(T, Ramp)
		Rng = np.random.default_rng(Seed + 100)
		Mask = Rng.random((Size, Size))
		for Index, Speck in enumerate(Specks):
			Hit = (Mask > Index * Density) & (Mask < (Index + 1) * Density)
			Color[Hit] = Speck
		Image.fromarray(np.clip(Color, 0, 255).astype(np.uint8), "RGB").save(os.path.join(Folder, f"{Name}.png"), optimize=True)
