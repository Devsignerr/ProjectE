# HD-2D 데모 메타 화면용 도트 아이콘·도감 그림 (HD2DMetaGen.WriteAll이 부른다 — 절차 생성, 결정적).
#   메뉴 아이콘(16x16: 일시정지 메뉴 줄·설정 줄·기록 줄·퀘스트 일지 표시), 지도 표시(12x12: 플레이어·마을 사람·목표·상자·기록·출구·보스),
#   재료 아이콘(16x16: 박쥐 날개·고블린 송곳니·낡은 뼛조각·독버섯 포자·수정 조각·골렘의 핵), 도감 그림(적 도트를 정수배 확대).
#   UI 샘플러가 선형이라 모두 최근접 확대 PNG (HD2DArt.UpscaleSave). 색은 HD2DArt 팔레트와 맞춘다.
import os

import numpy as np

import HD2DArt as A

GOLD, GOLD_L, GOLD_D = A.GOLD, A.GOLD_L, A.GOLD_D
INK = (24, 16, 28)
PAPER, PAPER_D = (236, 220, 178), (186, 160, 112)
LEATHER, LEATHER_D = (150, 92, 54), (104, 60, 36)
STEEL, STEEL_D = (206, 214, 228), (130, 140, 162)
RED, RED_L = (220, 70, 70), (255, 160, 150)
GREEN, GREEN_L = (96, 196, 104), (176, 240, 160)
BLUE, BLUE_L = (80, 140, 230), (170, 210, 255)


def _C(W=16, H=16):
	return A.FCanvas(W, H)


def _Done(C, Col=INK):
	C.Outline(Col)
	return C


# ---------------------------------------------------------------- 메뉴 아이콘 (16x16)
def IconResume():
	# 모닥불 (쉬던 자리에서 다시 길을 나선다)
	C = _C()
	A.Poly(C, [(8, 1), (12, 8), (11, 12), (5, 12), (4, 8)], (240, 120, 40))
	A.Poly(C, [(8, 5), (10.5, 9), (9.5, 12), (6.5, 12), (5.5, 9)], (255, 220, 110))
	C.Line(2, 14, 13, 12, LEATHER)
	C.Line(2, 12, 13, 14, LEATHER_D)
	return _Done(C)


def IconItems():
	# 가죽 주머니 + 물약 머리
	C = _C()
	C.Ellipse(8, 10.5, 6, 4.8, LEATHER)
	C.Ellipse(6.5, 9, 2.4, 1.6, (196, 132, 82))
	C.Rect(5, 4, 10, 6, LEATHER_D)
	C.Rect(6, 3, 9, 3, GOLD)
	C.Rect(4, 13, 11, 14, LEATHER_D)
	return _Done(C)


def IconEquip():
	# 방패 + 비스듬한 검
	C = _C()
	A.Poly(C, [(2, 3), (10, 3), (10, 9), (6, 14), (2, 9)], (70, 110, 190))
	A.Poly(C, [(3, 4), (6, 4), (6, 12), (3, 9)], (120, 160, 230))
	C.Rect(5, 6, 7, 7, GOLD)
	C.Line(7, 13, 14, 2, A.BLADE)
	C.Line(8, 13, 14, 3, A.BLADE_D)
	C.Line(7, 10, 10, 13, GOLD)
	return _Done(C)


def IconJournal():
	# 펼친 책
	C = _C()
	A.Poly(C, [(1, 4), (8, 3), (8, 14), (1, 13)], PAPER)
	A.Poly(C, [(8, 3), (15, 4), (15, 13), (8, 14)], (246, 234, 200))
	C.Line(8, 3, 8, 14, PAPER_D)
	for Y in (6, 8, 10):
		C.Line(3, Y, 6, Y, PAPER_D)
		C.Line(10, Y, 13, Y, PAPER_D)
	C.Rect(11, 1, 12, 5, RED)
	return _Done(C)


