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


def _SetUnits(Folder, Name, Units):
	import json
	Path = os.path.join(Folder, f"{Name}.esprite")
	with open(Path, encoding="utf-8") as File:
		Doc = json.load(File)
	Doc["UnitsPerPixel"] = Units
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")

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


def _Hoe(C, X0, Y0, X1, Y1, BladeX, BladeY, BladeW, BladeH, Kind="Hoe"):
	C.Line(X0, Y0, X1, Y1, HANDLE)
	C.Line(X0 + 1, Y0, X1 + 1, Y1, HANDLE_D)
	if Kind == "Axe":
		# 도끼: 두껍고 둥근 날 (한쪽으로 부푼 쐐기)
		C.Rect(BladeX - 1, BladeY - 1, BladeX + BladeW, BladeY + BladeH, METAL)
		C.Rect(BladeX - 1, BladeY + BladeH, BladeX + BladeW, BladeY + BladeH, METAL_D)
		C.Px(BladeX - 1, BladeY - 1, (230, 236, 244))
	elif Kind == "Pick":
		# 곡괭이: 양쪽으로 뾰족한 머리
		C.Rect(BladeX - 2, BladeY, BladeX + BladeW + 1, BladeY + 1, METAL)
		C.Px(BladeX - 3, BladeY + 2, METAL_D)
		C.Px(BladeX + BladeW + 2, BladeY + 2, METAL_D)
	else:
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
	if Tool in ("Hoe", "Axe", "Pick"):
		K = Tool
		if Dir == "Down":
			if Frame == 0:
				_Hoe(Tools, 22, 22, 25, 9, 23, 6, 5, 3, K)
			else:
				_Hoe(Tools, 18, 24, 17, 35, 14, 35, 7, 3, K)
		elif Dir == "Up":
			if Frame == 0:
				_Hoe(Tools, 10, 22, 7, 9, 5, 6, 5, 3, K)
			else:
				_Hoe(Tools, 16, 16, 16, 4, 13, 1, 7, 3, K)
		else:
			if Frame == 0:
				_Hoe(Tools, 15, 21, 8, 9, 5, 6, 5, 3, K)
			else:
				_Hoe(Tools, 18, 22, 27, 30, 26, 30, 4, 6, K)
	elif Tool == "Sword":
		Blade, Edge, Hilt = (220, 228, 240), (150, 160, 180), (200, 160, 60)
		Lines = {"Down": [((22, 22), (27, 10)), ((18, 24), (12, 37))], "Up": [((10, 22), (5, 10)), ((16, 16), (16, 1))], "Side": [((15, 21), (8, 8)), ((18, 22), (31, 27))]}
		(X0, Y0), (X1, Y1) = Lines[Dir][Frame]
		Tools.Line(X0, Y0, X1, Y1, Blade)
		Tools.Line(X0 + 1, Y0, X1 + 1, Y1, Edge)
		Tools.Rect(X0 - 1, Y0 - 1, X0 + 1, Y0 + 1, Hilt)
	elif Tool in ("Bow", "Gun"):
		Wood, Metal = (130, 84, 44), (90, 94, 104)
		if Dir == "Side":
			if Tool == "Bow":
				Tools.Line(24, 15, 27, 20, Wood)
				Tools.Line(27, 20, 24, 25, Wood)
				Tools.Line(24, 15, 24, 25, (220, 220, 210))
				if Frame == 0:
					Tools.Line(18, 20, 27, 20, (200, 170, 120))
			else:
				Tools.Rect(17, 19, 30, 20, Metal)
				Tools.Rect(17, 21, 21, 22, Wood)
				if Frame == 1:
					Tools.Rect(31, 18, 33 if Tools.W > 33 else 31, 21, (255, 210, 90))
		elif Dir == "Down":
			if Tool == "Bow":
				Tools.Line(11, 28, 16, 31, Wood)
				Tools.Line(16, 31, 21, 28, Wood)
				Tools.Line(11, 28, 21, 28, (220, 220, 210))
			else:
				Tools.Rect(15, 23, 16, 36, Metal)
				Tools.Rect(14, 23, 17, 26, Wood)
				if Frame == 1:
					Tools.Rect(14, 37, 17, 39, (255, 210, 90))
		else:
			if Tool == "Bow":
				Tools.Line(11, 8, 16, 5, Wood)
				Tools.Line(16, 5, 21, 8, Wood)
			else:
				Tools.Rect(15, 2, 16, 14, Metal)
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


