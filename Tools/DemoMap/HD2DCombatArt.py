# HD-2D 데모 전투 깊이용 도트 아트 (CC0, 절차 생성 — 결정적). HD2DCombatGen.WriteArt가 부른다.
#   UI(4배 최근접 확대 PNG — UI 샘플러가 선형): 공격 속성 배지 7종 + 미확인 "?"(약점 칸), 실드 아이콘(평소/깨짐), 상태 이상 배지 4종,
#             스킬 아이콘(무기 기술·마법), 해독제 아이템 아이콘, 스킬 칸 테두리.
#   월드 효과(Sprites/HD2D/Combat.esprite, 점 필터): 화염구·폭발, 얼음 창·서리 폭발, 낙뢰·연쇄 번개 줄기, 빛의 기둥, 마법진(눕혀 씀, 색은 스크립트가 곱함),
#             회오리 베기 고리, 실드 깨짐, 기절 별, 상태 이상 불꽃/거품/서리.
#   규약은 HD2DArt.py와 같다 (1 도트 = 6cm, 오른쪽을 향한 그림을 스크립트가 화면 각으로 돌린다, 외곽선은 Outline이 두른다).
import math
import os

import numpy as np

from HD2DArt import (FCanvas, FAtlas, Poly, UpscaleSave, WriteFlipbook, WHITE, BLADE, BLADE_D, HILT, SHAFT, SHAFT_D, BOW, BOW_D, STRING, GOLD, GOLD_L, GOLD_D,
					 OUTLINE)

# ---- 속성 색 (배지 테두리·기호) ---------------------------------------------------------------------------------------------
FIRE    = ((255, 248, 196), (255, 196, 64), (240, 108, 40), (176, 44, 36))     # 심 / 밝음 / 가운데 / 바깥
ICE     = ((248, 255, 255), (178, 236, 255), (96, 184, 246), (52, 104, 196))
THUNDER = ((255, 255, 236), (255, 238, 96), (246, 186, 40), (168, 104, 24))
LIGHT   = ((255, 255, 250), (255, 244, 186), (246, 210, 110), (190, 150, 70))
POISON  = ((236, 196, 255), (190, 120, 240), (132, 70, 190), (76, 36, 120))
STEEL   = ((236, 242, 252), (196, 206, 228), (140, 152, 186), (78, 86, 116))
PLATE   = (30, 24, 48)       # 배지 바탕
PLATE_L = (54, 44, 80)
EDGE    = (10, 8, 16)

# 배지 테두리 색 (속성마다 — 한눈에 구분)
ELEMENT_RIM = {
	"Slash": (196, 206, 228), "Pierce": (226, 176, 120), "Bow": (150, 210, 120), "Fire": (240, 108, 40), "Ice": (96, 184, 246),
	"Thunder": (246, 196, 40), "Light": (255, 236, 160), "Unknown": (110, 100, 140),
}


def _Glyph(C, X, Y, Rows, Col, Shadow=None):
	# 문자 모양 (행 문자열 "#" = 칠함). Shadow면 오른쪽 아래 한 칸에 그림자
	if Shadow:
		for J, Row in enumerate(Rows):
			for I, Ch in enumerate(Row):
				if Ch == "#":
					C.Px(X + I + 1, Y + J + 1, Shadow)
	for J, Row in enumerate(Rows):
		for I, Ch in enumerate(Row):
			if Ch == "#":
				C.Px(X + I, Y + J, Col)


def _Badge(C, Rim, bRound=False):
	# 16x16 배지: 어두운 바탕(위쪽 한 줄 밝게) + 속성 색 테두리. 둥근 배지 = 상태 이상
	if bRound:
		C.Ellipse(8, 8, 7.4, 7.4, Rim)
		C.Ellipse(8, 8, 6.2, 6.2, PLATE, 240)
		C.Ellipse(8, 7, 5.0, 4.0, PLATE_L, 240)
		C.Ellipse(8, 8.6, 5.4, 4.6, PLATE, 240)
	else:
		C.Rect(1, 2, 14, 13, Rim)
		C.Rect(2, 1, 13, 14, Rim)
		C.Rect(2, 2, 13, 13, PLATE, 240)
		C.Rect(3, 2, 12, 2, PLATE_L, 240)


def _Flame(C, CX, Bottom, H, W, Pal, Lean=0.0):
	# 불꽃 (아래 둥글고 위로 뾰족, 3겹) — Pal = FIRE 같은 4색
	def Shape(Scale, Col):
		Hs, Ws = H * Scale, W * Scale
		Pts = []
		for K in range(17):
			T = K / 16.0
			A = math.pi * T
			# 아래 반원 + 위로 모이는 혀
			X = CX + math.cos(A) * Ws * 0.5
			Y = Bottom - Ws * 0.5 + math.sin(A) * Ws * 0.5
			Pts.append((X, Y))
		Pts = [(CX - Ws * 0.5, Bottom - Ws * 0.5)] + Pts[::-1] + [(CX + Ws * 0.5, Bottom - Ws * 0.5)]
		Pts = Pts + [(CX + Ws * 0.36 + Lean * 0.4, Bottom - Hs * 0.62), (CX + Lean, Bottom - Hs), (CX - Ws * 0.2 + Lean * 0.5, Bottom - Hs * 0.7),
					 (CX - Ws * 0.42, Bottom - Hs * 0.55)]
		Poly(C, Pts, Col)
	Shape(1.0, Pal[3])
	Shape(0.78, Pal[2])
	Shape(0.52, Pal[1])
	C.Ellipse(CX, Bottom - W * 0.28, W * 0.16, W * 0.2, Pal[0])