def IconMap():
	# 접힌 지도 (세 칸) + 붉은 X
	C = _C()
	A.Poly(C, [(1, 3), (5, 2), (5, 13), (1, 14)], PAPER_D)
	A.Poly(C, [(5, 2), (10, 3), (10, 14), (5, 13)], PAPER)
	A.Poly(C, [(10, 3), (15, 2), (15, 13), (10, 14)], PAPER_D)
	C.Line(2, 9, 6, 6, (110, 160, 90))
	C.Line(6, 6, 9, 10, (110, 160, 90))
	C.Line(11, 5, 13, 7, RED)
	C.Line(13, 5, 11, 7, RED)
	return _Done(C)


def IconBestiary():
	# 표지에 슬라임 얼굴이 있는 두꺼운 책
	C = _C()
	C.Rect(3, 1, 13, 14, (120, 60, 90))
	C.Rect(2, 2, 3, 14, (84, 40, 64))
	C.Rect(4, 14, 13, 14, PAPER)
	C.Ellipse(8.5, 8.5, 3.6, 2.8, A.SLIME)
	C.Px(7, 8, INK)
	C.Px(10, 8, INK)
	C.Px(7, 7, A.SLIME_L)
	C.Rect(5, 3, 12, 3, GOLD)
	return _Done(C)


def IconRecords():
	# 메달 (리본 + 금빛 원)
	C = _C()
	A.Poly(C, [(4, 1), (7, 1), (9, 7), (6, 7)], RED)
	A.Poly(C, [(9, 1), (12, 1), (10, 7), (7, 7)], (60, 100, 200))
	C.Ellipse(8, 10.5, 4.6, 4.6, GOLD)
	C.Ellipse(8, 10.5, 2.6, 2.6, GOLD_D)
	C.Px(7, 9, GOLD_L)
	return _Done(C)


def IconSettings():
	# 톱니바퀴
	C = _C()
	for Y in range(16):
		for X in range(16):
			DX, DY = X + 0.5 - 8, Y + 0.5 - 8
			R = (DX * DX + DY * DY) ** 0.5
			Angle = np.arctan2(DY, DX)
			Tooth = (np.cos(Angle * 8) > 0.35)
			if R <= 4.6 or (R <= 6.6 and Tooth):
				C.Px(X, Y, STEEL if DY < 0.5 else STEEL_D)
	C.Ellipse(8, 8, 2.0, 2.0, (0, 0, 0), 0)
	for Y in range(16):
		for X in range(16):
			if (X + 0.5 - 8) ** 2 + (Y + 0.5 - 8) ** 2 <= 3.2:
				C.P[Y, X] = (0, 0, 0, 0)
	return _Done(C)


def IconSave():
	# 깃펜 + 잉크병
	C = _C()
	A.Poly(C, [(14, 1), (9, 9), (7, 9), (12, 1)], (246, 244, 236))
	C.Line(13, 2, 8, 10, (190, 190, 200))
	C.Line(8, 10, 6, 12, INK)
	C.Rect(2, 11, 7, 14, (60, 60, 110))
	C.Rect(3, 10, 6, 10, (90, 90, 150))
	return _Done(C)


def IconTitle():
	# 아치 문
	C = _C()
	C.Rect(3, 5, 12, 15, (130, 120, 112))
	C.Ellipse(7.5, 5.5, 4.6, 4.2, (130, 120, 112))
	C.Rect(5, 7, 10, 15, LEATHER)
	C.Ellipse(7.5, 7, 2.6, 2.2, LEATHER)
	C.Line(7, 6, 7, 15, LEATHER_D)
	C.Px(9, 11, GOLD)
	return _Done(C)


def IconPin():
	C = _C()
	C.Ellipse(8, 6, 4.4, 4.4, RED)
	A.Poly(C, [(4.5, 7.5), (11.5, 7.5), (8, 15)], RED)
	C.Ellipse(8, 6, 1.8, 1.8, (255, 230, 220))
	return _Done(C)


def IconTime():
	# 모래시계
	C = _C()
	C.Rect(3, 1, 12, 2, LEATHER)
	C.Rect(3, 13, 12, 14, LEATHER)
	A.Poly(C, [(4, 3), (11, 3), (8.5, 8), (6.5, 8)], (220, 236, 246))
	A.Poly(C, [(6.5, 8), (8.5, 8), (11, 13), (4, 13)], (220, 236, 246))
	A.Poly(C, [(5.5, 4), (9.5, 4), (8, 7)], (236, 200, 120))
	A.Poly(C, [(8, 10), (10.5, 12.5), (4.5, 12.5)], (236, 200, 120))
	return _Done(C)


