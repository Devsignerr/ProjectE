# HD-2D 데모 맵용 자체 도트 아트 (CC0, 절차 생성 — 결정적). BuildHD2D.py가 부른다.
#   용사(32x32 칸, 아래/위/옆 3방향 × 대기·걷기·공격·대시), 슬라임(24x20 칸), 효과(베기 호·불꽃 튐·먼지·펑·그림자·하트)
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


def DrawHero(Dir, Pose, Frame):
	# Dir: "Down" | "Up" | "Side", Pose: "Idle" | "Walk" | "Attack" | "Dash"
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
			if Frame == 0:
				C.Blade(21, 14, 25, 5)
			elif Frame == 1:
				C.Blade(19, 10, 7, 3)
			else:
				C.Blade(11, 12, 4, 7)
		_HeroLegsFront(C, Legs)
		_HeroBodyFront(C, B, bBack, ArmL, ArmR)
		_HeroHeadFront(C, B, bBack)
		if Pose == "Attack" and not bBack:
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
		if Pose == "Attack" and Frame == 0:
			C.Blade(14, 14 + B, 9, 4 + B)  # 머리 뒤로 치켜든 칼 (몸 뒤)
		_HeroLegsSide(C, Legs)
		_HeroBodySide(C, B, ArmDX, ArmDY, True, ScarfLen)
		_HeroHeadSide(C, B)
		if Pose == "Attack":
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


def WriteAll(Folder):
	# 용사
	Hero = FAtlas(32 * 8 + 9)
	for Dir in HERO_DIRS:
		for Pose, Count in HERO_POSES.items():
			for Frame in range(Count):
				Hero.Add(f"{Pose}{Dir}{Frame}", DrawHero(Dir, Pose, Frame))
	Hero.Save(Folder, "Hero")
	for Dir in HERO_DIRS:
		WriteFlipbook(Folder, f"Hero_Idle{Dir}", "Hero.esprite", [f"Idle{Dir}{I}" for I in range(2)], 2.5)
		WriteFlipbook(Folder, f"Hero_Walk{Dir}", "Hero.esprite", [f"Walk{Dir}{I}" for I in range(4)], 9.0)
		WriteFlipbook(Folder, f"Hero_Attack{Dir}", "Hero.esprite", [f"Attack{Dir}{I}" for I in range(3)], 0, "Once", [0.06, 0.09, 0.14])
		WriteFlipbook(Folder, f"Hero_Dash{Dir}", "Hero.esprite", [f"Dash{Dir}0"], 1.0)

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
	Fx.Save(Folder, "Fx")
	WriteFlipbook(Folder, "Fx_Slash", "Fx.esprite", [f"Slash{I}" for I in range(4)], 0, "Once", [0.03, 0.04, 0.05, 0.06])
	WriteFlipbook(Folder, "Fx_Spark", "Fx.esprite", [f"Spark{I}" for I in range(3)], 18.0, "Once")
	WriteFlipbook(Folder, "Fx_Dust", "Fx.esprite", [f"Dust{I}" for I in range(4)], 14.0, "Once")
	WriteFlipbook(Folder, "Fx_Poof", "Fx.esprite", [f"Poof{I}" for I in range(5)], 14.0, "Once")


if __name__ == "__main__":
	WriteAll(os.path.join(os.path.dirname(__file__), "..", "..", "Projects", "Sample", "Content", "Sprites", "HD2D"))
