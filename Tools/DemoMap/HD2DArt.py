# HD-2D 데모 맵용 자체 도트 아트 (CC0, 절차 생성 — 결정적). BuildHD2D.py가 부른다.
#   용사(32x32 칸, 아래/위/옆 3방향 × 대기·걷기·공격·대시 + 창·활·지팡이 공격 자세), 슬라임(24x20 칸), 적(박쥐·고블린·해골 궁수·독버섯),
#   보스(바위 골렘 64x64), 마을 사람 4종, 소품(보물상자·코인·주머니), 효과(베기 호·찌르기·마법 탄·폭발·화살·바위·충격파·독 웅덩이·반짝임·빛기둥·체력바 등),
#   UI 텍스처(창틀 9-슬라이스·선택 띠·커서·아이템 아이콘 — UI 샘플러가 선형이라 4배 최근접 확대 PNG, WriteUiTextures)
#   규약: 1 도트 = UNITS_PER_PIXEL cm, 캐릭터 슬라이스 피벗 = 아래 가운데(발), 옆모습은 오른쪽을 본다(왼쪽은 SetSpriteFlip)
#   그리는 순서: 칸마다 색 면을 칠한 뒤 Outline()이 불투명 화소 바깥 4-이웃에 외곽선 색을 두른다
import json
import math
import os

import numpy as np
from PIL import Image

UNITS_PER_PIXEL = 6.0

# ---- 팔레트 (sRGB) ----------------------------------------------------------------------------------------------------
OUTLINE   = (36, 24, 40)
SKIN      = (248, 208, 168)
SKIN_D    = (214, 156, 122)
BLUSH     = (238, 148, 136)
EYE       = (44, 30, 56)
HAIR      = (132, 80, 44)
HAIR_L    = (184, 120, 66)
HAIR_D    = (88, 50, 32)
TUNIC     = (56, 98, 176)
TUNIC_L   = (96, 146, 218)
TUNIC_D   = (36, 62, 122)
SCARF     = (206, 52, 54)
SCARF_D   = (140, 32, 44)
BELT      = (112, 70, 40)
BUCKLE    = (244, 204, 84)
PANTS     = (78, 66, 92)
PANTS_D   = (56, 46, 70)
BOOT      = (104, 62, 38)
BOOT_D    = (70, 40, 28)
BLADE     = (226, 234, 246)
BLADE_D   = (150, 162, 184)
HILT      = (244, 204, 84)

SLIME     = (92, 196, 96)
SLIME_L   = (164, 236, 140)
SLIME_D   = (46, 128, 64)
SLIME_OUT = (22, 58, 34)
WHITE     = (255, 255, 255)


class FCanvas:
	def __init__(self, W, H):
		self.W, self.H = W, H
		self.P = np.zeros((H, W, 4), dtype=np.uint8)

	def Px(self, X, Y, C, A=255):
		X, Y = int(round(X)), int(round(Y))
		if 0 <= X < self.W and 0 <= Y < self.H:
			self.P[Y, X] = (C[0], C[1], C[2], A)

	def Rect(self, X0, Y0, X1, Y1, C, A=255):
		for Y in range(min(Y0, Y1), max(Y0, Y1) + 1):
			for X in range(min(X0, X1), max(X0, X1) + 1):
				self.Px(X, Y, C, A)

	def Line(self, X0, Y0, X1, Y1, C, A=255):
		X0, Y0, X1, Y1 = int(X0), int(Y0), int(X1), int(Y1)
		DX, DY = abs(X1 - X0), -abs(Y1 - Y0)
		SX, SY = (1 if X0 < X1 else -1), (1 if Y0 < Y1 else -1)
		Err = DX + DY
		while True:
			self.Px(X0, Y0, C, A)
			if X0 == X1 and Y0 == Y1:
				break
			E2 = 2 * Err
			if E2 >= DY:
				Err += DY
				X0 += SX
			if E2 <= DX:
				Err += DX
				Y0 += SY

	def Ellipse(self, CX, CY, RX, RY, C, A=255):
		for Y in range(self.H):
			for X in range(self.W):
				DX, DY = (X + 0.5 - CX) / RX, (Y + 0.5 - CY) / RY
				if DX * DX + DY * DY <= 1.0:
					self.Px(X, Y, C, A)

	def Opaque(self, X, Y):
		return 0 <= X < self.W and 0 <= Y < self.H and self.P[Y, X, 3] > 0

	def Outline(self, C=OUTLINE):
		Mask = self.P[..., 3] > 0
		Grow = np.zeros_like(Mask)
		Grow[1:, :] |= Mask[:-1, :]
		Grow[:-1, :] |= Mask[1:, :]
		Grow[:, 1:] |= Mask[:, :-1]
		Grow[:, :-1] |= Mask[:, 1:]
		Edge = Grow & ~Mask
		self.P[Edge] = (C[0], C[1], C[2], 255)

	def Blade(self, X0, Y0, X1, Y1):
		# 칼: 날(밝은 선 + 아래쪽 그림자 선) + 손잡이 쪽 코등이
		self.Line(X0, Y0 + 1, X1, Y1 + 1, BLADE_D)
		self.Line(X0, Y0, X1, Y1, BLADE)
		self.Px(X0, Y0, HILT)
		self.Px(X0 + (1 if X1 >= X0 else -1), Y0, HILT)


# ---- 용사 ----------------------------------------------------------------------------------------------------------------
HERO_CELL = 32


def _HeroLegsFront(C, Phase):
	# Phase: 0 서기, 1 왼발 듦, 2 오른발 듦 (앞/뒤 모습)
	LiftL, LiftR = (1 if Phase == 1 else 0), (1 if Phase == 2 else 0)
	C.Rect(13, 25, 14, 27 - LiftL, PANTS)
	C.Rect(12, 28 - LiftL, 14, 30 - LiftL, BOOT)
	C.Rect(12, 30 - LiftL, 14, 30 - LiftL, BOOT_D)
	C.Rect(17, 25, 18, 27 - LiftR, PANTS_D)
	C.Rect(17, 28 - LiftR, 19, 30 - LiftR, BOOT)
	C.Rect(17, 30 - LiftR, 19, 30 - LiftR, BOOT_D)


def _HeroLegsSide(C, Phase):
	# Phase: 0 모음, 1 앞발 앞으로, 2 뒷발 앞으로 (옆모습, 오른쪽을 봄)
	if Phase == 0:
		C.Rect(14, 25, 15, 27, PANTS_D)
		C.Rect(14, 28, 16, 30, BOOT_D)
		C.Rect(16, 25, 17, 27, PANTS)
		C.Rect(16, 28, 18, 30, BOOT)
		return
	Front, Back = ((PANTS, BOOT), (PANTS_D, BOOT_D)) if Phase == 1 else ((PANTS_D, BOOT_D), (PANTS, BOOT))
	# 뒤로 뻗은 다리 (먼저 — 앞다리가 겹쳐 가린다)
	C.Rect(13, 25, 14, 26, Back[0])
	C.Rect(12, 27, 13, 28, Back[0])
	C.Rect(10, 28, 12, 30, Back[1])
	# 앞으로 뻗은 다리
	C.Rect(17, 25, 18, 26, Front[0])
	C.Rect(18, 27, 19, 28, Front[0])
	C.Rect(18, 28, 21, 30, Front[1])


def _HeroBodyFront(C, B, bBack, ArmL=0, ArmR=0):
	# B = 몸 내려감(숨쉬기·걸음 출렁임). ArmL/ArmR = 팔 들기(공격 자세에서 -2)
	C.Rect(12, 17 + B, 19, 24, TUNIC)
	C.Rect(11, 23, 20, 24, TUNIC)
	C.Rect(12, 17 + B, 12, 22, TUNIC_L)
	C.Rect(19, 17 + B, 19, 24, TUNIC_D)
	C.Rect(11, 24, 20, 24, TUNIC_D)
	C.Rect(12, 22 + B, 19, 22 + B, BELT)
	if not bBack:
		C.Rect(15, 22 + B, 16, 22 + B, BUCKLE)
	# 팔 (소매 + 손)
	C.Rect(10, 17 + B + ArmL, 11, 21 + B + ArmL, TUNIC)
	C.Rect(10, 22 + B + ArmL, 11, 22 + B + ArmL, SKIN)
	C.Rect(20, 17 + B + ArmR, 21, 21 + B + ArmR, TUNIC_D)
	C.Rect(20, 22 + B + ArmR, 21, 22 + B + ArmR, SKIN)
	# 목도리 (앞: 오른쪽으로 늘어진 끝, 뒤: 등 가운데로 늘어진 끝)
	C.Rect(11, 16 + B, 20, 17 + B, SCARF)
	C.Rect(11, 17 + B, 20, 17 + B, SCARF_D)
	if bBack:
		C.Rect(15, 18 + B, 16, 21 + B, SCARF)
		C.Rect(16, 18 + B, 16, 21 + B, SCARF_D)
	else:
		C.Rect(18, 18 + B, 19, 20 + B, SCARF)
		C.Px(19, 20 + B, SCARF_D)


def _HeroHeadFront(C, B, bBack):
	Top = 3 + B
	# 머리 윤곽 (모서리 깎은 12x13)
	if bBack:
		C.Rect(10, Top + 1, 21, Top + 11, HAIR)
		C.Rect(11, Top, 20, Top + 12, HAIR)
		C.Rect(12, Top + 1, 16, Top + 1, HAIR_L)
		C.Rect(11, Top + 10, 20, Top + 12, HAIR_D)
		C.Px(13, Top + 4, HAIR_L)
		C.Px(18, Top + 6, HAIR_D)
		return
	C.Rect(11, Top + 5, 20, Top + 12, SKIN)
	C.Rect(10, Top + 6, 10, Top + 10, SKIN)
	C.Rect(21, Top + 6, 21, Top + 10, SKIN)
	C.Rect(11, Top + 12, 20, Top + 12, SKIN_D)
	# 머리카락: 정수리 + 옆머리 + 앞머리 들쭉날쭉
	C.Rect(11, Top, 20, Top + 4, HAIR)
	C.Rect(10, Top + 1, 21, Top + 5, HAIR)
	C.Rect(10, Top + 6, 10, Top + 9, HAIR_D)
	C.Rect(21, Top + 6, 21, Top + 9, HAIR_D)
	for X in (11, 12, 14, 17, 19, 20):
		C.Px(X, Top + 6, HAIR)
	C.Rect(13, Top + 1, 16, Top + 1, HAIR_L)
	C.Px(12, Top + 2, HAIR_L)
	# 눈 + 볼
	C.Rect(13, Top + 8, 13, Top + 9, EYE)
	C.Rect(18, Top + 8, 18, Top + 9, EYE)
	C.Px(12, Top + 10, BLUSH)
	C.Px(19, Top + 10, BLUSH)
	C.Px(15, Top + 11, SKIN_D)
	C.Px(16, Top + 11, SKIN_D)