def IconSound():
	C = _C()
	C.Rect(2, 6, 4, 10, STEEL)
	A.Poly(C, [(4, 6), (9, 2), (9, 14), (4, 10)], STEEL)
	for R, Col in ((3.5, GOLD), (6.0, GOLD_D)):
		for T in np.linspace(-0.9, 0.9, 14):
			C.Px(9 + R * np.cos(T), 8 + R * np.sin(T), Col)
	return _Done(C)


def IconShake():
	# 흔들리는 화면 (사각 + 양옆 물결)
	C = _C()
	C.Rect(4, 3, 11, 12, (70, 90, 140))
	C.Rect(5, 4, 10, 11, (120, 160, 220))
	C.Rect(6, 8, 9, 11, (110, 170, 100))
	for X in (1, 14):
		for Y in (4, 7, 10):
			C.Px(X, Y, GOLD)
			C.Px(X + (1 if X == 1 else -1), Y + 1, GOLD)
	return _Done(C)


def IconNumbers():
	# 떠오르는 숫자 (붉은 "7" + 초록 "+")
	C = _C()
	C.Rect(2, 2, 8, 3, RED)
	C.Line(8, 3, 4, 12, RED)
	C.Line(7, 3, 3, 12, RED_L)
	C.Rect(11, 5, 12, 12, GREEN)
	C.Rect(8, 8, 15, 9, GREEN)
	return _Done(C)


def IconTextSpeed():
	# 말풍선 + 줄
	C = _C()
	C.Rect(1, 2, 14, 10, PAPER)
	A.Poly(C, [(4, 10), (8, 10), (3, 14)], PAPER)
	for Y in (4, 6, 8):
		C.Line(3, Y, 12 - (Y == 8) * 4, Y, PAPER_D)
	return _Done(C)


def IconMinimap():
	# 테두리 있는 작은 지도 + 가운데 점
	C = _C()
	C.Rect(1, 2, 14, 13, GOLD_D)
	C.Rect(2, 3, 13, 12, (126, 156, 88))
	C.Rect(2, 9, 13, 12, (92, 146, 170))
	C.Line(2, 6, 13, 8, (196, 160, 104))
	C.Rect(7, 6, 8, 7, RED)
	return _Done(C)


def IconReset():
	# 둥근 화살표
	C = _C()
	for T in np.linspace(0.6, 5.6, 40):
		C.Px(8 + 5 * np.cos(T), 8 + 5 * np.sin(T), BLUE)
		C.Px(8 + 4 * np.cos(T), 8 + 4 * np.sin(T), BLUE_L)
	A.Poly(C, [(13, 3), (14.5, 8.5), (9, 7)], BLUE)
	return _Done(C)


def IconStar():
	C = _C()
	Pts = []
	for K in range(10):
		R = 7.0 if K % 2 == 0 else 3.0
		T = -np.pi / 2 + K * np.pi / 5
		Pts.append((8 + R * np.cos(T), 8.5 + R * np.sin(T)))
	A.Poly(C, Pts, GOLD)
	C.Px(7, 6, GOLD_L)
	C.Px(8, 6, GOLD_L)
	return _Done(C)


def IconSkull():
	C = _C()
	C.Ellipse(8, 7, 6, 5.4, (236, 230, 214))
	C.Rect(5, 11, 11, 14, (236, 230, 214))
	C.Ellipse(5.7, 7.5, 1.6, 1.8, INK)
	C.Ellipse(10.3, 7.5, 1.6, 1.8, INK)
	C.Px(8, 10, INK)
	for X in (6, 8, 10):
		C.Px(X, 13, (150, 140, 130))
	return _Done(C)


def IconChest():
	C = _C()
	C.Rect(2, 7, 13, 14, LEATHER)
	C.Ellipse(7.5, 7, 5.8, 3.4, (176, 108, 62))
	C.Rect(2, 7, 13, 8, GOLD_D)
	C.Rect(7, 8, 8, 11, GOLD)
	C.Rect(2, 13, 13, 14, LEATHER_D)
	return _Done(C)