def _Snowflake(C, CX, CY, R, Col, ColL, bBranches=True):
	for K in range(3):
		A = math.radians(90 + K * 60)
		DX, DY = math.cos(A) * R, math.sin(A) * R
		C.Line(CX - DX, CY - DY, CX + DX, CY + DY, Col)
		if bBranches:
			for S in (-1, 1):
				BX, BY = CX + DX * 0.6 * S, CY + DY * 0.6 * S
				for T in (-1, 1):
					B = A + math.radians(35 * T) + (math.pi if S < 0 else 0)
					C.Px(BX + math.cos(B + math.pi) * 1.5, BY + math.sin(B + math.pi) * 1.5, ColL)
	C.Px(CX, CY, WHITE)


def _Bolt(C, Points, Col, ColL):
	# 번개 모양 다각형 (Points = 시계 방향 외곽) + 가운데 밝은 줄
	Poly(C, Points, Col)
	for I in range(len(Points) // 2 - 1):
		C.Px((Points[I][0] + Points[-1 - I][0]) * 0.5, (Points[I][1] + Points[-1 - I][1]) * 0.5, ColL)


def _Star(C, CX, CY, R, Col, ColL):
	# 빛: 둥근 심 + 긴 빛살 4개(상하좌우) + 짧은 빛살 4개(대각)
	for K in range(8):
		A = math.radians(K * 45)
		L = R if K % 2 == 0 else R * 0.62
		C.Line(CX + math.cos(A) * 2.6, CY + math.sin(A) * 2.6, CX + math.cos(A) * L, CY + math.sin(A) * L, Col if K % 2 == 0 else ColL)
	C.Ellipse(CX + 0.5, CY + 0.5, 2.4, 2.4, ColL)
	C.Ellipse(CX + 0.5, CY + 0.5, 1.3, 1.3, WHITE)


QUESTION = [".###.",
			"#...#",
			"....#",
			"...#.",
			"..#..",
			".....",
			"..#.."]


# ================================================================ 속성 배지 (약점 칸)
def DrawElement(Name):
	C = FCanvas(16, 16)
	_Badge(C, ELEMENT_RIM[Name])
	if Name == "Slash":
		C.Line(4, 11, 11, 4, BLADE)
		C.Line(5, 11, 11, 5, BLADE_D)
		C.Px(11, 3, WHITE)
		C.Line(3, 9, 6, 12, HILT)
		C.Px(3, 12, SHAFT)
	elif Name == "Pierce":
		C.Line(3, 12, 9, 6, SHAFT)
		C.Line(4, 12, 9, 7, SHAFT_D)
		Poly(C, [(8.5, 5.5), (12.8, 3.2), (10.5, 7.5)], STEEL[1])
		C.Line(9, 6, 12, 3, WHITE)
		C.Px(8, 8, (206, 52, 54))
		C.Px(7, 8, (206, 52, 54))
	elif Name == "Bow":
		# 왼쪽에 활대(D자 호), 세로 시위, 오른쪽을 향한 화살
		for K in range(15):
			A = math.radians(-70 + K * 10)
			C.Px(8.5 - math.cos(A) * 4.6, 8 + math.sin(A) * 5.6, BOW)
		C.Px(7, 2, BOW_D)
		C.Px(7, 13, BOW_D)
		C.Line(7, 3, 7, 12, STRING)
		C.Line(4, 8, 11, 8, (220, 200, 160))
		C.Px(12, 8, WHITE)
		C.Px(11, 7, BLADE)
		C.Px(11, 9, BLADE)
		C.Px(4, 7, (206, 52, 54))
		C.Px(4, 9, (206, 52, 54))
	elif Name == "Fire":
		_Flame(C, 8, 13, 10, 7.5, FIRE, 0.6)
	elif Name == "Ice":
		_Snowflake(C, 8, 8, 4.6, ICE[2], ICE[1])
		C.Px(8, 3, ICE[0])
		C.Px(8, 13, ICE[0])
	elif Name == "Thunder":
		_Bolt(C, [(9.5, 2.5), (5, 8.6), (7.8, 8.6), (6, 13.5), (11.2, 6.6), (8.4, 6.6), (10.8, 2.5)], THUNDER[1], THUNDER[0])
		C.Line(9, 3, 6, 8, THUNDER[0])
	elif Name == "Light":
		_Star(C, 7, 7, 5, LIGHT[2], LIGHT[1])
	elif Name == "Unknown":
		_Glyph(C, 6, 4, QUESTION, (214, 206, 236), (14, 10, 22))
	C.Outline(EDGE)
	return C


ELEMENTS = ("Slash", "Pierce", "Bow", "Fire", "Ice", "Thunder", "Light", "Unknown")


# ================================================================ 실드 (평소 / 깨짐)
def DrawShield(bBroken):
	C = FCanvas(18, 18)
	Outer = [(2, 2), (16, 2), (16, 8.5), (9, 16.5), (2, 8.5)]
	Inner = [(3.6, 3.6), (14.4, 3.6), (14.4, 8.2), (9, 14.4), (3.6, 8.2)]
	if not bBroken:
		Poly(C, Outer, STEEL[3])
		Poly(C, Inner, STEEL[2])
		Poly(C, [(3.6, 3.6), (9, 3.6), (9, 14.4), (3.6, 8.2)], STEEL[1])  # 왼쪽 반 밝게
		C.Line(4, 4, 13, 4, STEEL[0])
		C.Line(9, 4, 9, 13, GOLD_D)
		C.Line(5, 7, 13, 7, GOLD_D)
		C.Px(9, 7, GOLD_L)
	else:
		# 금이 가 둘로 갈라진 방패 (오른쪽 반이 내려앉음) + 붉은 기운
		Left = [(2, 2), (8.6, 2), (7.2, 6), (9.6, 9.5), (8, 16), (2, 8.5)]
		Right = [(9.6, 3), (16, 3), (16, 9.5), (9.5, 17.2), (11, 10.4), (8.6, 7)]
		Poly(C, Left, (120, 70, 80))
		Poly(C, [(3.4, 3.4), (7.6, 3.4), (6.4, 6.2), (8.2, 9.4), (7.2, 13.6), (3.4, 8.3)], (210, 120, 120))
		Poly(C, Right, (96, 54, 66))
		Poly(C, [(10.6, 4.3), (14.8, 4.3), (14.8, 9.2), (10.6, 14.6), (11.6, 10.2), (9.9, 7.2)], (176, 92, 100))
		C.Px(8, 8, (255, 220, 160))
		C.Px(9, 5, (255, 220, 160))
	C.Outline(EDGE)
	return C


# ================================================================ 상태 이상 배지 (둥근)
STATUS_RIM = {"Poison": POISON[2], "Burn": FIRE[2], "Freeze": ICE[2], "Stun": THUNDER[1]}


def DrawStatus(Name):
	C = FCanvas(16, 16)
	_Badge(C, STATUS_RIM[Name], True)
	if Name == "Poison":
		Poly(C, [(8, 3), (11.2, 8.4), (11, 11), (8, 13), (5, 11), (4.8, 8.4)], POISON[1])
		Poly(C, [(8, 5), (9.6, 8.5), (8, 9)], POISON[0])
		C.Ellipse(8, 10.2, 2.6, 2.2, POISON[2])
		C.Px(7, 10, (40, 16, 56))
		C.Px(9, 10, (40, 16, 56))
		C.Px(12, 4, POISON[0])
		C.Px(4, 5, POISON[1])
	elif Name == "Burn":
		_Flame(C, 8, 13, 9, 6.6, FIRE, -0.5)
	elif Name == "Freeze":
		_Snowflake(C, 8, 8, 4.2, ICE[1], ICE[0])
	elif Name == "Stun":
		for K, (SX, SY) in enumerate(((5, 6), (11, 7), (8, 11))):
			Col = THUNDER[1] if K != 1 else THUNDER[0]
			C.Px(SX, SY, Col)
			for DX, DY in ((1, 0), (-1, 0), (0, 1), (0, -1)):
				C.Px(SX + DX, SY + DY, THUNDER[2] if K != 1 else THUNDER[1])
	C.Outline(EDGE)
	return C


STATUSES = ("Poison", "Burn", "Freeze", "Stun")


# ================================================================ 스킬 아이콘 (16x16 — 스킬 칸 안, 바탕은 칸 테두리가 그린다)
def _Sword(C, X0, Y0, X1, Y1, Blade=BLADE, BladeD=BLADE_D, Hilt=HILT):
	C.Line(X0, Y0, X1, Y1, Blade)
	C.Line(X0 + 1, Y0, X1, Y1 + 1, BladeD)
	C.Px(X1, Y1, WHITE)
	C.Line(X0 - 2, Y0 - 2, X0 + 1, Y0 + 1, Hilt)  # 코등이 (칼날과 수직)
	C.Line(X0 - 1, Y0 + 1, X0 - 2, Y0 + 2, SHAFT)


def DrawSkillIcon(Name):
	C = FCanvas(16, 16)
	if Name == "Whirl":
		# 위로 세운 검 + 둘레를 도는 3/4 고리(끝에 화살촉) — 회전 베기
		for K in range(28):
			A = math.radians(150 + K * 10)
			X, Y = 8 + math.cos(A) * 6.2, 9 + math.sin(A) * 3.6
			C.Px(X, Y, WHITE if K > 18 else (255, 214, 120))
		C.Px(13, 11, WHITE)
		C.Px(12, 12, WHITE)
		C.Px(14, 12, (255, 214, 120))
		C.Line(8, 1, 8, 10, BLADE)
		C.Line(9, 2, 9, 10, BLADE_D)
		C.Px(8, 0, WHITE)
		C.Line(6, 11, 10, 11, HILT)
		C.Line(8, 12, 8, 14, SHAFT)
	elif Name == "FlameSlash":
		_Flame(C, 10, 14, 11, 7, FIRE, 1.2)
		_Sword(C, 4, 12, 12, 4)
	elif Name == "FrostSlash":
		_Snowflake(C, 11, 11, 3.6, ICE[2], ICE[0])
		_Sword(C, 4, 12, 12, 4, ICE[1], ICE[3], (176, 104, 250))
	elif Name == "PierceRush":
		for Y, L in ((5, 4), (8, 6), (11, 3)):
			C.Line(1, Y, 1 + L, Y, (180, 200, 240))
		C.Line(5, 12, 11, 6, SHAFT)
		Poly(C, [(10.5, 5.5), (14.8, 2.8), (12.3, 7.4)], STEEL[1])
		C.Line(11, 6, 14, 3, WHITE)
		C.Px(9, 9, (206, 52, 54))
	elif Name == "ThunderThrust":
		C.Line(2, 14, 9, 7, SHAFT)
		Poly(C, [(8.5, 6.5), (12.8, 3.8), (10.4, 8.3)], STEEL[1])
		_Bolt(C, [(12.5, 6), (9.5, 10), (11.4, 10), (10, 14.2), (14.5, 8.8), (12.4, 8.8), (14, 6)], THUNDER[1], THUNDER[0])
	elif Name == "ArrowRain":
		for X0 in (3, 7, 11):
			Y0 = 2 + (X0 % 3)
			C.Line(X0, Y0, X0, Y0 + 7, (220, 200, 160))
			C.Px(X0 - 1, Y0 + 7, BLADE)
			C.Px(X0 + 1, Y0 + 7, BLADE)
			C.Px(X0, Y0 + 8, WHITE)
			C.Px(X0 - 1, Y0, (206, 52, 54))
			C.Px(X0 + 1, Y0, (206, 52, 54))
		C.Line(1, 14, 14, 14, (90, 70, 50))
	elif Name == "FrostArrow":
		C.Line(2, 13, 10, 5, (220, 200, 160))
		C.Px(2, 12, WHITE)
		C.Px(3, 14, WHITE)
		Poly(C, [(9, 4.5), (14.5, 1.8), (11.8, 7.2)], ICE[1])
		C.Line(10, 5, 14, 2, ICE[0])
		_Snowflake(C, 12, 11, 2.6, ICE[2], ICE[0], False)
	elif Name == "HolyPillar":
		for X in range(5, 11):
			D = abs(X + 0.5 - 8) / 3.0
			C.Line(X, 1, X, 13, LIGHT[1] if D < 0.45 else LIGHT[3])
		C.Line(8, 1, 8, 13, WHITE)
		C.Ellipse(8, 13.5, 6.5, 1.8, LIGHT[2])
		C.Px(3, 4, LIGHT[1])
		C.Px(13, 7, LIGHT[1])
	elif Name == "ChainLightning":
		Pts = [(2, 3), (6, 6), (4, 8), (9, 10), (7, 12), (13, 14)]
		for (X0, Y0), (X1, Y1) in zip(Pts, Pts[1:]):
			C.Line(X0, Y0, X1, Y1, THUNDER[1])
		C.Line(6, 6, 11, 4, THUNDER[2])
		C.Line(11, 4, 14, 6, THUNDER[2])
		for X, Y in ((2, 3), (11, 4), (13, 14)):
			C.Ellipse(X + 0.5, Y + 0.5, 1.6, 1.6, THUNDER[0])
	elif Name == "Fireball":
		for K in range(6):
			C.Line(1 + K, 12 - K * 0.3, 6, 9, FIRE[3] if K < 3 else FIRE[2])
		C.Ellipse(10, 7, 4.6, 4.6, FIRE[3])
		C.Ellipse(10, 7, 3.4, 3.4, FIRE[2])
		C.Ellipse(10.6, 6.4, 2.0, 2.0, FIRE[1])
		C.Px(11, 6, FIRE[0])
	elif Name == "IceLance":
		Poly(C, [(2, 13), (5.5, 8.5), (13.5, 2.5), (7.5, 10.5)], ICE[2])
		Poly(C, [(4, 11), (13.5, 2.5), (6, 9)], ICE[1])
		C.Line(6, 9, 12, 4, ICE[0])
		C.Px(3, 6, ICE[1])
		C.Px(11, 12, ICE[1])
	elif Name == "Heal":
		C.Rect(6, 3, 9, 12, (90, 220, 120))
		C.Rect(3, 6, 12, 9, (90, 220, 120))
		C.Rect(7, 4, 8, 11, (190, 255, 200))
		C.Rect(4, 7, 11, 8, (190, 255, 200))
		C.Px(13, 3, WHITE)
		C.Px(2, 12, (190, 255, 200))
	C.Outline(EDGE)
	return C


SKILL_ICONS = ("Whirl", "FlameSlash", "FrostSlash", "PierceRush", "ThunderThrust", "ArrowRain", "FrostArrow", "HolyPillar", "ChainLightning",
			   "Fireball", "IceLance", "Heal")


def DrawAntidote():
	# 해독제: 초록 약병 + 잎
	C = FCanvas(16, 16)
	C.Ellipse(8, 10.5, 4.6, 4.2, (220, 230, 240))
	C.Ellipse(8, 11, 3.7, 3.2, (70, 190, 110))
	C.Ellipse(6.6, 9.6, 1.2, 1.0, (190, 255, 200))
	C.Rect(6, 3, 9, 6, (220, 230, 240))
	C.Rect(6, 2, 9, 3, (150, 100, 60))
	Poly(C, [(10, 4), (14.5, 1.5), (13, 5.5)], (90, 170, 70))
	C.Px(12, 3, (170, 230, 120))
	C.Outline((24, 16, 28))
	return C


def DrawSkillFrame(bLocked):
	# 스킬 칸 바탕 (20x20): 금테 + 어두운 안. 잠긴 칸은 회색 테
	C = FCanvas(20, 20)
	Rim, RimD = ((150, 140, 170), (90, 84, 110)) if bLocked else (GOLD, GOLD_D)
	C.Rect(1, 1, 18, 18, RimD)
	C.Rect(2, 2, 17, 17, Rim)
	C.Rect(3, 3, 16, 16, (18, 16, 34), 235)
	C.Rect(3, 3, 16, 4, (40, 34, 66), 235)
	for X, Y in ((2, 2), (17, 2), (2, 17), (17, 17)):
		C.Px(X, Y, GOLD_L if not bLocked else (200, 196, 214))
	C.Outline(EDGE)
	return C


# ================================================================ 월드 효과
def DrawFireball(Frame):
	# 오른쪽으로 나는 화염구: 머리(흰 심 → 노랑 → 주황 → 빨강) + 뒤로 흩날리는 불꼬리 (프레임마다 흔들림)
	C = FCanvas(24, 16)
	Rng = [((K * 37 + Frame * 53) % 17) / 16.0 for K in range(12)]
	for K in range(9):
		X = 13 - K * 1.35
		Y = 8 + math.sin(K * 1.3 + Frame * 1.9) * (0.4 + K * 0.22)
		R = 4.2 - K * 0.38
		if R > 0.6:
			C.Ellipse(X, Y, R, R * 0.92, FIRE[3] if K > 3 else FIRE[2])
	for K in range(5):
		X = 10 - K * 2.0 - Rng[K] * 1.5
		Y = 8 + (Rng[K + 5] - 0.5) * 6
		C.Px(X, Y, FIRE[1] if K < 2 else FIRE[2])
	C.Ellipse(15.5, 8, 4.8, 4.4, FIRE[2])
	C.Ellipse(16.2, 7.8, 3.4, 3.0, FIRE[1])
	C.Ellipse(16.8, 7.6, 1.8, 1.6, FIRE[0])
	C.Px(18, 7, WHITE)
	return C


def DrawFireBurst(Frame):
	# 폭발: 0 흰 섬광 → 1·2 불덩이가 부풀며 혀를 내밈 → 3 붉은 고리 + 연기 → 4·5 연기가 흩어짐
	C = FCanvas(48, 48)
	Cx, Cy = 24, 25
	if Frame <= 2:
		R = (6.0, 12.0, 16.0)[Frame]
		for K in range(10):
			A = math.radians(K * 36 + Frame * 13)
			L = R * (1.15 + 0.25 * ((K * 7) % 3))
			C.Ellipse(Cx + math.cos(A) * L * 0.72, Cy + math.sin(A) * L * 0.72, R * 0.42, R * 0.42, FIRE[3])
		C.Ellipse(Cx, Cy, R, R, FIRE[2])
		C.Ellipse(Cx - R * 0.12, Cy - R * 0.12, R * 0.72, R * 0.72, FIRE[1])
		C.Ellipse(Cx - R * 0.2, Cy - R * 0.2, R * 0.42, R * 0.42, FIRE[0])
		if Frame == 0:
			C.Ellipse(Cx, Cy, R * 0.55, R * 0.55, WHITE)
	else:
		Smoke, SmokeD = ((108, 96, 104), (72, 62, 74)) if Frame < 5 else ((96, 88, 98), (66, 58, 70))
		Ring = (17.0, 19.5, 21.0)[Frame - 3]
		Puff = (4.6, 4.0, 3.0)[Frame - 3]
		for K in range(9):
			A = math.radians(K * 40 + 10)
			X, Y = Cx + math.cos(A) * Ring * 0.8, Cy + math.sin(A) * Ring * 0.8 - (Frame - 3) * 2
			C.Ellipse(X, Y, Puff, Puff, SmokeD)
			C.Ellipse(X - 0.8, Y - 0.8, Puff * 0.7, Puff * 0.7, Smoke)
		if Frame == 3:
			for K in range(12):
				A = math.radians(K * 30)
				C.Px(Cx + math.cos(A) * (Ring - 4), Cy + math.sin(A) * (Ring - 4), FIRE[2])
				C.Px(Cx + math.cos(A) * (Ring - 6), Cy + math.sin(A) * (Ring - 6), FIRE[1])
		if Frame == 5:
			C.P[..., 3] = (C.P[..., 3] * 0.55).astype(np.uint8)
	return C


def DrawIceLance(Frame):
	# 오른쪽으로 나는 얼음 창: 길쭉한 마름모 결정 (위 모서리 밝게, 아래 어둡게) + 뒤에 흩어지는 서리 가루
	C = FCanvas(30, 11)
	Poly(C, [(4, 5.5), (12, 2), (28, 5.5), (12, 9)], ICE[3])
	Poly(C, [(6, 5.5), (12, 3), (27, 5.5)], ICE[1])
	Poly(C, [(6, 5.5), (27, 5.5), (12, 8)], ICE[2])
	C.Line(12, 3, 26, 5, ICE[0])
	C.Px(27, 5, WHITE)
	for K, (X, Y) in enumerate(((2, 3), (1, 7), (3, 9), (0, 5))):
		if (K + Frame) % 2 == 0:
			C.Px(X, Y, ICE[1])
	return C


def DrawFrost(Frame):
	# 서리 폭발: 수정 가시 6개가 뻗었다가(0~2) 부서져 반짝임으로(3~4)
	C = FCanvas(44, 44)
	Cx, Cy = 22, 22
	Grow = (0.45, 0.85, 1.0, 1.0, 0.0)[Frame]
	if Frame <= 3:
		for K in range(6):
			A = math.radians(K * 60 + 30)
			L = 18 * Grow * (1.0 if K % 2 == 0 else 0.78)
			W = 3.4 * (1.0 if Frame < 3 else 0.6)
			Tip = (Cx + math.cos(A) * L, Cy + math.sin(A) * L)
			N = (-math.sin(A) * W, math.cos(A) * W)
			Mid = (Cx + math.cos(A) * L * 0.35, Cy + math.sin(A) * L * 0.35)
			Poly(C, [(Cx, Cy), (Mid[0] + N[0], Mid[1] + N[1]), Tip, (Mid[0] - N[0], Mid[1] - N[1])], ICE[2] if Frame < 3 else ICE[3])
			C.Line(Cx, Cy, Tip[0], Tip[1], ICE[0] if Frame < 3 else ICE[1])
		C.Ellipse(Cx, Cy, 4.5 * Grow + 1, 4.5 * Grow + 1, ICE[1])
		C.Ellipse(Cx, Cy, 2.2, 2.2, WHITE)
	if Frame >= 3:
		for K in range(10):
			A = math.radians(K * 36 + 18)
			R = (14, 19)[Frame - 3] + (K % 3) * 1.5
			X, Y = Cx + math.cos(A) * R, Cy + math.sin(A) * R
			C.Px(X, Y, WHITE)
			if Frame == 3:
				for DX, DY in ((1, 0), (-1, 0), (0, 1), (0, -1)):
					C.Px(X + DX, Y + DY, ICE[1])
	return C


def _Zigzag(Seed, X0, Y0, X1, Y1, Steps, Amp):
	Pts = [(X0, Y0)]
	for K in range(1, Steps):
		T = K / Steps
		Off = (((Seed * 7 + K * 13) % 11) / 10.0 - 0.5) * 2 * Amp
		X = X0 + (X1 - X0) * T
		Y = Y0 + (Y1 - Y0) * T
		# 진행 방향에 수직으로 흔든다
		DX, DY = X1 - X0, Y1 - Y0
		L = max(math.hypot(DX, DY), 1e-6)
		Pts.append((X + -DY / L * Off, Y + DX / L * Off))
	Pts.append((X1, Y1))
	return Pts


def DrawLightning(Frame):
	# 낙뢰 (피벗 = 아래 가운데): 하늘에서 땅까지 지그재그 + 가지. 0 가는 예고 → 1 굵고 밝게 → 2 가늘게 → 3 잔광 가지만
	W, H = 28, 80
	C = FCanvas(W, H)
	Main = _Zigzag(3, 14, 0, 14, H - 2, 12, 4.5)
	Thick = (0, 2, 1, -1)[Frame]
	Col = (THUNDER[1], THUNDER[0], THUNDER[1], THUNDER[2])[Frame]
	if Thick >= 0:
		for (X0, Y0), (X1, Y1) in zip(Main, Main[1:]):
			for D in range(-Thick, Thick + 1):
				C.Line(X0 + D, Y0, X1 + D, Y1, THUNDER[2] if abs(D) == Thick and Thick > 0 else Col)
			C.Line(X0, Y0, X1, Y1, WHITE if Frame == 1 else Col)
	for K, Start in enumerate((3, 6, 9)):
		if Frame == 0 and K > 0:
			continue
		SX, SY = Main[Start]
		Side = -1 if K % 2 == 0 else 1
		Branch = _Zigzag(K + 5, SX, SY, SX + Side * (8 + K * 2), SY + 14, 4, 2.0)
		for (X0, Y0), (X1, Y1) in zip(Branch, Branch[1:]):
			C.Line(X0, Y0, X1, Y1, THUNDER[1] if Frame < 3 else THUNDER[3])
	if Frame in (1, 2):
		C.Ellipse(14, H - 3, 7 if Frame == 1 else 5, 2.6, THUNDER[1])
		C.Ellipse(14, H - 3, 3.5, 1.4, WHITE)
	return C


def DrawZap(Frame):
	# 연쇄 번개 줄기 (가로 64 — 스크립트가 두 점 사이로 돌리고 늘린다). 프레임마다 다른 지그재그
	C = FCanvas(64, 14)
	Pts = _Zigzag(Frame * 5 + 1, 0, 7, 63, 7, 9, 4.0)
	for (X0, Y0), (X1, Y1) in zip(Pts, Pts[1:]):
		C.Line(X0, Y0 + 1, X1, Y1 + 1, THUNDER[2])
		C.Line(X0, Y0 - 1, X1, Y1 - 1, THUNDER[1])
		C.Line(X0, Y0, X1, Y1, WHITE)
	return C


def DrawHolyBeam(Frame):
	# 빛의 기둥 (피벗 = 아래 가운데): 가는 빛줄기가 내려와(0) 굵어지고(1·2) 가늘어지며 사라짐(3·4). 가운데 흰 심 + 금빛 가장자리
	W, H = 32, 100
	C = FCanvas(W, H)
	Half = (2.0, 9.0, 11.0, 6.5, 3.0)[Frame]
	Top = (H * 0.55, 0, 0, 0, 0)[Frame]
	for Y in range(int(Top), H):
		T = Y / (H - 1)
		Fade = min(1.0, T * 2.2) if Frame >= 3 else 1.0
		for X in range(W):
			D = abs(X + 0.5 - W * 0.5) / max(Half, 0.5)
			if D <= 1.0:
				Col = WHITE if D < 0.35 else (LIGHT[1] if D < 0.7 else LIGHT[2])
				C.Px(X, Y, Col, int(255 * Fade * (1.0 - 0.35 * D)))
	if Frame in (1, 2, 3):
		for K in range(7):
			X = W * 0.5 + ((K * 9) % 13 - 6) * (1.6 if Frame > 1 else 1.0)
			Y = H - 6 - ((K * 17 + Frame * 23) % 70)
			C.Px(X, Y, WHITE)
			C.Px(X, Y - 1, LIGHT[1])
		C.Ellipse(W * 0.5, H - 2, Half + 4, 2.2, LIGHT[1])
	return C


def DrawMagicCircle(Frame):
	# 마법진 (위에서 본 원 — 눕혀 쓴다, 흰색으로 그려 스크립트가 속성 색을 곱한다): 이중 고리 + 육망성 + 룬 점
	S = 56
	C = FCanvas(S, S)
	Cx = Cy = S * 0.5
	Rot = math.radians(Frame * 15)
	for Y in range(S):
		for X in range(S):
			D = math.hypot(X + 0.5 - Cx, Y + 0.5 - Cy)
			if abs(D - 26) <= 1.0:
				C.Px(X, Y, WHITE)
			elif abs(D - 21.5) <= 0.7:
				C.Px(X, Y, (210, 210, 220))
	for K in range(2):
		Pts = [(Cx + math.cos(Rot + math.radians(90 + K * 60 + J * 120)) * 21, Cy + math.sin(Rot + math.radians(90 + K * 60 + J * 120)) * 21) for J in range(3)]
		for J in range(3):
			C.Line(Pts[J][0], Pts[J][1], Pts[(J + 1) % 3][0], Pts[(J + 1) % 3][1], (230, 230, 240))
	for K in range(12):
		A = Rot * -1.5 + math.radians(K * 30)
		X, Y = Cx + math.cos(A) * 23.8, Cy + math.sin(A) * 23.8
		C.Rect(int(X), int(Y), int(X) + (K % 2), int(Y), WHITE)
	C.Ellipse(Cx, Cy, 3, 3, WHITE)
	return C


def DrawWhirl(Frame):
	# 회오리 베기 (서 있는 몸을 두르는 수평 고리 — 카메라에서 보면 납작한 타원). 0 앞 오른쪽 → 1 반 바퀴 → 2 온 바퀴 → 3 가늘게 사라짐
	W, H = 76, 34
	C = FCanvas(W, H)
	Cx, Cy, RX, RY = W * 0.5, H * 0.5, 34.0, 13.0
	Sweep = ((-30, 80), (-30, 200), (-30, 330), (0, 360))[Frame]
	Thick = (3.6, 4.2, 4.2, 1.6)[Frame]
	for Y in range(H):
		for X in range(W):
			DX, DY = (X + 0.5 - Cx) / RX, (Y + 0.5 - Cy) / RY
			R = math.hypot(DX, DY)
			A = (math.degrees(math.atan2(DY, DX)) + 360) % 360
			Rel = (A - Sweep[0]) % 360
			if Rel > Sweep[1] - Sweep[0]:
				continue
			T = Rel / max(Sweep[1] - Sweep[0], 1)  # 0 = 꼬리, 1 = 머리
			Width = Thick * (0.25 + 0.75 * T) / RY
			if abs(R - 1.0) <= Width * 0.5:
				Inner = abs(R - 1.0) <= Width * 0.2
				C.Px(X, Y, (255, 255, 236) if Inner else (255, 214, 120), 255 if Frame < 3 else 170)
	return C


def DrawShatter(Frame):
	# 실드 깨짐: 0 하얗게 번쩍인 방패 → 1 금 → 2~5 조각이 사방으로 튀며 사라짐 (반짝임 동반)
	S = 40
	C = FCanvas(S, S)
	Cx, Cy = 20, 19
	Shards = [  # (조각 꼭짓점 — 방패 좌표 18칸 기준 가운데 (9, 9)), 날아가는 방향
		([(2, 2), (8.6, 2), (7.2, 6), (2, 8.5)], (-1.0, -0.8)),
		([(8.6, 2), (16, 2), (16, 7), (9.6, 7.2), (7.2, 6)], (1.0, -0.9)),
		([(2, 8.5), (7.2, 6), (9.6, 7.2), (8.8, 12), (5.5, 12.5)], (-1.1, 0.5)),
		([(9.6, 7.2), (16, 7), (16, 8.5), (12.5, 12.5), (8.8, 12)], (1.1, 0.6)),
		([(5.5, 12.5), (8.8, 12), (12.5, 12.5), (9, 16.5)], (0.1, 1.2)),
	]
	Spread = (0, 0, 3.0, 6.5, 10.0, 13.5)[Frame]
	for Pts, (DX, DY) in Shards:
		Moved = [(Cx - 9 + X + DX * Spread, Cy - 9 + Y + DY * Spread + (Spread * Spread) * 0.04) for X, Y in Pts]
		Col = WHITE if Frame == 0 else (STEEL[2] if Frame < 3 else STEEL[3])
		Poly(C, Moved, Col)
		if Frame >= 1:
			Poly(C, [(X * 0.7 + Moved[0][0] * 0.3, Y * 0.7 + Moved[0][1] * 0.3) for X, Y in Moved], STEEL[1] if Frame < 4 else STEEL[2])
	if Frame == 1:
		C.Line(Cx - 2, Cy - 7, Cx - 1, Cy - 3, (255, 236, 150))
		C.Line(Cx - 1, Cy - 3, Cx + 1, Cy - 2, (255, 236, 150))
		C.Line(Cx + 1, Cy - 2, Cx, Cy + 3, (255, 236, 150))
	C.Outline(EDGE)
	if Frame >= 2:
		for K in range(8):
			A = math.radians(K * 45 + 20)
			R = 6 + Spread * 1.25
			X, Y = Cx + math.cos(A) * R, Cy + math.sin(A) * R
			C.Px(X, Y, GOLD_L)
			if Frame < 4:
				C.Px(X + 1, Y, GOLD)
				C.Px(X, Y + 1, GOLD)
	if Frame == 5:
		C.P[..., 3] = (C.P[..., 3] * 0.5).astype(np.uint8)
	return C


def _SmallStar(C, X, Y, Col, ColL):
	C.Px(X, Y, ColL)
	for DX, DY in ((1, 0), (-1, 0), (0, 1), (0, -1)):
		C.Px(X + DX, Y + DY, Col)


def DrawStars(Frame):
	# 기절 별 3개가 머리 위 타원을 돈다 (앞쪽 별이 크다)
	C = FCanvas(32, 14)
	for K in range(3):
		A = math.radians(Frame * 30 + K * 120)
		X, Y = 16 + math.cos(A) * 12, 7 + math.sin(A) * 4
		bFront = math.sin(A) > 0
		_SmallStar(C, X, Y, THUNDER[1] if bFront else THUNDER[2], WHITE if bFront else THUNDER[1])
		if bFront:
			C.Px(X + 1, Y + 1, THUNDER[2])
	C.Outline((60, 40, 10))
	return C


def DrawStatusFlame(Frame):
	# 화상: 몸에 붙는 작은 불꽃 (피벗 아래) — 3프레임 깜빡
	C = FCanvas(12, 16)
	H = (12, 14, 11)[Frame]
	_Flame(C, 6, 15, H, 7, FIRE, (0.8, -0.6, 0.2)[Frame])
	return C


def DrawBubble(Frame):
	# 중독: 보라 거품이 오르며 터짐
	C = FCanvas(12, 16)
	for K, (X, Y0, R) in enumerate(((4, 14, 2.2), (8, 11, 1.6), (6, 7, 1.3))):
		Y = Y0 - Frame * 2.5
		if Y < 2:
			continue
		C.Ellipse(X, Y, R, R, POISON[2])
		C.Ellipse(X - 0.4, Y - 0.4, R * 0.6, R * 0.6, POISON[1])
		C.Px(X - 1, Y - 1, POISON[0])
	C.Outline((40, 16, 56))
	return C


def DrawFrostBit(Frame):
	# 빙결: 몸 둘레에 맺히는 작은 서리 결정
	C = FCanvas(12, 12)
	_Snowflake(C, 6, 6, (3.6, 4.6)[Frame], ICE[2], ICE[0])
	C.Outline((30, 50, 90))
	return C


# ================================================================ 쓰기
def WriteUi(UiFolder):
	for Name in ELEMENTS:
		UpscaleSave(DrawElement(Name), os.path.join(UiFolder, "Combat", f"Elem{Name}.png"))
	UpscaleSave(DrawShield(False), os.path.join(UiFolder, "Combat", "Shield.png"))
	UpscaleSave(DrawShield(True), os.path.join(UiFolder, "Combat", "ShieldBroken.png"))
	for Name in STATUSES:
		UpscaleSave(DrawStatus(Name), os.path.join(UiFolder, "Combat", f"Status{Name}.png"))
	for Name in SKILL_ICONS:
		UpscaleSave(DrawSkillIcon(Name), os.path.join(UiFolder, "Combat", f"Skill{Name}.png"))
	UpscaleSave(DrawSkillFrame(False), os.path.join(UiFolder, "Combat", "SkillFrame.png"))
	UpscaleSave(DrawSkillFrame(True), os.path.join(UiFolder, "Combat", "SkillFrameLocked.png"))
	UpscaleSave(DrawAntidote(), os.path.join(UiFolder, "Icons", "Antidote.png"))


def WriteSprites(Folder):
	Fx = FAtlas(256)
	for I in range(4):
		Fx.Add(f"Fireball{I}", DrawFireball(I), (0.62, 0.5))
	for I in range(6):
		Fx.Add(f"FireBurst{I}", DrawFireBurst(I), (0.5, 0.5))
	for I in range(2):
		Fx.Add(f"IceLance{I}", DrawIceLance(I), (0.6, 0.5))
	for I in range(5):
		Fx.Add(f"Frost{I}", DrawFrost(I), (0.5, 0.5))
	for I in range(4):
		Fx.Add(f"Lightning{I}", DrawLightning(I), (0.5, 0.0))
	for I in range(3):
		Fx.Add(f"Zap{I}", DrawZap(I), (0.0, 0.5))
	for I in range(5):
		Fx.Add(f"HolyBeam{I}", DrawHolyBeam(I), (0.5, 0.0))
	for I in range(2):
		Fx.Add(f"MagicCircle{I}", DrawMagicCircle(I), (0.5, 0.5))
	for I in range(4):
		Fx.Add(f"Whirl{I}", DrawWhirl(I), (0.5, 0.5))
	for I in range(6):
		Fx.Add(f"Shatter{I}", DrawShatter(I), (0.5, 0.5))
	for I in range(4):
		Fx.Add(f"Stars{I}", DrawStars(I), (0.5, 0.5))
	for I in range(3):
		Fx.Add(f"Flame{I}", DrawStatusFlame(I), (0.5, 0.0))
	for I in range(3):
		Fx.Add(f"Bubble{I}", DrawBubble(I), (0.5, 0.0))
	for I in range(2):
		Fx.Add(f"FrostBit{I}", DrawFrostBit(I), (0.5, 0.5))
	Fx.Save(Folder, "Combat")
	Book = lambda Name, Slices, Fps, Loop="Loop", Durations=None: WriteFlipbook(Folder, f"Combat_{Name}", "Combat.esprite", Slices, Fps, Loop, Durations)
	Book("Fireball", [f"Fireball{I}" for I in range(4)], 16.0)
	Book("FireBurst", [f"FireBurst{I}" for I in range(6)], 0, "Once", [0.04, 0.05, 0.07, 0.08, 0.09, 0.1])
	Book("IceLance", ["IceLance0", "IceLance1"], 12.0)
	Book("Frost", [f"Frost{I}" for I in range(5)], 0, "Once", [0.04, 0.05, 0.12, 0.08, 0.08])
	Book("Lightning", [f"Lightning{I}" for I in range(4)], 0, "Once", [0.05, 0.07, 0.06, 0.1])
	Book("Zap", [f"Zap{I}" for I in range(3)], 20.0)
	Book("HolyBeam", [f"HolyBeam{I}" for I in range(5)], 0, "Once", [0.06, 0.08, 0.2, 0.1, 0.1])
	Book("MagicCircle", ["MagicCircle0", "MagicCircle1"], 8.0)
	Book("Whirl", [f"Whirl{I}" for I in range(4)], 0, "Once", [0.04, 0.05, 0.06, 0.08])
	Book("Shatter", [f"Shatter{I}" for I in range(6)], 0, "Once", [0.05, 0.06, 0.06, 0.07, 0.08, 0.1])
	Book("Stars", [f"Stars{I}" for I in range(4)], 9.0)
	Book("Flame", [f"Flame{I}" for I in range(3)], 11.0)
	Book("Bubble", [f"Bubble{I}" for I in range(3)], 6.0)
	Book("FrostBit", ["FrostBit0", "FrostBit1"], 4.0)