def _HeroBodySide(C, B, ArmDX=0, ArmDY=0, bArm=True, ScarfLen=0):
	C.Rect(13, 17 + B, 18, 24, TUNIC)
	C.Rect(12, 23, 19, 24, TUNIC)
	C.Rect(13, 17 + B, 13, 24, TUNIC_D)
	C.Rect(12, 24, 19, 24, TUNIC_D)
	C.Rect(13, 22 + B, 18, 22 + B, BELT)
	C.Px(18, 22 + B, BUCKLE)
	# 목도리 + 뒤로 날리는 끝 (ScarfLen = 달릴 때 더 길게)
	C.Rect(12, 16 + B, 19, 17 + B, SCARF)
	C.Rect(12, 17 + B, 19, 17 + B, SCARF_D)
	C.Rect(10 - ScarfLen, 17 + B, 12, 18 + B, SCARF)
	C.Rect(10 - ScarfLen, 18 + B, 11, 18 + B, SCARF_D)
	if ScarfLen > 0:
		C.Px(9 - ScarfLen, 19 + B, SCARF_D)
	if bArm:
		C.Rect(15 + ArmDX, 18 + B + ArmDY, 16 + ArmDX, 21 + B + ArmDY, TUNIC_L)
		C.Rect(15 + ArmDX, 22 + B + ArmDY, 16 + ArmDX, 22 + B + ArmDY, SKIN)


def _HeroHeadSide(C, B):
	Top = 3 + B
	C.Rect(14, Top + 5, 20, Top + 12, SKIN)
	C.Rect(21, Top + 7, 21, Top + 10, SKIN)
	C.Rect(14, Top + 12, 20, Top + 12, SKIN_D)
	C.Rect(12, Top, 19, Top + 4, HAIR)
	C.Rect(11, Top + 1, 20, Top + 5, HAIR)
	C.Rect(11, Top + 6, 14, Top + 11, HAIR)
	C.Rect(11, Top + 9, 13, Top + 11, HAIR_D)
	for X in (15, 16, 18, 20):
		C.Px(X, Top + 6, HAIR)
	C.Rect(14, Top + 1, 17, Top + 1, HAIR_L)
	C.Px(15, Top + 8, SKIN_D)  # 귀
	C.Rect(19, Top + 8, 19, Top + 9, EYE)
	C.Px(20, Top + 10, BLUSH)


def DrawHero(Dir, Pose, Frame, Weapon="Sword"):
	# Dir: "Down" | "Up" | "Side", Pose: "Idle" | "Walk" | "Attack" | "Dash", Weapon: 공격 자세에 든 무기 (Sword | Spear | Bow | Staff)
	C = FCanvas(HERO_CELL, HERO_CELL)
	B = 0
	Legs = 0
	if Pose == "Idle":
		B = Frame % 2
	elif Pose == "Walk":
		Legs = (1, 0, 2, 0)[Frame % 4]
		B = (0, 1, 0, 1)[Frame % 4]
	elif Pose == "Dash":
		Legs = 1
	elif Pose == "Attack":
		Legs = 1 if Dir == "Side" else 0
		B = (0, 1, 1)[Frame]

	if Dir in ("Down", "Up"):
		bBack = Dir == "Up"
		ArmR = -3 if (Pose == "Attack" and Frame == 0) else 0
		ArmL = -1 if (Pose == "Attack" and Frame == 1) else 0
		# 위 방향 공격: 칼은 몸 뒤 (먼저 그림)
		if Pose == "Attack" and bBack:
			if Weapon != "Sword":
				DrawHeroWeapon(C, Weapon, "Up", Frame, B)
			elif Frame == 0:
				C.Blade(21, 14, 25, 5)
			elif Frame == 1:
				C.Blade(19, 10, 7, 3)
			else:
				C.Blade(11, 12, 4, 7)
		_HeroLegsFront(C, Legs)
		_HeroBodyFront(C, B, bBack, ArmL, ArmR)
		_HeroHeadFront(C, B, bBack)
		if Pose == "Attack" and not bBack and Weapon != "Sword":
			DrawHeroWeapon(C, Weapon, "Down", Frame, B)
		elif Pose == "Attack" and not bBack:
			if Frame == 0:
				C.Blade(21, 16 + B, 25, 7 + B)
			elif Frame == 1:
				C.Blade(20, 19 + B, 7, 27)
			else:
				C.Blade(11, 22 + B, 4, 28)
		if Pose == "Dash":
			# 앞/뒤 대시: 목도리 끝이 양옆으로 날림
			C.Rect(21, 17, 23, 18, SCARF)
			C.Px(24, 18, SCARF_D)
	else:
		ArmDX, ArmDY, ScarfLen = 0, 0, 0
		if Pose == "Walk":
			ArmDX = (1, 0, -1, 0)[Frame % 4]
			ScarfLen = 1
		elif Pose == "Dash":
			ArmDX, ScarfLen = -2, 3
		elif Pose == "Attack":
			ArmDX, ArmDY = ((-2, -3), (3, -1), (2, 1))[Frame]
		if Pose == "Attack" and Frame == 0 and Weapon == "Sword":
			C.Blade(14, 14 + B, 9, 4 + B)  # 머리 뒤로 치켜든 칼 (몸 뒤)
		if Pose == "Attack" and Weapon == "Staff" and Frame == 0:
			DrawHeroWeapon(C, Weapon, "Side", Frame, B)  # 치켜든 지팡이 (몸 뒤)
		_HeroLegsSide(C, Legs)
		_HeroBodySide(C, B, ArmDX, ArmDY, True, ScarfLen)
		_HeroHeadSide(C, B)
		if Pose == "Attack" and Weapon != "Sword":
			if not (Weapon == "Staff" and Frame == 0):
				DrawHeroWeapon(C, Weapon, "Side", Frame, B)
		elif Pose == "Attack":
			if Frame == 1:
				C.Blade(19, 19 + B, 30, 18 + B)
			elif Frame == 2:
				C.Blade(18, 22 + B, 27, 29)
		if Pose == "Dash":
			# 몸을 앞으로 기울인 느낌: 윗몸 한 칸 앞으로 민다
			C.P[0:24, 1:] = np.where(C.P[0:24, :-1, 3:4] > 0, C.P[0:24, :-1], C.P[0:24, 1:])
	C.Outline()
	return C


HERO_POSES = {"Idle": 2, "Walk": 4, "Attack": 3, "Dash": 1}
HERO_DIRS = ("Down", "Up", "Side")


# ---- 슬라임 ---------------------------------------------------------------------------------------------------------------
SLIME_W, SLIME_H = 24, 20
# (가로 반지름, 세로 반지름) — 바닥은 항상 y = 18
SLIME_FRAMES = {
	"Idle": [(8.0, 6.0), (8.8, 5.4), (8.0, 6.0), (7.4, 6.6)],
	"Hop":  [(9.6, 4.6), (6.6, 8.0), (7.2, 7.2)],
}


def DrawSlime(RX, RY, bBlink=False):
	C = FCanvas(SLIME_W, SLIME_H)
	CX, Base = 12.0, 18.0
	CY = Base - RY
	C.Ellipse(CX, CY, RX, RY, SLIME)
	# 아래쪽 어둡게, 왼쪽 위 밝게
	for Y in range(SLIME_H):
		for X in range(SLIME_W):
			if C.P[Y, X, 3] == 0:
				continue
			DX, DY = (X + 0.5 - CX) / RX, (Y + 0.5 - CY) / RY
			if DY > 0.45 or (DY > 0.2 and abs(DX) > 0.75):
				C.Px(X, Y, SLIME_D)
			elif (DX + 0.35) ** 2 + (DY + 0.45) ** 2 < 0.12:
				C.Px(X, Y, SLIME_L)
	# 반짝임 + 눈 + 입
	C.Rect(int(CX - RX * 0.55), int(CY - RY * 0.6), int(CX - RX * 0.55) + 1, int(CY - RY * 0.6), WHITE)
	C.Px(int(CX - RX * 0.55), int(CY - RY * 0.6) + 1, WHITE)
	EyeY = int(round(CY - RY * 0.05))
	for EX in (int(round(CX - 3)), int(round(CX + 2))):
		if bBlink:
			C.Px(EX, EyeY + 1, SLIME_OUT)
		else:
			C.Rect(EX, EyeY, EX, EyeY + 1, SLIME_OUT)
	C.Px(int(round(CX - 0.5)), EyeY + 2, SLIME_D)
	C.Outline(SLIME_OUT)
	return C


# ---- 효과 ----------------------------------------------------------------------------------------------------------------
def DrawSlash(Frame):
	# 오른쪽을 향한 초승달 호: 프레임마다 위에서 아래로 쓸고(0~1) 뒤쪽부터 사라짐(2~3). 가산 블렌드로 그린다
	Size = 48
	C = FCanvas(Size, Size)
	Sweep = [(-80, -10), (-80, 55), (-55, 80), (10, 80)][Frame]
	R0, R1 = 13.0, 21.0
	for Y in range(Size):
		for X in range(Size):
			DX, DY = X + 0.5 - 22.0, Y + 0.5 - 24.0
			R = math.hypot(DX, DY)
			A = math.degrees(math.atan2(DY, DX))
			if not (Sweep[0] <= A <= Sweep[1]):
				continue
			T = (A - Sweep[0]) / max(Sweep[1] - Sweep[0], 1.0)  # 0 = 꼬리, 1 = 머리
			Thick = (R1 - R0) * (0.15 + 0.85 * math.sin(T * math.pi)) * (1.0, 1.0, 0.7, 0.4)[Frame]  # 가운데가 두꺼운 초승달, 끝으로 갈수록 가늘어짐
			Mid = (R0 + R1) * 0.5 + 2.0
			if abs(R - Mid) <= Thick * 0.5:
				Inner = abs(R - Mid) <= Thick * 0.22
				C.Px(X, Y, (255, 255, 236) if Inner else (255, 214, 120))
	return C