def IconHammer():
	C = _C()
	C.Line(4, 14, 11, 7, LEATHER)
	C.Line(5, 14, 12, 7, LEATHER_D)
	A.Poly(C, [(7, 4), (12, 1), (15, 5), (10, 8)], STEEL)
	A.Poly(C, [(7, 4), (9, 3), (12, 7), (10, 8)], STEEL_D)
	return _Done(C)


def IconBoot():
	C = _C()
	C.Rect(5, 2, 10, 10, LEATHER)
	C.Rect(5, 10, 14, 13, LEATHER)
	C.Rect(5, 13, 14, 14, LEATHER_D)
	C.Rect(5, 2, 10, 3, GOLD_D)
	return _Done(C)


def IconHeart():
	C = _C()
	C.Ellipse(5.2, 6, 3.6, 3.4, RED)
	C.Ellipse(10.8, 6, 3.6, 3.4, RED)
	A.Poly(C, [(1.8, 7), (14.2, 7), (8, 14)], RED)
	C.Px(4, 5, RED_L)
	C.Px(5, 4, RED_L)
	return _Done(C)


def IconCheck():
	C = _C()
	C.Ellipse(8, 8, 6.6, 6.6, (70, 150, 80))
	C.Line(4, 8, 7, 11, (236, 255, 230))
	C.Line(7, 11, 12, 5, (236, 255, 230))
	C.Line(4, 9, 7, 12, (200, 240, 200))
	C.Line(7, 12, 12, 6, (200, 240, 200))
	return _Done(C)


def IconScroll():
	# 서브 퀘스트 (두루마리)
	C = _C()
	C.Rect(4, 3, 12, 13, PAPER)
	C.Rect(3, 2, 13, 3, PAPER_D)
	C.Rect(3, 13, 13, 14, PAPER_D)
	for Y in (5, 7, 9, 11):
		C.Line(6, Y, 10, Y, PAPER_D)
	C.Px(12, 9, RED)
	return _Done(C)


def IconCrown():
	# 메인 퀘스트 (왕관)
	C = _C()
	A.Poly(C, [(2, 5), (5, 9), (8, 3), (11, 9), (14, 5), (13, 12), (3, 12)], GOLD)
	C.Rect(3, 11, 13, 13, GOLD_D)
	C.Px(8, 6, RED)
	C.Px(5, 10, BLUE_L)
	C.Px(11, 10, BLUE_L)
	return _Done(C)


def IconLock():
	# 도감 미확인 (물음표 자물쇠)
	C = _C()
	for T in np.linspace(np.pi, 2 * np.pi, 18):
		C.Px(8 + 3.5 * np.cos(T), 7 + 3.5 * np.sin(T), STEEL_D)
	C.Rect(4, 7, 4, 8, STEEL_D)
	C.Rect(11, 7, 11, 8, STEEL_D)
	C.Rect(3, 8, 12, 14, (110, 104, 124))
	C.Px(8, 10, (40, 36, 50))
	C.Px(8, 11, (40, 36, 50))
	return _Done(C)


MENU_ICONS = {
	"Resume": IconResume, "Items": IconItems, "Equip": IconEquip, "Journal": IconJournal, "Map": IconMap, "Bestiary": IconBestiary,
	"Records": IconRecords, "Settings": IconSettings, "Save": IconSave, "Title": IconTitle, "Pin": IconPin, "Time": IconTime,
	"Sound": IconSound, "Shake": IconShake, "Numbers": IconNumbers, "TextSpeed": IconTextSpeed, "Minimap": IconMinimap, "Reset": IconReset,
	"Star": IconStar, "Skull": IconSkull, "Chest": IconChest, "Hammer": IconHammer, "Boot": IconBoot, "Heart": IconHeart, "Check": IconCheck,
	"Scroll": IconScroll, "Crown": IconCrown, "Lock": IconLock,
}


# ---------------------------------------------------------------- 지도 표시 (12x12)
def _Marker(Fill, FillL, Shape="Circle"):
	C = _C(12, 12)
	if Shape == "Circle":
		C.Ellipse(6, 6, 4.6, 4.6, Fill)
		C.Ellipse(5, 5, 1.8, 1.8, FillL)
	elif Shape == "Diamond":
		A.Poly(C, [(6, 0.5), (11.5, 6), (6, 11.5), (0.5, 6)], Fill)
		A.Poly(C, [(6, 2.5), (8.5, 5), (6, 6), (3.5, 5)], FillL)
	return _Done(C, (20, 14, 22))


