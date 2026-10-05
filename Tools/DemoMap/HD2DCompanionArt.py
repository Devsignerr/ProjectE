# HD-2D 데모 동료 "견습 마법사 엘라" 도트 아트 (CC0, 절차 생성 — 결정적). HD2DCombatGen.WriteArt가 부른다.
#   칸 32x32 (용사와 같은 크기·피벗 = 발 아래 가운데, 옆모습은 오른쪽), 아래/위/옆 3방향 × 대기 2·걷기 4·시전 2 + 쓰러짐 1, 대화·HUD 초상화.
#   모습: 남보라 로브(금 깃·밑단) + 넓은 챙의 고깔모자(분홍 리본, 끝이 꺾임) + 금발 긴 머리 + 분홍 보주 지팡이 — 용사(파란 옷·붉은 목도리)와 한눈에 구분.
#   그리는 순서·외곽선은 HD2DArt.py와 같다 (칸마다 칠한 뒤 Outline).
import math
import os

from HD2DArt import (FCanvas, FAtlas, Poly, UpscaleSave, WriteFlipbook, DrawPortrait, SKIN, SKIN_D, BLUSH, EYE, SHAFT, SHAFT_D, BOOT, BOOT_D, GOLD,
					 GOLD_L, GOLD_D, WHITE)

CELL = 32
ROBE, ROBE_L, ROBE_D = (96, 72, 168), (138, 112, 212), (60, 44, 116)
HAT, HAT_L, HAT_D = (80, 58, 146), (120, 94, 192), (46, 32, 92)
RIBBON, RIBBON_D = (236, 104, 140), (176, 60, 100)
HAIR, HAIR_L, HAIR_D = (238, 202, 112), (255, 234, 164), (190, 142, 64)
ORB, ORB_L, ORB_D = (255, 138, 188), (255, 226, 240), (198, 78, 142)


def _Orb(C, X, Y, R=2.2, bGlow=False):
	if bGlow:
		C.Ellipse(X + 0.5, Y + 0.5, R + 1.6, R + 1.6, (255, 176, 214), 200)
	C.Ellipse(X + 0.5, Y + 0.5, R, R, ORB)
	C.Ellipse(X, Y, R * 0.55, R * 0.55, ORB_L)
	C.Px(X + 1, Y + 1, ORB_D)


def _Staff(C, X0, Y0, X1, Y1, bGlow=False):
	# 자루 (X0,Y0 = 보주 쪽 → X1,Y1 = 아래 끝) + 보주 받침(금 갈고리) + 보주
	C.Line(X0, Y0, X1, Y1, SHAFT)
	DX, DY = X1 - X0, Y1 - Y0
	L = max(math.hypot(DX, DY), 1e-6)
	NX, NY = -DY / L, DX / L
	C.Line(X0 + NX, Y0 + NY, X1 + NX, Y1 + NY, SHAFT_D)
	C.Px(X0 - NX, Y0 - NY, GOLD)
	C.Px(X0 + NX * 2, Y0 + NY * 2, GOLD)
	_Orb(C, X0 - DX / L * 2.2, Y0 - DY / L * 2.2, 2.2, bGlow)


def _HatFront(C, Top, bBack):
	# 챙 (넓고 납작) + 고깔 (끝이 오른쪽으로 꺾임) + 리본 띠
	Poly(C, [(10, Top + 2.5), (22, Top + 2.5), (19.5, Top - 3), (22.5, Top - 6.2), (16.5, Top - 3.5)], HAT)
	Poly(C, [(10, Top + 2.5), (14, Top + 2.5), (16.5, Top - 3.5), (15, Top - 2)], HAT_L if not bBack else HAT_D)
	C.Rect(11, Top + 1, 21, Top + 1, RIBBON if not bBack else RIBBON_D)
	if not bBack:
		C.Px(20, Top + 1, RIBBON_D)
		C.Px(21, Top + 2, RIBBON)
	C.Rect(6, Top + 2, 25, Top + 3, HAT)
	C.Rect(7, Top + 4, 24, Top + 4, HAT_D)
	C.Rect(8, Top + 2, 14, Top + 2, HAT_L)