def DrawSpark(Frame):
	C = FCanvas(16, 16)
	Len = (3, 6, 7)[Frame]
	Col = ((255, 255, 255), (255, 236, 150), (255, 170, 80))[Frame]
	for DX, DY in ((1, 0), (-1, 0), (0, 1), (0, -1)):
		for I in range(Len if Frame < 2 else 2):
			Off = 0 if Frame < 2 else Len - 2
			C.Px(8 + DX * (I + Off), 8 + DY * (I + Off), Col)
	if Frame < 2:
		for DX, DY in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
			for I in range(1, Len // 2 + 1):
				C.Px(8 + DX * I, 8 + DY * I, Col)
	C.Rect(7, 7, 8, 8, WHITE if Frame < 2 else Col)
	return C


def _Puffs(C, Items, Col, ColD):
	for (X, Y, R) in Items:
		C.Ellipse(X, Y, R, R, Col)
	for (X, Y, R) in Items:
		for YY in range(C.H):
			for XX in range(C.W):
				if C.P[YY, XX, 3] and (XX + 0.5 - X) ** 2 + (YY + 0.5 - Y) ** 2 <= R * R and YY + 0.5 > Y + R * 0.35:
					C.Px(XX, YY, ColD)


def DrawDust(Frame):
	C = FCanvas(16, 12)
	R = (2.5, 3.5, 3.2, 2.2)[Frame]
	Spread = (2.0, 3.5, 5.0, 6.0)[Frame]
	Rise = (0.0, 1.0, 2.0, 3.0)[Frame]
	_Puffs(C, [(8 - Spread, 9 - Rise, R), (8 + Spread, 9 - Rise, R * 0.9), (8, 8 - Rise * 1.3, R * 0.8)], (236, 226, 204), (196, 180, 156))
	C.Outline((120, 104, 88))
	return C


def DrawPoof(Frame):
	C = FCanvas(32, 32)
	R = (3.0, 4.0, 4.0, 3.2, 2.0)[Frame]
	Ring = (4.0, 7.0, 9.5, 11.0, 12.0)[Frame]
	Items = [(16 + math.cos(math.radians(A)) * Ring, 17 + math.sin(math.radians(A)) * Ring * 0.8, R) for A in range(0, 360, 45)]
	if Frame < 2:
		Items.append((16, 17, R + 1.5))
	_Puffs(C, Items, (244, 240, 232), (196, 192, 204))
	C.Outline((96, 92, 110))
	return C


def DrawShadow():
	C = FCanvas(24, 10)
	C.Ellipse(12, 5, 11, 4.5, (20, 16, 28), 110)
	C.Ellipse(12, 5, 8, 3.2, (20, 16, 28), 150)
	return C


def DrawHeart(bFull):
	Shape = ["0110110",
			 "1111111",
			 "1111111",
			 "0111110",
			 "0011100",
			 "0001000"]
	C = FCanvas(9, 8)
	for Y, Row in enumerate(Shape):
		for X, Ch in enumerate(Row):
			if Ch == "1":
				C.Px(X + 1, Y + 1, (226, 44, 64) if bFull else (70, 50, 70))
	if bFull:
		C.Px(2, 2, (255, 200, 200))
	C.Outline((40, 16, 28))
	return C


# ---- 무기 (용사 공격 자세 — 검 외 3종) -------------------------------------------------------------------------------------
SHAFT     = (156, 102, 58)
SHAFT_D   = (104, 66, 40)
TASSEL    = (206, 52, 54)
BOW       = (170, 112, 60)
BOW_D     = (112, 70, 40)
STRING    = (236, 228, 210)
ORB       = (120, 220, 255)
ORB_L     = (230, 250, 255)
ORB_D     = (70, 120, 220)


def _Unit(X0, Y0, X1, Y1):
	L = max(math.hypot(X1 - X0, Y1 - Y0), 1e-6)
	return (X1 - X0) / L, (Y1 - Y0) / L


def SpearAt(C, X0, Y0, X1, Y1):
	# 자루(X0,Y0 → X1,Y1) + 창날(끝에서 3칸) + 붉은 술
	DX, DY = _Unit(X0, Y0, X1, Y1)
	C.Line(X0, Y0, X1, Y1, SHAFT)
	C.Px(X0, Y0, SHAFT_D)
	for I in range(1, 4):
		C.Px(X1 + DX * I, Y1 + DY * I, BLADE if I < 3 else BLADE_D)
	C.Px(X1 + DX - DY, Y1 + DY + DX, BLADE_D)
	C.Px(X1 + DX + DY, Y1 + DY - DX, BLADE_D)
	C.Px(X1 - DX - DY, Y1 - DY + DX, TASSEL)
	C.Px(X1 - DX * 2 - DY, Y1 - DY * 2 + DX, TASSEL)


def StaffAt(C, X0, Y0, X1, Y1, bGlow):
	# 지팡이: 자루 + 끝의 마법 구슬 (bGlow = 마법을 쏘는 순간 밝게)
	DX, DY = _Unit(X0, Y0, X1, Y1)
	C.Line(X0, Y0, X1, Y1, SHAFT_D)
	if abs(DX) < 0.5:
		C.Line(X0 + 1, Y0, X1 + 1, Y1, SHAFT)
	else:
		C.Line(X0, Y0 + 1, X1, Y1 + 1, SHAFT)
	OX, OY = int(round(X1 + DX * 2)), int(round(Y1 + DY * 2))
	C.Rect(OX - 1, OY - 1, OX + 1, OY + 1, ORB_D)
	C.Rect(OX - 1, OY - 1, OX, OY, ORB)
	C.Px(OX - 1, OY - 1, ORB_L)
	if bGlow:
		for GX, GY in ((-3, 0), (3, 0), (0, -3), (0, 3)):
			C.Px(OX + GX, OY + GY, ORB_L)


def BowAt(C, CX, CY, Half, bVertical, Facing, bArrow, Pull=2):
	# 활: 둥근 활대(Half = 반 길이) + 시위 (+ 당긴 화살). Facing = 활이 휜 쪽 (+1/-1)
	Pts = []
	for T in range(-Half, Half + 1):
		Bulge = int(round((1.0 - (T / Half) ** 2) * 2.0)) * Facing
		Pts.append((CX + Bulge, CY + T) if bVertical else (CX + T, CY + Bulge))
	for I, (X, Y) in enumerate(Pts):
		C.Px(X, Y, BOW if 0 < I < len(Pts) - 1 else BOW_D)
	(AX, AY), (BX, BY) = Pts[0], Pts[-1]
	if bArrow:
		MX, MY = ((CX - Pull * Facing, CY) if bVertical else (CX, CY - Pull * Facing))
		C.Line(AX, AY, MX, MY, STRING)
		C.Line(MX, MY, BX, BY, STRING)
		if bVertical:
			C.Line(MX, MY, CX + 4 * Facing, CY, SHAFT)
			C.Px(CX + 5 * Facing, CY, BLADE)
		else:
			C.Line(MX, MY, CX, CY + 4 * Facing, SHAFT)
			C.Px(CX, CY + 5 * Facing, BLADE)
	else:
		C.Line(AX, AY, BX, BY, STRING)


def DrawHeroWeapon(C, Weapon, Dir, Frame, B):
	if Weapon == "Spear":
		Spears = {"Side": ((8, 20, 20, 20), (14, 20, 27, 20), (12, 20, 25, 20)),
				  "Down": ((21, 23, 21, 9), (19, 14, 19, 27), (19, 13, 19, 26)),
				  "Up":   ((21, 24, 21, 10), (17, 18, 17, 4), (17, 19, 17, 5))}
		X0, Y0, X1, Y1 = Spears[Dir][Frame]
		O = B if Dir == "Side" else 0
		SpearAt(C, X0, Y0 + O, X1, Y1 + O)
	elif Weapon == "Bow":
		if Dir == "Side":
			BowAt(C, 22, 19 + B, 7, True, 1, Frame == 0, 3)
		elif Dir == "Down":
			BowAt(C, 16, 25, 6, False, 1, Frame == 0, 2)
		else:
			BowAt(C, 16, 7, 6, False, -1, Frame == 0, 2)
	elif Weapon == "Staff":
		Staffs = {"Side": ((17, 25, 13, 7), (15, 22, 25, 14), (15, 22, 25, 15)),
				  "Down": ((22, 25, 22, 8), (20, 16, 20, 26), (20, 16, 20, 25)),
				  "Up":   ((22, 25, 22, 8), (17, 17, 17, 4), (17, 18, 17, 5))}
		X0, Y0, X1, Y1 = Staffs[Dir][Frame]
		StaffAt(C, X0, Y0, X1, Y1, Frame == 1)


# ---- 공통 도형 도우미 -------------------------------------------------------------------------------------------------------
def Poly(C, Points, Col, A=255):
	# 다각형 채우기 (화소 가운데 기준 짝홀 규칙)
	Xs = [P[0] for P in Points]
	Ys = [P[1] for P in Points]
	for Y in range(max(int(min(Ys)), 0), min(int(max(Ys)) + 1, C.H)):
		for X in range(max(int(min(Xs)), 0), min(int(max(Xs)) + 1, C.W)):
			PX, PY = X + 0.5, Y + 0.5
			Inside = False
			J = len(Points) - 1
			for I in range(len(Points)):
				XI, YI = Points[I]
				XJ, YJ = Points[J]
				if (YI > PY) != (YJ > PY) and PX < (XJ - XI) * (PY - YI) / (YJ - YI) + XI:
					Inside = not Inside
				J = I
			if Inside:
				C.Px(X, Y, Col, A)


def ShadeBottom(C, Col, Below):
	# 불투명 화소 중 Y >= Below인 것을 어둡게 칠한다 (아래쪽 그늘)
	for Y in range(Below, C.H):
		for X in range(C.W):
			if C.P[Y, X, 3] > 0:
				C.Px(X, Y, Col)


# ---- 박쥐 (정면, 날갯짓) ----------------------------------------------------------------------------------------------------
BAT, BAT_D, BAT_L = (98, 66, 128), (60, 40, 86), (146, 110, 178)
WING, WING_D = (74, 50, 102), (48, 32, 70)
RED_EYE = (255, 74, 74)


def DrawBat(Frame):
	# Frame 0~2 날갯짓(위·가운데·아래), 3 급강하(날개 접음)
	C = FCanvas(28, 20)
	CX = 14
	if Frame < 3:
		TipY = (2, 8, 14)[Frame]
		for S in (-1, 1):
			Wing = [(CX + S * 3, 8), (CX + S * 6, 5 + (TipY - 2) * 0.3), (CX + S * 12, TipY), (CX + S * 10, TipY + 3),
					(CX + S * 8, TipY + 1), (CX + S * 6, TipY + 4), (CX + S * 4, 12)]
			Poly(C, Wing, WING)
			C.Line(CX + S * 3, 8, CX + S * 12, TipY, BAT_L)
			C.Line(CX + S * 6, 7 + (TipY - 8) * 0.4, CX + S * 6, TipY + 3, WING_D)
	else:
		for S in (-1, 1):
			Poly(C, [(CX + S * 3, 7), (CX + S * 7, 1), (CX + S * 6, 9), (CX + S * 3, 13)], WING)
			C.Line(CX + S * 3, 7, CX + S * 7, 1, BAT_L)
	C.Ellipse(CX, 10, 4.2, 4.6, BAT)
	C.Ellipse(CX - 0.8, 9, 2.2, 2.4, BAT_L)
	C.Px(CX - 3, 5, BAT)
	C.Px(CX - 3, 4, BAT_D)
	C.Px(CX + 2, 5, BAT)
	C.Px(CX + 2, 4, BAT_D)
	C.Px(CX - 2, 9, RED_EYE)
	C.Px(CX + 1, 9, RED_EYE)
	C.Px(CX - 1, 12, WHITE)
	C.Px(CX, 12, WHITE)
	ShadeBottom(C, BAT_D, 13)
	C.Outline((26, 16, 34))
	return C


# ---- 고블린 도적 (옆모습, 오른쪽을 봄) ------------------------------------------------------------------------------------------
GOB, GOB_D, GOB_L = (120, 172, 74), (76, 120, 52), (166, 208, 104)
LEATHER, LEATHER_D = (128, 86, 52), (86, 56, 36)
BANDANA, BANDANA_D = (186, 44, 52), (122, 28, 40)
DAGGER = (214, 222, 236)


def DrawGoblin(Pose, Frame):
	# Pose: Idle(2) / Walk(4) / Windup(1) / Attack(2)
	C = FCanvas(30, 28)
	B = (Frame % 2) if Pose == "Idle" else ((0, 1, 0, 1)[Frame % 4] if Pose == "Walk" else 0)
	Lean = {"Windup": -2, "Attack": 3}.get(Pose, 0)
	Crouch = 2 if Pose == "Windup" else 0
	Base = 26
	Step = (2, 0, -2, 0)[Frame % 4] if Pose == "Walk" else (3 if Pose == "Attack" else 0)
	Back = Step - (Lean if Pose == "Attack" else 0)  # 찌를 때 뒷다리는 몸 밑에 남는다
	C.Rect(12 - Back, 21, 13 - Back, Base - 1, GOB_D)
	C.Rect(11 - Back, Base - 1, 13 - Back, Base, LEATHER_D)
	C.Rect(16 + Step, 21, 17 + Step, Base - 1, GOB)
	C.Rect(16 + Step, Base - 1, 18 + Step, Base, LEATHER)
	# 몸 (가죽 조끼)
	T = 13 + B + Crouch
	C.Rect(11 + Lean, T, 18 + Lean, 22, LEATHER)
	C.Rect(11 + Lean, T, 12 + Lean, 22, LEATHER_D)
	C.Rect(11 + Lean, 20, 18 + Lean, 20, (60, 40, 26))
	C.Px(17 + Lean, 20, BUCKLE)
	# 머리 (큰 머리 + 뒤로 뻗은 귀 + 매부리코 + 두건)
	H = 3 + B + Crouch
	HX = 12 + Lean
	C.Ellipse(HX + 4.5, H + 5.5, 5.2, 5.0, GOB)
	C.Ellipse(HX + 3.5, H + 4.0, 2.6, 2.0, GOB_L)
	Poly(C, [(HX + 0, H + 5), (HX - 5, H + 2), (HX - 3, H + 6), (HX + 1, H + 8)], GOB_D)
	C.Rect(HX + 9, H + 6, HX + 10, H + 7, GOB)
	C.Px(HX + 11, H + 7, GOB_D)
	C.Rect(HX + 1, H + 0, HX + 8, H + 2, BANDANA)
	C.Rect(HX + 1, H + 2, HX + 8, H + 2, BANDANA_D)
	C.Rect(HX - 2, H + 2, HX, H + 3, BANDANA)
	C.Px(HX - 3, H + 4, BANDANA_D)
	C.Rect(HX + 7, H + 4, HX + 7, H + 5, (255, 236, 90))
	C.Px(HX + 8, H + 9, (52, 30, 30))
	C.Px(HX + 7, H + 9, WHITE)
	# 팔 + 단검
	if Pose == "Windup":
		C.Rect(HX - 1, T, HX + 0, T + 3, GOB)
		C.Line(HX - 1, T - 1, HX - 5, T - 5, DAGGER)
		C.Px(HX - 1, T - 1, BUCKLE)
	elif Pose == "Attack":
		C.Rect(HX + 6, T + 3, HX + 9, T + 4, GOB)
		C.Line(HX + 10, T + 3, HX + 15 + Frame, T + 3, DAGGER)
		C.Px(HX + 10, T + 3, BUCKLE)
	else:
		C.Rect(HX + 4, T + 1, HX + 5, T + 5 + B, GOB)
		C.Line(HX + 6, T + 5, HX + 9, T + 3, DAGGER)
	C.Outline((24, 34, 20))
	return C


# ---- 해골 궁수 (옆모습, 오른쪽을 봄) ------------------------------------------------------------------------------------------
BONE, BONE_D, BONE_DD = (234, 228, 208), (176, 166, 146), (112, 102, 94)
HOOD, HOOD_D = (70, 80, 110), (44, 50, 74)


def DrawArcher(Pose, Frame):
	# Pose: Idle(2) / Walk(4) / Draw(2) / Shoot(1)
	C = FCanvas(30, 32)
	B = (Frame % 2) if Pose == "Idle" else ((0, 1, 0, 1)[Frame % 4] if Pose == "Walk" else 0)
	Step = (2, 0, -2, 0)[Frame % 4] if Pose == "Walk" else 0
	# 뼈 다리
	C.Line(13, 22, 12 - Step, 29, BONE_D)
	C.Rect(11 - Step, 30, 13 - Step, 30, BONE_DD)
	C.Line(16, 22, 17 + Step, 29, BONE)
	C.Rect(16 + Step, 30, 18 + Step, 30, BONE_D)
	# 골반 + 갈비
	C.Rect(12, 21 + B, 17, 22 + B, BONE_D)
	C.Line(14, 14 + B, 14, 21 + B, BONE)
	for Y in (15, 17, 19):
		C.Line(12, Y + B, 17, Y + B, BONE if Y < 19 else BONE_D)
	# 너덜너덜한 두건 망토
	Poly(C, [(10, 12 + B), (13, 10 + B), (13, 22 + B), (9, 24 + B), (10, 17 + B)], HOOD)
	C.Line(9, 24 + B, 10, 13 + B, HOOD_D)
	# 해골
	H = 3 + B
	C.Ellipse(16, H + 5, 5.0, 5.2, BONE)
	C.Rect(15, H + 9, 20, H + 11, BONE)
	C.Rect(15, H + 11, 20, H + 11, BONE_D)
	C.Rect(18, H + 4, 19, H + 6, (30, 20, 30))
	C.Px(18, H + 5, (255, 90, 70))
	C.Px(16, H + 9, BONE_DD)
	C.Px(18, H + 10, BONE_DD)
	Poly(C, [(10, H + 1), (17, H - 1), (21, H + 2), (12, H + 6)], HOOD)
	C.Rect(10, H + 5, 12, H + 10, HOOD_D)
	# 팔 + 활
	if Pose in ("Draw", "Shoot"):
		C.Line(16, 15 + B, 21, 15 + B, BONE)
		Pull = (2, 4)[Frame] if Pose == "Draw" else 0
		BowAt(C, 23, 15 + B, 8, True, 1, Pose == "Draw", Pull)
		if Pose == "Draw":
			C.Line(17, 16 + B, 23 - Pull, 15 + B, BONE_D)
	else:
		C.Line(15, 15 + B, 17, 20 + B, BONE)
		BowAt(C, 19, 19 + B, 7, True, 1, False)
	C.Outline((30, 26, 36))
	return C


# ---- 독버섯 (좌우 대칭) ------------------------------------------------------------------------------------------------------------
CAP, CAP_L, CAP_D = (150, 74, 170), (204, 128, 220), (98, 46, 118)
SPOT = (222, 242, 140)
STEM, STEM_D = (238, 224, 198), (190, 170, 148)


def DrawMushroom(Pose, Frame):
	# Pose: Idle(2) / Walk(2) / Windup(1) / Puff(1)
	C = FCanvas(26, 26)
	Squash = {"Idle": (0, 1)[Frame % 2], "Walk": (0, 1)[Frame % 2], "Windup": -2, "Puff": 3}[Pose]
	Swell = 2 if Pose == "Windup" else 0
	Base = 24
	FX = (1 if Frame == 0 else -1) if Pose == "Walk" else 0
	C.Rect(9 + FX, Base - 1, 11 + FX, Base, STEM_D)
	C.Rect(15 - FX, Base - 1, 17 - FX, Base, STEM_D)
	# 줄기 (얼굴)
	C.Rect(9, 14 + Squash, 17, Base - 2, STEM)
	C.Rect(9, Base - 3, 17, Base - 2, STEM_D)
	C.Rect(17, 14 + Squash, 17, Base - 2, STEM_D)
	EY = 17 + Squash
	C.Rect(11, EY, 11, EY + 1, EYE)
	C.Rect(15, EY, 15, EY + 1, EYE)
	C.Px(10, EY + 2, BLUSH)
	C.Px(16, EY + 2, BLUSH)
	C.Rect(12, EY + 3, 14, EY + 3, (120, 60, 70) if Pose != "Puff" else EYE)
	# 갓
	CY = 9 + Squash
	RX, RY = 10.5 + Swell, 6.5 + Swell * 0.6
	C.Ellipse(13, CY, RX, RY, CAP)
	C.Rect(int(13 - RX), int(CY + 2), int(13 + RX), int(CY + 4), CAP_D)
	C.Ellipse(10, CY - 2.5, 4.0, 2.2, CAP_L)
	for SX, SY, R in ((8, CY - 1, 1.6), (15, CY - 3, 1.9), (19, CY + 0, 1.3), (12, CY + 1, 1.1)):
		C.Ellipse(SX, SY, R, R, SPOT)
	C.Outline((40, 20, 48))
	return C


# ---- 바위 골렘 (정면, 보스) ------------------------------------------------------------------------------------------------------
STONE, STONE_L, STONE_D, STONE_DD = (138, 128, 120), (178, 168, 154), (94, 86, 84), (62, 58, 62)
MOSS, MOSS_L = (98, 142, 66), (146, 186, 90)
CORE, CORE_L = (255, 162, 60), (255, 236, 160)


def _StoneBlock(C, X0, Y0, X1, Y1, Seed):
	Rng = np.random.default_rng(Seed)
	C.Rect(X0, Y0, X1, Y1, STONE)
	C.Rect(X0, Y0, X1, Y0, STONE_L)
	C.Rect(X0, Y0, X0, Y1, STONE_L)
	C.Rect(X0, Y1, X1, Y1, STONE_D)
	C.Rect(X1, Y0, X1, Y1, STONE_D)
	for _ in range(max(1, (X1 - X0) * (Y1 - Y0) // 18)):
		X, Y = int(Rng.integers(X0 + 1, max(X0 + 2, X1))), int(Rng.integers(Y0 + 1, max(Y0 + 2, Y1)))
		C.Px(X, Y, STONE_D if Rng.random() < 0.6 else STONE_L)


def DrawGolem(Pose, Frame):
	# Pose: Dormant(1) / Idle(2) / Walk(4) / Raise(1) / Slam(1) / Throw(1)
	C = FCanvas(64, 72)  # 위 8칸 = 치켜든 팔·던질 바위 자리
	B = (Frame % 2) if Pose == "Idle" else ((0, 1, 0, 1)[Frame % 4] if Pose == "Walk" else 0)
	Sway = (-1, 0, 1, 0)[Frame % 4] if Pose == "Walk" else 0
	Low = 8 + 8 if Pose == "Dormant" else (8 + 3 if Pose == "Slam" else 8)
	Base = 70
	# 다리
	if Pose != "Dormant":
		LL = (2, 0, 0, 0)[Frame % 4] if Pose == "Walk" else 0
		RL = (0, 0, 2, 0)[Frame % 4] if Pose == "Walk" else 0
		_StoneBlock(C, 20, 48 + 8, 28, Base - LL, 11)
		_StoneBlock(C, 36, 48 + 8, 44, Base - RL, 12)
	# 몸통
	TY = 22 + B + Low
	_StoneBlock(C, 16 + Sway, TY, 48 + Sway, 50 + Low, 13)  # Low에 위 여백 8 포함
	_StoneBlock(C, 19 + Sway, TY + 2, 31 + Sway, TY + 12, 14)
	_StoneBlock(C, 33 + Sway, TY + 2, 45 + Sway, TY + 12, 15)
	# 가슴 핵 (빛남)
	C.Ellipse(32 + Sway, TY + 18, 4.0, 3.6, CORE if Pose != "Dormant" else (110, 80, 60))
	if Pose != "Dormant":
		C.Ellipse(31 + Sway, TY + 17, 1.8, 1.6, CORE_L)
	# 이끼 (어깨·머리)
	for MX, MY, R in ((18, TY, 3.2), (46, TY + 1, 3.0), (25, TY - 1, 2.2), (40, TY - 1, 2.0)):
		C.Ellipse(MX + Sway, MY, R, R * 0.7, MOSS)
		C.Px(MX + Sway - 1, MY - 1, MOSS_L)
	# 머리
	HY = TY - 12 + (2 if Pose == "Slam" else 0)
	_StoneBlock(C, 25 + Sway, HY, 39 + Sway, TY + 1, 16)
	EyeC = CORE_L if Pose != "Dormant" else STONE_DD
	C.Rect(28 + Sway, HY + 5, 30 + Sway, HY + 6, EyeC)
	C.Rect(34 + Sway, HY + 5, 36 + Sway, HY + 6, EyeC)
	if Pose in ("Raise", "Slam", "Throw"):
		C.Rect(27 + Sway, HY + 3, 30 + Sway, HY + 3, STONE_DD)  # 찌푸린 눈썹
		C.Rect(34 + Sway, HY + 3, 37 + Sway, HY + 3, STONE_DD)
	# 팔
	if Pose == "Raise":
		_StoneBlock(C, 6, TY - 18, 15, TY + 6, 17)
		_StoneBlock(C, 49, TY - 18, 58, TY + 6, 18)
		_StoneBlock(C, 4, TY - 26, 17, TY - 16, 19)
		_StoneBlock(C, 47, TY - 26, 60, TY - 16, 20)
	elif Pose == "Slam":
		_StoneBlock(C, 4, TY + 4, 15, TY + 26, 17)
		_StoneBlock(C, 49, TY + 4, 60, TY + 26, 18)
		_StoneBlock(C, 1, Base - 9, 16, Base, 19)
		_StoneBlock(C, 48, Base - 9, 63, Base, 20)
	elif Pose == "Throw":
		_StoneBlock(C, 6, TY + 2, 15, TY + 24, 17)
		_StoneBlock(C, 4, TY + 22, 16, TY + 32, 19)
		_StoneBlock(C, 49, TY - 18, 58, TY + 6, 18)
		_StoneBlock(C, 47, TY - 26, 60, TY - 16, 20)
		C.Ellipse(54, TY - 24, 6, 5, STONE_D)  # 주먹에 쥔 바위
		C.Ellipse(53, TY - 25, 3, 2.5, STONE_L)
	else:
		AY = TY + 2 + ((Low - 8) // 2)
		_StoneBlock(C, 6 + Sway, AY, 15 + Sway, AY + 22, 17)
		_StoneBlock(C, 49 + Sway, AY, 58 + Sway, AY + 22, 18)
		_StoneBlock(C, 4 + Sway, AY + 20, 16 + Sway, AY + 30 - ((Low - 8) // 2), 19)
		_StoneBlock(C, 48 + Sway, AY + 20, 60 + Sway, AY + 30 - ((Low - 8) // 2), 20)
	C.Outline((30, 26, 34))
	return C


# ---- 마을 사람 (정면 대기, 2프레임) ------------------------------------------------------------------------------------------
NPC_STYLES = {
	# 머리색(밝/보통/어두움), 옷(밝/보통/어두움), 특징
	"Elder":    {"Hair": ((236, 236, 236), (206, 204, 210), (150, 148, 160)), "Cloth": ((150, 110, 196), (112, 76, 156), (74, 48, 108)), "Beard": True, "Cane": True},
	"Merchant": {"Hair": ((196, 120, 66), (150, 86, 46), (98, 54, 34)), "Cloth": ((120, 176, 96), (82, 136, 70), (52, 94, 50)), "Scarf": (236, 196, 92), "Apron": True},
	"Girl":     {"Hair": ((255, 176, 96), (232, 128, 60), (170, 84, 40)), "Cloth": ((255, 160, 180), (226, 106, 140), (160, 64, 100)), "Tails": True},
	"Guard":    {"Hair": ((206, 214, 226), (150, 160, 178), (96, 104, 124)), "Cloth": ((200, 70, 70), (156, 44, 50), (104, 28, 38)), "Helmet": True, "Spear": True},
}


def DrawVillager(Style, Frame):
	St = NPC_STYLES[Style]
	HL, HM, HD = St["Hair"]
	CL, CM, CD = St["Cloth"]
	C = FCanvas(HERO_CELL, HERO_CELL)
	B = Frame % 2
	if St.get("Spear"):
		SpearAt(C, 23, 30, 23, 3)
	# 다리 + 신발 (긴 옷이면 치마)
	if St.get("Apron") or St.get("Beard") or St.get("Tails"):
		Poly(C, [(11, 21), (20, 21), (22, 29), (9, 29)], CM)
		C.Rect(9, 28, 22, 29, CD)
		C.Rect(11, 30, 13, 30, BOOT_D)
		C.Rect(18, 30, 20, 30, BOOT_D)
	else:
		_HeroLegsFront(C, 0)
	# 몸
	C.Rect(12, 17 + B, 19, 24, CM)
	C.Rect(12, 17 + B, 12, 24, CL)
	C.Rect(19, 17 + B, 19, 24, CD)
	C.Rect(10, 17 + B, 11, 22 + B, CM)
	C.Rect(20, 17 + B, 21, 22 + B, CD)
	C.Rect(10, 23 + B, 11, 23 + B, SKIN)
	C.Rect(20, 23 + B, 21, 23 + B, SKIN)
	if St.get("Apron"):
		C.Rect(13, 19 + B, 18, 27, (236, 228, 208))
		C.Rect(13, 19 + B, 18, 19 + B, (196, 186, 166))
	if St.get("Scarf"):
		C.Rect(11, 16 + B, 20, 17 + B, St["Scarf"])
	if St.get("Cane"):
		C.Line(8, 20 + B, 8, 30, SHAFT)
		C.Rect(7, 19 + B, 9, 19 + B, SHAFT_D)
	# 머리
	Top = 3 + B
	C.Rect(11, Top + 5, 20, Top + 12, SKIN)
	C.Rect(10, Top + 6, 10, Top + 10, SKIN)
	C.Rect(21, Top + 6, 21, Top + 10, SKIN)
	C.Rect(11, Top + 12, 20, Top + 12, SKIN_D)
	if St.get("Helmet"):
		C.Rect(10, Top, 21, Top + 5, HM)
		C.Rect(11, Top - 1, 20, Top, HL)
		C.Rect(10, Top + 5, 21, Top + 5, HD)
		C.Rect(15, Top - 3, 16, Top - 1, (206, 52, 54))
	else:
		C.Rect(11, Top, 20, Top + 4, HM)
		C.Rect(10, Top + 1, 21, Top + 5, HM)
		C.Rect(10, Top + 6, 10, Top + 9, HD)
		C.Rect(21, Top + 6, 21, Top + 9, HD)
		C.Rect(13, Top + 1, 16, Top + 1, HL)
	if St.get("Tails"):
		C.Rect(7, Top + 4, 9, Top + 10, HM)
		C.Rect(22, Top + 4, 24, Top + 10, HM)
		C.Px(8, Top + 11, HD)
		C.Px(23, Top + 11, HD)
	if St.get("Apron") and St.get("Scarf"):
		C.Rect(11, Top - 1, 20, Top + 1, St["Scarf"])
	C.Rect(13, Top + 8, 13, Top + 9, EYE)
	C.Rect(18, Top + 8, 18, Top + 9, EYE)
	if St.get("Beard"):
		Poly(C, [(11, Top + 10), (20, Top + 10), (18, Top + 16 + B), (13, Top + 16 + B)], HL)
		C.Rect(13, Top + 10, 18, Top + 10, HM)
		C.Rect(12, Top + 7, 14, Top + 7, HL)
		C.Rect(17, Top + 7, 19, Top + 7, HL)
	else:
		C.Px(12, Top + 10, BLUSH)
		C.Px(19, Top + 10, BLUSH)
		C.Rect(15, Top + 11, 16, Top + 11, SKIN_D)
	C.Outline()
	return C


# ---- 보물상자 / 줍는 것 ----------------------------------------------------------------------------------------------------------
WOOD, WOOD_L, WOOD_D = (156, 92, 48), (196, 128, 70), (102, 58, 34)
GOLD, GOLD_L, GOLD_D = (244, 196, 70), (255, 240, 160), (176, 120, 40)


def DrawChest(Frame):
	# 0 닫힘, 1 뚜껑 반, 2 열림 (안쪽 금빛)
	C = FCanvas(24, 22)
	Base = 21
	C.Rect(3, 11, 20, Base, WOOD)
	C.Rect(3, 11, 20, 12, WOOD_L)
	C.Rect(3, Base - 1, 20, Base, WOOD_D)
	for X in (3, 11, 12, 20):
		C.Rect(X, 11, X, Base, GOLD_D if X in (11, 12) else GOLD)
	C.Rect(3, 15, 20, 15, GOLD_D)
	if Frame == 0:
		C.Rect(3, 5, 20, 10, WOOD)
		C.Rect(4, 4, 19, 4, WOOD_L)
		C.Rect(3, 10, 20, 10, WOOD_D)
		C.Rect(3, 5, 3, 10, GOLD)
		C.Rect(20, 5, 20, 10, GOLD)
		C.Rect(10, 8, 13, 12, GOLD)
		C.Rect(11, 10, 12, 11, (60, 40, 30))
	elif Frame == 1:
		C.Rect(3, 4, 20, 8, WOOD_D)
		C.Rect(4, 3, 19, 3, WOOD)
		C.Rect(4, 9, 19, 10, GOLD_L)
	else:
		C.Rect(3, 1, 20, 6, WOOD_D)
		C.Rect(4, 0, 19, 0, WOOD)
		C.Rect(3, 1, 3, 6, GOLD_D)
		C.Rect(20, 1, 20, 6, GOLD_D)
		C.Rect(4, 7, 19, 11, GOLD_L)
		C.Rect(6, 9, 9, 11, GOLD)
		C.Rect(13, 8, 17, 11, GOLD)
	C.Outline((40, 22, 18))
	return C


def DrawCoin(Frame):
	C = FCanvas(10, 10)
	W = (3.6, 2.4, 0.9, 2.4)[Frame]
	C.Ellipse(5, 5, W, 3.8, GOLD)
	if W > 1.5:
		C.Ellipse(5, 5, W * 0.55, 2.2, GOLD_D)
		C.Px(4, 3, GOLD_L)
	C.Outline((90, 56, 20))
	return C


def DrawBag():
	C = FCanvas(12, 12)
	C.Ellipse(6, 7.5, 4.5, 3.8, (190, 150, 100))
	C.Ellipse(5, 6.5, 2, 1.6, (226, 196, 150))
	C.Rect(4, 2, 7, 3, (190, 150, 100))
	C.Rect(4, 4, 7, 4, (206, 52, 54))
	C.Outline((60, 40, 24))
	return C


# ---- 효과 추가 ---------------------------------------------------------------------------------------------------------------
def DrawThrust(Frame):
	# 오른쪽을 향한 찌르기 궤적 (가산): 0 짧게 → 1·2 길게 → 3 가늘게 사라짐
	C = FCanvas(48, 12)
	Len = (20, 40, 44, 44)[Frame]
	Thick = (2.5, 3.5, 2.5, 1.2)[Frame]
	for X in range(48 - Len, 48):
		T = (X - (48 - Len)) / max(Len - 1, 1)
		H = Thick * (0.3 + 0.7 * T)
		for Y in range(12):
			D = abs(Y + 0.5 - 6.0)
			if D <= H:
				C.Px(X, Y, (255, 255, 240) if D <= H * 0.4 else (150, 210, 255))
	return C


def DrawBolt(Frame):
	C = FCanvas(14, 14)
	R = (4.6, 5.2)[Frame]
	C.Ellipse(7, 7, R, R, (90, 120, 255))
	C.Ellipse(7, 7, R * 0.7, R * 0.7, (140, 200, 255))
	C.Ellipse(7, 7, R * 0.35, R * 0.35, WHITE)
	for A in range(0, 360, 90):
		Ang = math.radians(A + Frame * 45)
		C.Px(7 + math.cos(Ang) * (R + 1.5), 7 + math.sin(Ang) * (R + 1.5), (200, 230, 255))
	return C


def DrawBurst(Frame):
	C = FCanvas(36, 36)
	Ring = (5.0, 9.0, 12.5, 15.0, 16.5)[Frame]
	Width = (4.0, 3.5, 2.5, 1.6, 0.9)[Frame]
	Col = ((255, 255, 255), (190, 230, 255), (130, 170, 255), (110, 120, 240), (90, 80, 200))[Frame]
	for Y in range(36):
		for X in range(36):
			D = math.hypot(X + 0.5 - 18, Y + 0.5 - 18)
			if abs(D - Ring) <= Width * 0.5 or (Frame < 2 and D < Ring - Width):
				C.Px(X, Y, Col)
	for A in range(0, 360, 45):
		Ang = math.radians(A + 22.5)
		R = Ring + 2 + Frame
		C.Px(18 + math.cos(Ang) * R, 18 + math.sin(Ang) * R, (255, 255, 220))
	return C


def DrawArrow():
	C = FCanvas(18, 5)
	C.Line(2, 2, 14, 2, SHAFT)
	C.Rect(14, 1, 15, 3, BLADE_D)
	C.Px(16, 2, BLADE)
	C.Px(1, 1, WHITE)
	C.Px(1, 3, WHITE)
	C.Px(2, 1, (206, 52, 54))
	C.Px(2, 3, (206, 52, 54))
	return C


def DrawRock(Frame):
	C = FCanvas(16, 14)
	Pts = [(3, 5), (7, 1), (12, 2), (15, 7), (12, 12), (5, 13), (1, 9)]
	if Frame == 1:
		Pts = [(16 - X, Y) for X, Y in Pts]
	Poly(C, Pts, STONE)
	C.Ellipse(7, 5, 2.5, 1.6, STONE_L)
	ShadeBottom(C, STONE_D, 10)
	C.Outline((30, 26, 34))
	return C


def DrawRing(Col=(255, 230, 170), Size=48, Width=2.2):
	C = FCanvas(Size, Size)
	R = Size * 0.5 - 3
	for Y in range(Size):
		for X in range(Size):
			D = math.hypot(X + 0.5 - Size * 0.5, Y + 0.5 - Size * 0.5)
			if abs(D - R) <= Width:
				C.Px(X, Y, WHITE if abs(D - R) <= Width * 0.4 else Col)
	return C


def DrawWarn():
	# 바닥 경고 원 (점선 + 옅은 안쪽) — 눕혀서 쓴다
	C = FCanvas(40, 40)
	for Y in range(40):
		for X in range(40):
			D = math.hypot(X + 0.5 - 20, Y + 0.5 - 20)
			A = math.degrees(math.atan2(Y + 0.5 - 20, X + 0.5 - 20))
			if abs(D - 18) <= 1.3 and int((A + 180) / 15) % 2 == 0:
				C.Px(X, Y, (255, 80, 60))
			elif D < 16.5:
				C.Px(X, Y, (255, 60, 40), 70)
	return C


def DrawPoison(Frame):
	# 바닥 독 웅덩이 (위에서 본 원 — 눕혀서 쓴다), 거품 위치가 프레임마다 다름
	C = FCanvas(40, 40)
	C.Ellipse(20, 20, 18, 18, (120, 60, 150), 170)
	C.Ellipse(20, 20, 14, 14, (160, 90, 190), 190)
	C.Ellipse(17, 16, 6, 4, (200, 140, 230), 200)
	for (BX, BY) in (((10, 22), (26, 12), (24, 28)), ((14, 12), (28, 22), (18, 29)))[Frame]:
		C.Ellipse(BX, BY, 2.2, 2.2, (230, 250, 150), 230)
		C.Px(BX - 1, BY - 1, WHITE)
	return C


def DrawSparkle(Frame):
	C = FCanvas(24, 24)
	Stars = [(12, 12, (3, 6, 8, 4)[Frame]), (5, 6, (0, 2, 3, 2)[Frame]), (19, 5, (2, 3, 2, 0)[Frame]), (18, 19, (0, 3, 4, 2)[Frame]), (5, 18, (2, 2, 0, 0)[Frame])]
	for X, Y, L in Stars:
		if L <= 0:
			continue
		for I in range(-L, L + 1):
			Col = WHITE if abs(I) <= L // 3 else (255, 236, 150)
			C.Px(X + I, Y, Col)
			C.Px(X, Y + I, Col)
		if L >= 3:
			for I in (-1, 1):
				C.Px(X + I, Y + I, (255, 236, 150))
				C.Px(X + I, Y - I, (255, 236, 150))
	return C


def DrawPillar():
	C = FCanvas(20, 64)
	for Y in range(64):
		Fade = Y / 63.0
		for X in range(20):
			D = abs(X + 0.5 - 10) / 10.0
			A = (1.0 - D * D) * Fade
			if A > 0.05:
				C.Px(X, Y, (255, 230, 150) if D > 0.35 else (255, 255, 230), int(255 * min(1.0, A * 1.3)))
	return C


def DrawExclaim():
	C = FCanvas(8, 14)
	C.Rect(3, 1, 4, 8, (255, 220, 70))
	C.Rect(3, 1, 3, 8, (255, 246, 170))
	C.Rect(3, 10, 4, 11, (255, 220, 70))
	C.Outline((70, 40, 10))
	return C


def DrawBar(W, H, Col, Edge=None):
	C = FCanvas(W, H)
	C.Rect(0, 0, W - 1, H - 1, Col)
	if Edge:
		C.Outline(Edge)
	return C


def DrawAlert(Frame):
	# 적 공격 예고: 머리 위 붉은 번쩍 (3프레임)
	C = FCanvas(14, 14)
	R = (2.5, 4.5, 6.0)[Frame]
	for A in range(0, 360, 45):
		Ang = math.radians(A)
		C.Line(7, 7, 7 + math.cos(Ang) * R, 7 + math.sin(Ang) * R, (255, 90, 60) if Frame < 2 else (255, 170, 90))
	C.Rect(6, 6, 7, 7, WHITE)
	return C


# ---- UI 텍스처 (UI 샘플러는 선형이라 미리 정수배 최근접 확대) -------------------------------------------------------------
UI_SCALE = 4
FRAME_FILL = (16, 18, 38)


def UpscaleSave(Canvas, Path, Scale=UI_SCALE):
	Img = Image.fromarray(Canvas.P, "RGBA").resize((Canvas.W * Scale, Canvas.H * Scale), Image.NEAREST)
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	Img.save(Path, optimize=True)


def DrawFrame9(bLight=False):
	# 24x24 9-슬라이스 창틀: 어두운 남색 반투명 안 + 금색 이중 테두리 + 네 귀퉁이 마름모 장식 (가장자리 = 8 도트 → Margin 1/3)
	C = FCanvas(24, 24)
	C.Rect(2, 2, 21, 21, FRAME_FILL if not bLight else (44, 36, 66), 214 if not bLight else 235)
	for (X0, Y0, X1, Y1, Col) in ((1, 1, 22, 1, GOLD_D), (1, 22, 22, 22, GOLD_D), (1, 1, 1, 22, GOLD_D), (22, 1, 22, 22, GOLD_D),
								  (3, 3, 20, 3, GOLD), (3, 20, 20, 20, GOLD), (3, 3, 3, 20, GOLD), (20, 3, 20, 20, GOLD)):
		C.Rect(X0, Y0, X1, Y1, Col)
	for CX, CY in ((3, 3), (20, 3), (3, 20), (20, 20)):
		for DX, DY in ((0, -2), (-2, 0), (2, 0), (0, 2), (0, -1), (-1, 0), (1, 0), (0, 1)):
			C.Px(CX + DX, CY + DY, GOLD_L if abs(DX) + abs(DY) == 1 else GOLD)
		C.Px(CX, CY, WHITE)
	C.Outline((10, 8, 16))
	return C


def DrawSelect():
	# 선택 줄 강조 (가로로 늘림): 왼쪽에서 오른쪽으로 옅어지는 금빛 띠 + 위·아래 선
	C = FCanvas(24, 12)
	for X in range(24):
		C.Rect(X, 1, X, 10, (230, 180, 80), int(150 * (1.0 - X / 30.0)))
	C.Rect(0, 0, 23, 0, GOLD, 220)
	C.Rect(0, 11, 23, 11, GOLD_D, 200)
	return C


def DrawCursor():
	# 금빛 삼각 커서 (9x9)
	C = FCanvas(10, 10)
	Poly(C, [(1, 0.5), (8.5, 5), (1, 9.5)], GOLD)
	Poly(C, [(2, 2.5), (6, 5), (2, 5)], GOLD_L)
	C.Outline((40, 26, 8))
	return C


def DrawIcon(Name):
	# 아이템 아이콘 16x16
	C = FCanvas(16, 16)
	if Name == "Sword":
		C.Line(4, 11, 13, 2, BLADE)
		C.Line(5, 11, 13, 3, BLADE_D)
		C.Line(2, 9, 6, 13, GOLD)
		C.Line(2, 13, 4, 11, SHAFT)
		C.Px(1, 14, GOLD)
	elif Name == "Spear":
		SpearAt(C, 2, 14, 10, 6)
	elif Name == "Bow":
		for T in range(-6, 7):
			Bulge = (1.0 - (T / 6) ** 2) * 3
			C.Px(8 + T * 0.7 - Bulge * 0.7, 8 + T * 0.7 + Bulge * 0.7, BOW)
		C.Line(4, 4, 12, 12, STRING)
		C.Line(3, 13, 12, 4, SHAFT)
		C.Px(13, 3, BLADE)
	elif Name == "Staff":
		StaffAt(C, 3, 14, 10, 6, True)
	elif Name in ("Potion", "HiPotion", "Ether", "Elixir"):
		Col = {"Potion": ((230, 60, 70), (255, 150, 150)), "HiPotion": ((240, 120, 40), (255, 210, 140)),
			   "Ether": ((70, 110, 240), (160, 200, 255)), "Elixir": ((240, 200, 60), (255, 250, 190))}[Name]
		Big = Name in ("HiPotion", "Elixir")
		C.Ellipse(8, 10.5, 5.2 if Big else 4.4, 4.6 if Big else 4.0, (220, 230, 240))
		C.Ellipse(8, 11, 4.2 if Big else 3.5, 3.6 if Big else 3.0, Col[0])
		C.Ellipse(6.5, 9.5, 1.2, 1.0, Col[1])
		C.Rect(6, 3, 9, 6, (220, 230, 240))
		C.Rect(6, 2, 9, 3, (150, 100, 60))
		if Name == "Elixir":
			C.Px(8, 11, WHITE)
	elif Name == "Coin":
		C.Ellipse(8, 8, 6, 6, GOLD)
		C.Ellipse(8, 8, 3.6, 3.6, GOLD_D)
		C.Rect(7, 5, 8, 10, GOLD_L)
	C.Outline((24, 16, 28))
	return C


ITEM_ICONS = ("Sword", "Spear", "Bow", "Staff", "Potion", "HiPotion", "Ether", "Elixir", "Coin")


def WriteUiTextures(Folder):
	UpscaleSave(DrawFrame9(), os.path.join(Folder, "Frame.png"))
	UpscaleSave(DrawFrame9(True), os.path.join(Folder, "FrameLight.png"))
	UpscaleSave(DrawSelect(), os.path.join(Folder, "Select.png"))
	UpscaleSave(DrawCursor(), os.path.join(Folder, "Cursor.png"))
	for Name in ITEM_ICONS:
		UpscaleSave(DrawIcon(Name), os.path.join(Folder, "Icons", f"{Name}.png"))


# ---- 아틀라스 쓰기 -------------------------------------------------------------------------------------------------------
class FAtlas:
	# 칸을 가로로 이어 붙이는 단순 선반 패킹 (간격 1px — 점 필터라 번짐 없음, 편집기 보기용 여유)
	def __init__(self, Width):
		self.Width = Width
		self.Items = []  # (이름, 캔버스, 피벗)
		self.X = self.Y = 1
		self.RowH = 0
		self.Placed = []

	def Add(self, Name, Canvas, Pivot=(0.5, 0.0)):
		if self.X + Canvas.W + 1 > self.Width:
			self.X = 1
			self.Y += self.RowH + 1
			self.RowH = 0
		self.Placed.append((Name, Canvas, Pivot, self.X, self.Y))
		self.X += Canvas.W + 1
		self.RowH = max(self.RowH, Canvas.H)

	def Save(self, Folder, Name, Filter="Point"):
		Height = self.Y + self.RowH + 1
		Image_ = np.zeros((Height, self.Width, 4), dtype=np.uint8)
		Slices = []
		for SliceName, Canvas, Pivot, X, Y in self.Placed:
			Image_[Y:Y + Canvas.H, X:X + Canvas.W] = Canvas.P
			Slices.append({"Name": SliceName, "X": X, "Y": Y, "W": Canvas.W, "H": Canvas.H, "Pivot": list(Pivot)})
		os.makedirs(Folder, exist_ok=True)
		Image.fromarray(Image_, "RGBA").save(os.path.join(Folder, f"{Name}.png"), optimize=True)
		Doc = {"Version": 1, "Texture": f"{Name}.png", "TextureWidth": self.Width, "TextureHeight": Height,
			   "UnitsPerPixel": UNITS_PER_PIXEL, "Filter": Filter, "Slices": Slices}
		_WriteJson(os.path.join(Folder, f"{Name}.esprite"), Doc)


def _WriteJson(Path, Doc):
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def WriteFlipbook(Folder, Name, Sprite, Slices, Fps, Loop="Loop", Durations=None):
	Frames = []
	for Index, Slice in enumerate(Slices):
		Frame = {"Slice": Slice}
		if Durations:
			Frame["Duration"] = Durations[Index]
		Frames.append(Frame)
	# 프레임마다 Duration을 주면 Fps는 쓰이지 않지만 양수여야 한다
	_WriteJson(os.path.join(Folder, f"{Name}.eflipbook"), {"Version": 1, "Sprite": Sprite, "Fps": Fps if Fps > 0 else 10.0, "Loop": Loop, "Frames": Frames})


HERO_WEAPON_TIMES = {"Spear": [0.07, 0.12, 0.2], "Bow": [0.14, 0.08, 0.18], "Staff": [0.12, 0.1, 0.2]}
ENEMY_FRAMES = {
	# 종류: { 애니메이션: (그리기 인자 목록, 초당 프레임 또는 프레임별 길이, 반복) } — 이름은 HD2DEnemy.lua 규약 Idle/Move/Windup/Attack
	"Bat":      {"Idle": ([0, 1, 2, 1], 12.0, "Loop"), "Move": ([0, 1, 2, 1], 16.0, "Loop"), "Windup": ([0, 1, 2, 1], 24.0, "Loop"), "Attack": ([3], 1.0, "Loop")},
	"Goblin":   {"Idle": ([("Idle", 0), ("Idle", 1)], 3.0, "Loop"), "Move": ([("Walk", I) for I in range(4)], 10.0, "Loop"),
				 "Windup": ([("Windup", 0)], 1.0, "Loop"), "Attack": ([("Attack", 0), ("Attack", 1)], 10.0, "Once")},
	"Archer":   {"Idle": ([("Idle", 0), ("Idle", 1)], 2.5, "Loop"), "Move": ([("Walk", I) for I in range(4)], 8.0, "Loop"),
				 "Windup": ([("Draw", 0), ("Draw", 1)], [0.25, 1.0], "Once"), "Attack": ([("Shoot", 0)], 1.0, "Loop")},
	"Mushroom": {"Idle": ([("Idle", 0), ("Idle", 1)], 3.0, "Loop"), "Move": ([("Walk", 0), ("Walk", 1)], 6.0, "Loop"),
				 "Windup": ([("Windup", 0)], 1.0, "Loop"), "Attack": ([("Puff", 0)], 1.0, "Loop")},
}
GOLEM_POSES = {"Dormant": 1, "Idle": 2, "Walk": 4, "Raise": 1, "Slam": 1, "Throw": 1}


def WriteAll(Folder):
	# 용사 (기본 검 + 무기별 공격 자세)
	Hero = FAtlas(32 * 8 + 9)
	for Dir in HERO_DIRS:
		for Pose, Count in HERO_POSES.items():
			for Frame in range(Count):
				Hero.Add(f"{Pose}{Dir}{Frame}", DrawHero(Dir, Pose, Frame))
	for Weapon in HERO_WEAPON_TIMES:
		for Dir in HERO_DIRS:
			for Frame in range(3):
				Hero.Add(f"Attack{Weapon}{Dir}{Frame}", DrawHero(Dir, "Attack", Frame, Weapon))
	Hero.Save(Folder, "Hero")
	for Dir in HERO_DIRS:
		WriteFlipbook(Folder, f"Hero_Idle{Dir}", "Hero.esprite", [f"Idle{Dir}{I}" for I in range(2)], 2.5)
		WriteFlipbook(Folder, f"Hero_Walk{Dir}", "Hero.esprite", [f"Walk{Dir}{I}" for I in range(4)], 9.0)
		WriteFlipbook(Folder, f"Hero_Attack{Dir}", "Hero.esprite", [f"Attack{Dir}{I}" for I in range(3)], 0, "Once", [0.06, 0.09, 0.14])
		WriteFlipbook(Folder, f"Hero_Dash{Dir}", "Hero.esprite", [f"Dash{Dir}0"], 1.0)
		for Weapon, Times in HERO_WEAPON_TIMES.items():
			WriteFlipbook(Folder, f"Hero_Attack{Weapon}{Dir}", "Hero.esprite", [f"Attack{Weapon}{Dir}{I}" for I in range(3)], 0, "Once", Times)

	# 슬라임
	Slime = FAtlas(25 * 8 + 1)
	for Pose, Frames in SLIME_FRAMES.items():
		for Index, (RX, RY) in enumerate(Frames):
			Slime.Add(f"{Pose}{Index}", DrawSlime(RX, RY))
	Slime.Add("Blink", DrawSlime(*SLIME_FRAMES["Idle"][0], bBlink=True))
	Slime.Save(Folder, "Slime")
	WriteFlipbook(Folder, "Slime_Idle", "Slime.esprite", ["Idle0", "Idle1", "Idle2", "Idle3", "Idle0", "Blink"], 0, "Loop",
				  [0.22, 0.16, 0.22, 0.16, 0.6, 0.12])
	WriteFlipbook(Folder, "Slime_Hop", "Slime.esprite", ["Hop0", "Hop1", "Hop2", "Hop0"], 0, "Once", [0.1, 0.22, 0.14, 0.1])

	# 적 (박쥐·고블린·해골 궁수·독버섯) — 박쥐는 몸 가운데 피벗(공중), 나머지는 발
	Enemy = FAtlas(256)
	Drawn = set()
	Draw = {"Bat": lambda A: DrawBat(A), "Goblin": lambda A: DrawGoblin(*A), "Archer": lambda A: DrawArcher(*A), "Mushroom": lambda A: DrawMushroom(*A)}
	for Kind, Anims in ENEMY_FRAMES.items():
		for Anim, (Args, Rate, Loop) in Anims.items():
			Slices = []
			for A in Args:
				Name = f"{Kind}{A[0]}{A[1]}" if isinstance(A, tuple) else f"{Kind}Fly{A}"
				if Name not in Drawn:
					Drawn.add(Name)
					Enemy.Add(Name, Draw[Kind](A), (0.5, 0.5) if Kind == "Bat" else (0.5, 0.0))
				Slices.append(Name)
			if isinstance(Rate, list):
				WriteFlipbook(Folder, f"{Kind}_{Anim}", "Enemies.esprite", Slices, 0, Loop, Rate)
			else:
				WriteFlipbook(Folder, f"{Kind}_{Anim}", "Enemies.esprite", Slices, Rate, Loop)
	Enemy.Save(Folder, "Enemies")

	# 보스 골렘
	Golem = FAtlas(65 * 5 + 1)  # 칸 64x72
	for Pose, Count in GOLEM_POSES.items():
		for Frame in range(Count):
			Golem.Add(f"{Pose}{Frame}", DrawGolem(Pose, Frame))
	Golem.Save(Folder, "Golem")
	for Pose, Count in GOLEM_POSES.items():
		WriteFlipbook(Folder, f"Golem_{Pose}", "Golem.esprite", [f"{Pose}{I}" for I in range(Count)], {"Idle": 2.0, "Walk": 6.0}.get(Pose, 1.0))

	# 마을 사람
	Npc = FAtlas(33 * 4 + 1)
	for Style in NPC_STYLES:
		for Frame in range(2):
			Npc.Add(f"{Style}{Frame}", DrawVillager(Style, Frame))
	Npc.Save(Folder, "Npcs")
	for Style in NPC_STYLES:
		WriteFlipbook(Folder, f"Npc_{Style}", "Npcs.esprite", [f"{Style}0", f"{Style}1"], 0, "Loop", [0.7, 0.5])

	# 소품 (보물상자·코인·주머니)
	Props = FAtlas(128)
	for I in range(3):
		Props.Add(f"Chest{I}", DrawChest(I))
	for I in range(4):
		Props.Add(f"Coin{I}", DrawCoin(I))
	Props.Add("Bag", DrawBag())
	Props.Save(Folder, "Props")
	WriteFlipbook(Folder, "Chest_Open", "Props.esprite", ["Chest0", "Chest1", "Chest2"], 0, "Once", [0.08, 0.12, 0.2])
	WriteFlipbook(Folder, "Coin_Spin", "Props.esprite", [f"Coin{I}" for I in range(4)], 10.0)

	# 효과 (가운데 피벗)
	Fx = FAtlas(256)
	for I in range(4):
		Fx.Add(f"Slash{I}", DrawSlash(I), (0.46, 0.5))
	for I in range(3):
		Fx.Add(f"Spark{I}", DrawSpark(I), (0.5, 0.5))
	for I in range(4):
		Fx.Add(f"Dust{I}", DrawDust(I), (0.5, 0.0))
	for I in range(5):
		Fx.Add(f"Poof{I}", DrawPoof(I), (0.5, 0.3))
	Fx.Add("Shadow", DrawShadow(), (0.5, 0.5))
	Fx.Add("HeartFull", DrawHeart(True), (0.5, 0.0))
	Fx.Add("HeartEmpty", DrawHeart(False), (0.5, 0.0))
	for I in range(4):
		Fx.Add(f"Thrust{I}", DrawThrust(I), (0.0, 0.5))
	for I in range(2):
		Fx.Add(f"Bolt{I}", DrawBolt(I), (0.5, 0.5))
	for I in range(5):
		Fx.Add(f"Burst{I}", DrawBurst(I), (0.5, 0.5))
	Fx.Add("Arrow", DrawArrow(), (0.5, 0.5))
	for I in range(2):
		Fx.Add(f"Rock{I}", DrawRock(I), (0.5, 0.5))
	Fx.Add("Ring", DrawRing(), (0.5, 0.5))
	Fx.Add("Warn", DrawWarn(), (0.5, 0.5))
	for I in range(2):
		Fx.Add(f"Poison{I}", DrawPoison(I), (0.5, 0.5))
	for I in range(4):
		Fx.Add(f"Sparkle{I}", DrawSparkle(I), (0.5, 0.5))
	Fx.Add("Pillar", DrawPillar(), (0.5, 0.0))
	Fx.Add("Exclaim", DrawExclaim(), (0.5, 0.0))
	Fx.Add("HpBack", DrawBar(22, 5, (34, 18, 30), (20, 10, 18)), (0.5, 0.5))
	Fx.Add("HpFill", DrawBar(20, 3, (255, 255, 255)), (0.0, 0.5))
	for I in range(3):
		Fx.Add(f"Alert{I}", DrawAlert(I), (0.5, 0.5))
	Fx.Save(Folder, "Fx")
	WriteFlipbook(Folder, "Fx_Slash", "Fx.esprite", [f"Slash{I}" for I in range(4)], 0, "Once", [0.03, 0.04, 0.05, 0.06])
	WriteFlipbook(Folder, "Fx_Spark", "Fx.esprite", [f"Spark{I}" for I in range(3)], 18.0, "Once")
	WriteFlipbook(Folder, "Fx_Dust", "Fx.esprite", [f"Dust{I}" for I in range(4)], 14.0, "Once")
	WriteFlipbook(Folder, "Fx_Poof", "Fx.esprite", [f"Poof{I}" for I in range(5)], 14.0, "Once")
	WriteFlipbook(Folder, "Fx_Thrust", "Fx.esprite", [f"Thrust{I}" for I in range(4)], 0, "Once", [0.03, 0.05, 0.06, 0.07])
	WriteFlipbook(Folder, "Fx_Bolt", "Fx.esprite", ["Bolt0", "Bolt1"], 12.0)
	WriteFlipbook(Folder, "Fx_Burst", "Fx.esprite", [f"Burst{I}" for I in range(5)], 16.0, "Once")
	WriteFlipbook(Folder, "Fx_Rock", "Fx.esprite", ["Rock0", "Rock1"], 8.0)
	WriteFlipbook(Folder, "Fx_Poison", "Fx.esprite", ["Poison0", "Poison1"], 3.0)
	WriteFlipbook(Folder, "Fx_Sparkle", "Fx.esprite", [f"Sparkle{I}" for I in range(4)], 12.0, "Once")
	WriteFlipbook(Folder, "Fx_Alert", "Fx.esprite", [f"Alert{I}" for I in range(3)], 14.0, "Once")
	WriteFlipbook(Folder, "Fx_Ring", "Fx.esprite", ["Ring"], 1.0)


if __name__ == "__main__":
	WriteAll(os.path.join(os.path.dirname(__file__), "..", "..", "Projects", "Sample", "Content", "Sprites", "HD2D"))