def MarkerPlayer():
	# 아래를 가리키는 금빛 화살촉 + 흰 테
	C = _C(12, 12)
	A.Poly(C, [(6, 11.5), (0.8, 2), (6, 4.5), (11.2, 2)], (255, 236, 150))
	A.Poly(C, [(6, 9.5), (2.8, 3.5), (6, 5.5), (9.2, 3.5)], (236, 90, 60))
	return _Done(C, (30, 14, 10))


def MarkerGoal():
	C = _C(12, 12)
	Pts = []
	for K in range(10):
		R = 5.6 if K % 2 == 0 else 2.4
		T = -np.pi / 2 + K * np.pi / 5
		Pts.append((6 + R * np.cos(T), 6.4 + R * np.sin(T)))
	A.Poly(C, Pts, (255, 214, 70))
	C.Px(5, 4, (255, 250, 200))
	return _Done(C, (60, 30, 10))


def MarkerChest():
	C = _C(12, 12)
	C.Rect(1, 4, 10, 10, LEATHER)
	C.Rect(1, 4, 10, 5, GOLD_D)
	C.Rect(5, 5, 6, 7, GOLD)
	return _Done(C, (30, 16, 10))


def MarkerSave():
	C = _C(12, 12)
	C.Rect(2, 1, 9, 7, (196, 150, 96))
	C.Rect(3, 2, 8, 6, PAPER)
	C.Rect(5, 7, 6, 11, LEATHER_D)
	return _Done(C, (30, 16, 10))


def MarkerExit():
	C = _C(12, 12)
	A.Poly(C, [(1, 6), (6, 1), (6, 4), (11, 4), (11, 8), (6, 8), (6, 11)], (110, 200, 255))
	A.Poly(C, [(3, 6), (6, 3), (6, 5), (9, 5)], (220, 245, 255))
	return _Done(C, (10, 20, 40))


def MarkerBoss():
	C = _C(12, 12)
	C.Ellipse(6, 5, 4.8, 4.4, (240, 90, 80))
	C.Rect(3, 8, 8, 10, (240, 90, 80))
	C.Ellipse(4.3, 5.2, 1.2, 1.4, INK)
	C.Ellipse(7.7, 5.2, 1.2, 1.4, INK)
	return _Done(C, (40, 10, 10))


MARKERS = {
	"Player": MarkerPlayer, "Goal": MarkerGoal, "Chest": MarkerChest, "Save": MarkerSave, "Exit": MarkerExit, "Boss": MarkerBoss,
	"Npc": lambda: _Marker((110, 210, 120), (210, 255, 200)), "NpcQuest": lambda: _Marker((255, 200, 70), (255, 250, 200), "Diamond"),
}


# ---------------------------------------------------------------- 재료 아이콘 (16x16)
def MatBatWing():
	C = _C()
	A.Poly(C, [(1, 4), (6, 6), (8, 12), (5, 10), (3, 11), (2, 8)], (110, 70, 130))
	A.Poly(C, [(15, 4), (10, 6), (8, 12), (11, 10), (13, 11), (14, 8)], (110, 70, 130))
	C.Line(2, 5, 7, 10, (160, 120, 180))
	C.Line(14, 5, 9, 10, (160, 120, 180))
	return _Done(C)


def MatFang():
	C = _C()
	A.Poly(C, [(5, 2), (11, 2), (9, 14), (8, 14)], (244, 238, 216))
	A.Poly(C, [(5, 2), (7, 2), (8, 13)], (206, 196, 170))
	C.Rect(4, 1, 12, 2, (150, 110, 70))
	return _Done(C)


def MatBone():
	C = _C()
	C.Line(4, 11, 11, 4, (232, 226, 206))
	C.Line(5, 12, 12, 5, (200, 192, 170))
	for CX, CY in ((3, 11), (4, 13), (11, 3), (13, 4)):
		C.Ellipse(CX, CY, 1.8, 1.8, (232, 226, 206))
	return _Done(C)