USE_POSES = {"Hoe": [0.2, 0.22], "Can": [0.16, 0.3], "Axe": [0.2, 0.22], "Pick": [0.2, 0.22], "Sword": [0.12, 0.22], "Bow": [0.2, 0.3],
			 "Gun": [0.15, 0.5]}  # 도구·무기 → 프레임 길이


# ---- 보부상 (수상한 떠돌이 상인: 후드 망토 + 커다란 등짐 + 그림자 속 빛나는 눈) -----------------------------------------------------
CLOAK, CLOAK_L, CLOAK_D = (78, 58, 96), (112, 86, 134), (50, 36, 64)
PACK, PACK_L, PACK_D = (140, 96, 56), (182, 132, 80), (96, 62, 36)
GLOW = (250, 214, 110)


def DrawPeddler(Frame):
	C = FCanvas(36, 44)
	B = Frame % 2
	# 등짐 (몸 뒤로 크게 — 냄비·두루마리·등불)
	C.Rect(5, 4 + B, 30, 26 + B, PACK)
	C.Rect(6, 5 + B, 12, 25 + B, PACK_L)
	C.Rect(5, 26 + B, 30, 26 + B, PACK_D)
	C.Rect(4, 12 + B, 31, 13 + B, PACK_D)
	C.Ellipse(9, 4 + B, 3.5, 3, (110, 110, 120))      # 냄비
	C.Rect(22, 1 + B, 28, 4 + B, (220, 206, 170))     # 두루마리
	C.Rect(29, 15 + B, 32, 20 + B, (90, 70, 40))      # 등불
	C.Rect(30, 16 + B, 31, 19 + B, GLOW)
	# 망토 몸
	C.Rect(10, 16 + B, 25, 38, CLOAK)
	C.Rect(9, 24 + B, 26, 38, CLOAK)
	C.Rect(11, 17 + B, 13, 37, CLOAK_L)
	C.Rect(9, 38, 26, 39, CLOAK_D)
	C.Rect(17, 26 + B, 18, 38, CLOAK_D)               # 앞 여밈
	# 후드 + 그림진 얼굴 + 빛나는 눈
	C.Ellipse(17.5, 15 + B, 7.5, 7, CLOAK)
	C.Ellipse(17.5, 14 + B, 6, 5.5, CLOAK_L)
	C.Ellipse(17.5, 17 + B, 4.5, 3.6, (24, 16, 30))
	C.Px(15, 17 + B, GLOW)
	C.Px(20, 17 + B, GLOW)
	# 손 + 지팡이
	C.Line(28, 22 + B, 30, 41, (110, 80, 50))
	C.Rect(26, 26 + B, 28, 27 + B, (200, 170, 140))
	# 발
	C.Rect(12, 40, 16, 41, (60, 44, 34))
	C.Rect(19, 40, 23, 41, (60, 44, 34))
	C.Outline(OUTLINE)
	return C