def _HeadFront(C, Top, bBack, bClosed=False):
	if bBack:
		# 뒷머리: 등까지 내려오며 끝이 갈라짐 (가운데·양쪽 머릿결 + 밝은 결)
		C.Rect(10, Top + 4, 21, Top + 13, HAIR)
		C.Rect(11, Top + 14, 20, Top + 14, HAIR)
		for X in (11, 14, 17, 20):
			C.Px(X, Top + 15, HAIR_D)
		C.Rect(10, Top + 10, 10, Top + 13, HAIR_D)
		C.Rect(21, Top + 6, 21, Top + 13, HAIR_D)
		C.Line(15, Top + 6, 15, Top + 13, HAIR_D)
		C.Line(18, Top + 7, 18, Top + 12, HAIR_D)
		C.Line(12, Top + 5, 12, Top + 11, HAIR_L)
		C.Px(13, Top + 5, HAIR_L)
		_HatFront(C, Top, True)
		return
	# 긴 옆머리 (어깨까지) → 얼굴 → 앞머리 → 모자
	C.Rect(10, Top + 4, 10, Top + 13, HAIR)
	C.Rect(21, Top + 4, 21, Top + 13, HAIR_D)
	C.Px(10, Top + 14, HAIR_D)
	C.Px(21, Top + 14, HAIR_D)
	C.Px(9, Top + 11, HAIR)
	C.Px(22, Top + 11, HAIR_D)
	C.Rect(11, Top + 4, 20, Top + 11, SKIN)
	C.Rect(11, Top + 11, 20, Top + 11, SKIN_D)
	C.Rect(11, Top + 4, 20, Top + 5, HAIR)
	for X in (11, 13, 14, 17, 19, 20):
		C.Px(X, Top + 6, HAIR)
	C.Rect(12, Top + 4, 15, Top + 4, HAIR_L)
	if bClosed:
		C.Rect(12, Top + 8, 14, Top + 8, EYE)
		C.Rect(17, Top + 8, 19, Top + 8, EYE)
	else:
		C.Rect(13, Top + 7, 13, Top + 8, EYE)
		C.Rect(18, Top + 7, 18, Top + 8, EYE)
		C.Px(13, Top + 7, (120, 90, 200))  # 보랏빛 눈동자 반짝
		C.Px(18, Top + 7, (120, 90, 200))
	C.Px(12, Top + 9, BLUSH)
	C.Px(19, Top + 9, BLUSH)
	C.Px(15, Top + 10, SKIN_D)
	_HatFront(C, Top, False)


def _RobeFront(C, B, bBack, Sway=0, ArmL=0, ArmR=0):
	Y0 = 17 + B
	Poly(C, [(11, Y0), (20.5, Y0), (22.5 + Sway, 29.5), (9 + Sway, 29.5)], ROBE)
	Poly(C, [(11, Y0), (13, Y0), (11.5 + Sway, 29.5), (9 + Sway, 29.5)], ROBE_L if not bBack else ROBE)
	Poly(C, [(19, Y0), (20.5, Y0), (22.5 + Sway, 29.5), (20.5 + Sway, 29.5)], ROBE_D)
	C.Rect(9 + Sway, 28, 22 + Sway, 29, ROBE_D)
	C.Rect(10 + Sway, 28, 21 + Sway, 28, GOLD_D)
	C.Rect(12, 22 + B, 19, 22 + B, ROBE_D)  # 허리끈
	if not bBack:
		C.Rect(12, Y0, 19, Y0, GOLD)  # 깃
		C.Px(15, Y0 + 1, GOLD_D)
		C.Px(16, Y0 + 1, GOLD_D)
		C.Px(15, 22 + B, ORB)  # 허리끈 보석
		C.Px(16, 22 + B, ORB_D)
		C.Line(15, Y0 + 2, 15 + Sway * 0.5, 27, ROBE_D)  # 앞섶
	# 넓은 소매 (끝이 나팔처럼)
	C.Rect(9, Y0 + ArmL, 10, Y0 + 4 + ArmL, ROBE)
	C.Rect(8, Y0 + 4 + ArmL, 10, Y0 + 5 + ArmL, ROBE_D)
	C.Rect(21, Y0 + ArmR, 22, Y0 + 4 + ArmR, ROBE_D)
	C.Rect(21, Y0 + 4 + ArmR, 23, Y0 + 5 + ArmR, ROBE_D)
	C.Rect(9, Y0 + 6 + ArmL, 10, Y0 + 6 + ArmL, SKIN)
	C.Rect(21, Y0 + 6 + ArmR, 22, Y0 + 6 + ArmR, SKIN)
	if not bBack:
		C.Px(23, Y0 + 5 + ArmR, SKIN)  # 지팡이를 쥔 손
	else:
		C.Px(8, Y0 + 5 + ArmL, SKIN)


def _FeetFront(C, Phase):
	LiftL, LiftR = (1 if Phase == 1 else 0), (1 if Phase == 2 else 0)
	C.Rect(11, 30 - LiftL, 13, 30 - LiftL, BOOT_D)
	C.Rect(18, 30 - LiftR, 20, 30 - LiftR, BOOT_D)