def MatSpore():
	C = _C()
	for CX, CY, R in ((6, 9, 3.2), (10.5, 7, 2.6), (9, 11.5, 2.4), (4.5, 5, 1.8)):
		C.Ellipse(CX, CY, R, R, (180, 110, 210))
		C.Px(CX - 1, CY - 1, (230, 190, 250))
	return _Done(C)


def MatShard():
	C = _C()
	A.Poly(C, [(8, 1), (12, 7), (9, 15), (5, 9)], A.CRYSTAL)
	A.Poly(C, [(8, 1), (12, 7), (8.5, 8)], A.CRYSTAL_L)
	A.Poly(C, [(5, 9), (8.5, 8), (9, 15)], A.CRYSTAL_D)
	return _Done(C)


def MatCore():
	C = _C()
	C.Ellipse(8, 8.5, 6.2, 6.0, (120, 110, 104))
	C.Ellipse(8, 8.5, 3.6, 3.6, (255, 150, 60))
	C.Ellipse(7, 7.5, 1.6, 1.6, (255, 230, 160))
	C.Line(3, 6, 5, 8, (80, 72, 70))
	C.Line(12, 11, 13, 13, (80, 72, 70))
	return _Done(C)


MATERIAL_ICONS = {"BatWing": MatBatWing, "Fang": MatFang, "Bone": MatBone, "Spore": MatSpore, "Shard": MatShard, "Core": MatCore}


# ---------------------------------------------------------------- 도감 그림
def _Crop(C):
	Alpha = C.P[..., 3] > 0
	Ys, Xs = np.where(Alpha)
	Out = A.FCanvas(int(Xs.max() - Xs.min() + 1), int(Ys.max() - Ys.min() + 1))
	Out.P = C.P[Ys.min():Ys.max() + 1, Xs.min():Xs.max() + 1].copy()
	return Out


def _Tint(C, Tint):
	Out = A.FCanvas(C.W, C.H)
	P = C.P.astype(np.float32)
	P[..., :3] = np.clip(P[..., :3] * np.array(Tint[:3], dtype=np.float32), 0, 255)
	Out.P = P.astype(np.uint8)
	return Out


PICTURES = {  # 적 종류 → 그리기 (도감 그림 — 대기 자세 첫 칸)
	"Slime": lambda: A.DrawSlime(*A.SLIME_FRAMES["Idle"][0]),
	"Bat": lambda: A.DrawBat(1),
	"Goblin": lambda: A.DrawGoblin("Idle", 0),
	"Archer": lambda: A.DrawArcher("Idle", 0),
	"Mushroom": lambda: A.DrawMushroom("Idle", 0),
	"Golem": lambda: A.DrawGolem("Idle", 0),
	"SpiderQueen": lambda: A.DrawSpiderQueen("Idle", 0),
}


def WritePicture(Path, Look, Tint=(1, 1, 1, 1), Box=150):
	# 도트를 정수배로 키워 Box 안에 (그림 크기 반환 — 도감 칸 ImageSize)
	C = _Crop(PICTURES[Look]())
	if tuple(Tint[:3]) != (1, 1, 1):
		C = _Tint(C, Tint)
	Scale = max(1, int(Box // max(C.W, C.H)))
	A.UpscaleSave(C, Path, Scale)
	return C.W * Scale, C.H * Scale


def WriteOpenFrame(Path):
	# 가운데가 빈 창틀 (미니맵처럼 안쪽 그림 위에 겹칠 때) — HD2DArt.DrawFrame9와 같은 테두리, 안쪽 투명
	C = A.DrawFrame9()
	C.P[5:19, 5:19] = (0, 0, 0, 0)
	A.UpscaleSave(C, Path)


def WriteIcons(UiFolder):
	for Name, Draw in MENU_ICONS.items():
		A.UpscaleSave(Draw(), os.path.join(UiFolder, "Meta", f"{Name}.png"))
	for Name, Draw in MARKERS.items():
		A.UpscaleSave(Draw(), os.path.join(UiFolder, "Meta", f"Mk{Name}.png"))
	for Name, Draw in MATERIAL_ICONS.items():
		A.UpscaleSave(Draw(), os.path.join(UiFolder, "Icons", f"{Name}.png"))