# ---- 숲 채집물 (빌보드, 1 도트 = 6.25cm — Forage.esprite) -----------------------------------------------------------------
def DrawFiberBush(bPicked):
	C = FCanvas(20, 16)
	Leaf, LeafL, LeafD = (96, 150, 70), (140, 190, 96), (60, 104, 52)
	if bPicked:
		C.Ellipse(10, 13, 6, 2.5, LeafD)
		for X in (6, 9, 12, 14):
			C.Line(X, 14, X + (1 if X % 2 else -1), 10, LeafD)
	else:
		C.Ellipse(10, 10, 8.5, 5.5, LeafD)
		C.Ellipse(9, 8.5, 7, 4.5, Leaf)
		C.Ellipse(8, 7, 4, 2.5, LeafL)
		for X, Y in ((4, 4), (8, 2), (12, 3), (16, 5)):
			C.Line(X, Y, X + 1, Y + 4, (200, 196, 140))  # 섬유 줄기
	C.Outline((30, 44, 24))
	return C


def DrawHerb(bPicked):
	C = FCanvas(14, 14)
	if bPicked:
		C.Rect(6, 11, 7, 13, (70, 110, 60))
	else:
		C.Line(7, 13, 7, 5, (70, 120, 60))
		for X, Y, Col in ((4, 6, (110, 180, 90)), (10, 5, (110, 180, 90)), (5, 9, (90, 160, 80)), (9, 9, (90, 160, 80))):
			C.Ellipse(X, Y, 2.4, 1.4, Col)
		C.Ellipse(7, 3, 1.6, 1.6, (240, 236, 130))  # 작은 꽃
	C.Outline((24, 40, 22))
	return C


def DrawMushroom(bPicked):
	C = FCanvas(14, 14)
	if bPicked:
		C.Rect(6, 12, 8, 13, (200, 190, 170))
	else:
		C.Rect(6, 8, 8, 13, (226, 216, 196))
		C.Ellipse(7, 7, 6, 3.5, (170, 70, 160))
		C.Ellipse(7, 6, 4.5, 2.4, (210, 110, 200))
		for X, Y in ((4, 6), (9, 5), (7, 7)):
			C.Px(X, Y, (250, 230, 250))
	C.Outline((40, 20, 40))
	return C


def _Bar(Col):
	C = FCanvas(16, 3)
	C.Rect(0, 0, 15, 2, Col)
	return C


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
	Peddler = FAtlas(128)
	for Frame in range(2):
		Peddler.Add(f"Idle{Frame}", DrawPeddler(Frame))
	Peddler.Save(Folder, "Peddler")
	WriteFlipbook(Folder, "Peddler_Idle", "Peddler.esprite", ["Idle0", "Idle1"], 2.0)
	Forage = FAtlas(128)
	for Name, Draw in (("Fiber", DrawFiberBush), ("Herb", DrawHerb), ("Mushroom", DrawMushroom)):
		Forage.Add(Name, Draw(False))
		Forage.Add(Name + "Picked", Draw(True))
	Forage.Save(Folder, "Forage")
	_SetUnits(Folder, "Forage", 100.0 / 16.0)
	Fx = FAtlas(256)
	Fx.Add("Shadow", DrawShadow(), (0.5, 0.5))
	Warn = FCanvas(20, 30)
	Warn.Rect(9, 12, 10, 29, (110, 72, 40))
	Warn.Rect(2, 1, 17, 13, (200, 40, 40))
	Warn.Rect(3, 2, 16, 12, (232, 70, 60))
	Warn.Ellipse(9.5, 6, 3.5, 3, (240, 236, 220))
	Warn.Px(8, 6, (40, 20, 20))
	Warn.Px(11, 6, (40, 20, 20))
	Warn.Rect(8, 9, 11, 10, (240, 236, 220))
	Warn.Outline((40, 18, 16))
	Fx.Add("Warn", Warn, (0.5, 0.0))
	Fx.Add("HpBack", _Bar((30, 20, 24)), (0.5, 0.5))
	Fx.Add("HpFill", _Bar((230, 60, 60)), (0.0, 0.5))
	import FarmBieZombies
	FarmBieZombies.WriteSprites(Folder, Fx)
	Fx.Save(Folder, "Fx")
	FarmBieZombies.WriteFxFlipbooks(Folder)


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