def _HeadSide(C, Top, Lean=0):
	# 옆모습 (오른쪽): 뒤로 늘어진 긴 머리 → 얼굴 → 모자 (챙이 앞으로, 고깔 끝은 뒤로 꺾임)
	C.Rect(10, Top + 4, 14, Top + 14, HAIR)
	C.Rect(10, Top + 12, 13, Top + 15, HAIR_D)
	C.Rect(11, Top + 5, 12, Top + 10, HAIR_L)
	C.Rect(14, Top + 4, 20, Top + 11, SKIN)
	C.Rect(21, Top + 6, 21, Top + 9, SKIN)
	C.Rect(14, Top + 11, 20, Top + 11, SKIN_D)
	C.Rect(14, Top + 4, 20, Top + 5, HAIR)
	for X in (16, 18, 20):
		C.Px(X, Top + 6, HAIR)
	C.Rect(14, Top + 6, 15, Top + 9, HAIR)
	C.Rect(19, Top + 7, 19, Top + 8, EYE)
	C.Px(19, Top + 7, (120, 90, 200))
	C.Px(20, Top + 9, BLUSH)
	Poly(C, [(11, Top + 2.5), (21, Top + 2.5), (16, Top - 3), (10.5, Top - 6.2), (12.5, Top - 2.5)], HAT)
	Poly(C, [(16, Top + 2.5), (21, Top + 2.5), (16, Top - 3)], HAT_L)
	C.Rect(12, Top + 1, 20, Top + 1, RIBBON)
	C.Rect(9, Top + 1, 11, Top + 2, RIBBON)  # 뒤로 날리는 리본 끝
	C.Px(8, Top + 3, RIBBON_D)
	C.Rect(7, Top + 2, 27, Top + 3, HAT)
	C.Rect(8, Top + 4, 26, Top + 4, HAT_D)
	C.Rect(19, Top + 2, 26, Top + 2, HAT_L)


def _RobeSide(C, B, Sway=0):
	Y0 = 17 + B
	Poly(C, [(13, Y0), (19, Y0), (21.5 + Sway, 29.5), (10 + Sway, 29.5)], ROBE)
	Poly(C, [(13, Y0), (14.5, Y0), (12 + Sway, 29.5), (10 + Sway, 29.5)], ROBE_D)
	C.Rect(10 + Sway, 28, 21 + Sway, 29, ROBE_D)
	C.Rect(11 + Sway, 28, 21 + Sway, 28, GOLD_D)
	C.Rect(13, 22 + B, 19, 22 + B, ROBE_D)
	C.Rect(14, Y0, 19, Y0, GOLD)


def _FeetSide(C, Phase):
	if Phase == 0:
		C.Rect(13, 30, 15, 30, BOOT_D)
		C.Rect(17, 30, 19, 30, BOOT)
	elif Phase == 1:
		C.Rect(11, 30, 13, 30, BOOT_D)
		C.Rect(19, 30, 21, 30, BOOT)
	else:
		C.Rect(12, 30, 14, 30, BOOT)
		C.Rect(18, 30, 20, 30, BOOT_D)


def _ArmSide(C, B, DX=0, DY=0):
	Y0 = 18 + B
	C.Rect(15 + DX, Y0 + DY, 17 + DX, Y0 + 4 + DY, ROBE_L)
	C.Rect(15 + DX, Y0 + 4 + DY, 18 + DX, Y0 + 5 + DY, ROBE)
	C.Rect(17 + DX, Y0 + 6 + DY, 18 + DX, Y0 + 6 + DY, SKIN)


def DrawMage(Dir, Pose, Frame):
	# Dir: Down | Up | Side, Pose: Idle | Walk | Cast | Down(쓰러짐)
	C = FCanvas(CELL, CELL)
	if Pose == "Down":
		return DrawMageDown()
	B, Feet, Sway = 0, 0, 0
	if Pose == "Idle":
		B = Frame % 2
	elif Pose == "Walk":
		Feet = (1, 0, 2, 0)[Frame % 4]
		B = (0, 1, 0, 1)[Frame % 4]
		Sway = (1, 0, -1, 0)[Frame % 4]
	elif Pose == "Cast":
		B = (0, 1)[Frame]
	Top = 6 + B
	if Dir in ("Down", "Up"):
		bBack = Dir == "Up"
		StaffX = 7 if bBack else 24
		ArmStaff = -2 if Pose == "Cast" and Frame == 0 else 0
		# 지팡이 (손보다 먼저 — 손이 자루를 쥔다)
		if Pose == "Cast" and Frame == 0:
			_Staff(C, StaffX + (1 if not bBack else -1), 4, StaffX, 27, True)
		elif Pose == "Cast":
			_Staff(C, StaffX - (2 if not bBack else -2), 9, StaffX + (1 if not bBack else -1), 29, True)
		else:
			_Staff(C, StaffX, 15 + B, StaffX, 30, False)
		_FeetFront(C, Feet)
		if bBack:
			_RobeFront(C, B, True, Sway, ArmStaff, 0)
		else:
			_RobeFront(C, B, False, Sway, 0, ArmStaff)
		_HeadFront(C, Top, bBack)
	else:
		_FeetSide(C, Feet)
		if Pose == "Cast" and Frame == 0:
			_Staff(C, 13, 2, 18, 26, True)  # 머리 위로 치켜든 지팡이 (몸 뒤)
		_RobeSide(C, B, Sway)
		_HeadSide(C, Top)
		if Pose == "Cast" and Frame == 1:
			_Staff(C, 28, 14 + B, 15, 25 + B, True)  # 앞으로 내민 지팡이
			_ArmSide(C, B, 2, -1)
		elif Pose == "Cast":
			_ArmSide(C, B, -1, -3)
		else:
			_Staff(C, 23, 15 + B, 23, 30, False)
			_ArmSide(C, B, 4, 0)  # 지팡이를 쥔 팔
	C.Outline()
	return C


def DrawMageDown():
	# 쓰러짐: 무릎 꿇고 주저앉음 (로브가 바닥에 퍼짐) + 고개 숙임(눈 감음) + 벗겨진 모자·떨어진 지팡이
	C = FCanvas(CELL, CELL)
	_Staff(C, 3, 29, 15, 30, False)
	Poly(C, [(9, 22), (21, 22), (25, 30.5), (6, 30.5)], ROBE)
	Poly(C, [(9, 22), (12, 22), (9, 30.5), (6, 30.5)], ROBE_L)
	C.Rect(6, 29, 25, 30, ROBE_D)
	C.Rect(7, 29, 24, 29, GOLD_D)
	C.Rect(8, 25, 10, 27, ROBE)
	C.Rect(21, 25, 23, 27, ROBE_D)
	C.Rect(9, 28, 10, 28, SKIN)
	C.Rect(21, 28, 22, 28, SKIN)
	Top = 12
	C.Rect(11, Top + 3, 20, Top + 4, HAIR)
	C.Rect(10, Top + 5, 21, Top + 11, HAIR)
	C.Rect(10, Top + 12, 11, Top + 13, HAIR_D)
	C.Rect(20, Top + 12, 21, Top + 13, HAIR_D)
	C.Rect(12, Top + 7, 19, Top + 11, SKIN)
	C.Rect(12, Top + 7, 19, Top + 8, HAIR)
	for X in (13, 16, 18):
		C.Px(X, Top + 9, HAIR)
	C.Rect(12, Top + 3, 16, Top + 3, HAIR_L)
	C.Rect(13, Top + 10, 14, Top + 10, EYE)
	C.Rect(17, Top + 10, 18, Top + 10, EYE)
	C.Px(15, Top + 11, SKIN_D)
	# 벗겨져 옆에 떨어진 모자
	Poly(C, [(23, 30.5), (31, 30.5), (29, 25), (31, 22.5), (26.5, 25)], HAT)
	C.Rect(22, 29, 31, 30, HAT_D)
	C.Rect(24, 28, 30, 28, RIBBON)
	C.Outline()
	return C


POSES = {"Idle": 2, "Walk": 4, "Cast": 2}
DIRS = ("Down", "Up", "Side")


def WriteArt(SpriteFolder, UiFolder):
	Atlas = FAtlas(33 * 8 + 1)
	for Dir in DIRS:
		for Pose, Count in POSES.items():
			for Frame in range(Count):
				Atlas.Add(f"{Pose}{Dir}{Frame}", DrawMage(Dir, Pose, Frame))
	Atlas.Add("Down0", DrawMageDown())
	Atlas.Save(SpriteFolder, "Mage")
	for Dir in DIRS:
		WriteFlipbook(SpriteFolder, f"Mage_Idle{Dir}", "Mage.esprite", [f"Idle{Dir}{I}" for I in range(2)], 2.4)
		WriteFlipbook(SpriteFolder, f"Mage_Walk{Dir}", "Mage.esprite", [f"Walk{Dir}{I}" for I in range(4)], 8.5)
		WriteFlipbook(SpriteFolder, f"Mage_Cast{Dir}", "Mage.esprite", [f"Cast{Dir}0", f"Cast{Dir}1"], 0, "Once", [0.28, 0.4])
	WriteFlipbook(SpriteFolder, "Mage_Down", "Mage.esprite", ["Down0"], 1.0)
	UpscaleSave(DrawPortrait(DrawMage("Down", "Idle", 0), (5, 0, 27, 20)), os.path.join(UiFolder, "Portraits", "Mage.png"), 6)
