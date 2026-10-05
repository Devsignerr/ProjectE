# HD-2D 데모 4차(세계 확장) 도트 아트 — 항구 "갈매기 항구"(BuildHD2DHarbor.py가 WriteHarborArt를 부른다). 절차 생성·결정적 (CC0).
#   항구 소품(HarborProps: 생선 더미·매단 생선·조개·그림 표지판·깃발 펄럭임·렌즈 상자·갈매기), 항구 주민 7명(HarborNpcs — 4프레임 대기: 숨쉬기·눈 깜빡임·손에 든 물건 움직임),
#   항구 적(HarborEnemies — 바위 게·해적 졸개·갈매기·떠도는 망령, HD2DEnemy.lua 규약 Idle/Move/Windup/Attack), 보스 "해적 선장"(PirateCaptain 64x64),
#   항구 효과(HarborFx — 총알·화약통·포탄 그림자), UI(초상화·아이템 아이콘), 그물 텍스처.
#   규약은 HD2DArt.py와 같다: 1 도트 = 6cm, 캐릭터 피벗 = 발(아래 가운데), 옆모습은 오른쪽을 본다, 칠한 뒤 Outline()으로 외곽선.
#   명암: 왼쪽 위에서 빛 (밝은 면 = 왼쪽·위, 어두운 면 = 오른쪽·아래) — 메인 맵 도트와 같은 방향
import math
import os

import numpy as np
from PIL import Image

import HD2DArt as A
from HD2DArt import FCanvas, Poly, FAtlas, WriteFlipbook, UpscaleSave

OUT = A.OUTLINE
WOOD, WOOD_L, WOOD_D = A.WOOD, A.WOOD_L, A.WOOD_D
GOLD, GOLD_L, GOLD_D = A.GOLD, A.GOLD_L, A.GOLD_D

# ---- 팔레트 -------------------------------------------------------------------------------------------------------------
FISH = [((172, 196, 214), (226, 240, 248), (102, 124, 150)),   # 은빛 고등어 (몸, 밝음, 어두움)
		((110, 150, 196), (176, 210, 236), (60, 92, 138)),     # 푸른 등 생선
		((226, 128, 96), (255, 190, 150), (162, 76, 58))]      # 붉은 도미
CRAB, CRAB_L, CRAB_D = (214, 78, 56), (255, 140, 102), (140, 40, 36)
SEA_BLUE = (52, 112, 176)
ICE = (220, 236, 244)


def _Fish(C, X, Y, Length, Angle, Pal, bTail=True):
	# 위에서 본 생선 한 마리: 방추형 몸 + 꼬리 지느러미 + 눈 + 등 무늬 (Angle = 머리 방향, 도)
	Body, Light, Dark = Pal
	Ca, Sa = math.cos(math.radians(Angle)), math.sin(math.radians(Angle))
	Half = Length * 0.5
	Width = max(1.2, Length * 0.2)
	for Y_ in range(C.H):
		for X_ in range(C.W):
			DX, DY = X_ + 0.5 - X, Y_ + 0.5 - Y
			U = DX * Ca + DY * Sa          # 머리 쪽 +
			V = -DX * Sa + DY * Ca
			if abs(U) <= Half:
				W = Width * math.sqrt(max(0.0, 1.0 - (U / Half) ** 2)) * (1.0 if U > -Half * 0.3 else 0.85)
				if abs(V) <= W:
					Col = Light if V < -W * 0.35 else (Dark if V > W * 0.45 else Body)
					C.Px(X_, Y_, Col)
	if bTail:
		TX, TY = X - Ca * (Half + 0.6), Y - Sa * (Half + 0.6)
		for S in (-1, 1):
			C.Line(TX, TY, TX - Ca * 1.8 - Sa * S * 1.6, TY - Sa * 1.8 + Ca * S * 1.6, Dark)
	EX, EY = X + Ca * (Half - 1.4), Y + Sa * (Half - 1.4)
	C.Px(EX - Sa * 0.6, EY + Ca * 0.6 - 0.4, (30, 30, 40))


def _Paste(Dst, Src, OX, OY):
	# Src의 불투명 화소를 Dst 위에 덮어 그린다 (외곽선 포함 — 겹친 물고기가 서로 구분되게)
	for Y in range(Src.H):
		for X in range(Src.W):
			if Src.P[Y, X, 3] > 0:
				TX, TY = X + OX, Y + OY
				if 0 <= TX < Dst.W and 0 <= TY < Dst.H:
					Dst.P[TY, TX] = Src.P[Y, X]


def _FishSprite(Pal, bFlip=False, Length=9):
	# 옆으로 누운 생선 한 마리 (얼음 위 — 위에서 보면 옆모습): 등(어두움)·옆구리·배(밝음) + 등지느러미 + 꼬리 + 눈 + 아가미 선, 외곽선
	Body, Light, Dark = Pal
	W, H = Length + 4, 7
	F = FCanvas(W, H)
	CX, CY, RX, RY = 2.0 + Length * 0.5 + 0.5, 3.5, Length * 0.5, 1.9
	for Y in range(H):
		for X in range(W):
			DX, DY = (X + 0.5 - CX) / RX, (Y + 0.5 - CY) / RY
			if DX * DX + DY * DY <= 1.0:
				F.Px(X, Y, Dark if DY < -0.45 else (Light if DY > 0.35 else Body))
	F.Px(int(CX) - 1, 1, Dark)              # 등지느러미
	F.Px(int(CX), 1, Dark)
	for Y in (2, 3, 4):                     # 꼬리 (왼쪽)
		F.Px(1, Y, Dark)
	F.Px(0, 2, Dark)
	F.Px(0, 4, Dark)
	EX = int(CX + RX - 2)
	F.Px(EX, 3, (250, 250, 250))
	F.Px(EX + 1, 3, (20, 24, 32))
	F.Px(EX - 1, 3, Dark)                   # 아가미
	F.Px(EX - 1, 4, Dark)
	F.Px(int(CX) - 2, 3, Light)             # 비늘 반짝임
	F.Outline((32, 40, 54))
	if bFlip:
		F.P = F.P[:, ::-1].copy()
	return F


def _IceTray(W, H):
	C = FCanvas(W, H)
	C.Rect(0, 1, W - 1, H - 1, (170, 200, 216))
	C.Rect(1, 2, W - 2, H - 2, ICE)
	for X, Y in ((3, 4), (9, 3), (15, 5), (6, 10), (13, 11), (18, 9), (2, 12)):
		if X < W - 1 and Y < H - 1:
			C.Px(X, Y, (255, 255, 255))
	return C


def DrawFishPile(Kind):
	# 상자 위 생선 진열 (위에서 본 모양 — 바닥에 눕혀 놓는다): 얼음 바탕 + 엇갈린 세 줄로 누운 생선 (시장 진열처럼)
	C = _IceTray(22, 15)
	Pal = {"A": [FISH[0], FISH[1], FISH[0]], "B": [FISH[1], FISH[1], FISH[0]], "C": [FISH[2], FISH[0], FISH[2]]}[Kind]
	for DX, DY, PI, bFlip in ((0, 0, 0, False), (10, 0, 1, True), (-1, 4, 1, True), (9, 4, 2, False), (0, 8, 2, False), (10, 8, 0, True)):
		_Paste(C, _FishSprite(Pal[PI], bFlip), DX, DY)
	return C


def _CrabSprite(bFlip=False):
	# 위에서 본 삶은 게: 둥근 등딱지 + 집게 둘 + 다리 셋씩
	F = FCanvas(11, 8)
	F.Ellipse(5.5, 4.2, 3.3, 2.5, CRAB)
	F.Px(4, 3, CRAB_L)
	F.Px(5, 3, CRAB_L)
	for S in (-1, 1):
		X0 = 5 + S * 4
		F.Px(X0, 2, CRAB_D)
		F.Px(X0 + S, 1, CRAB)
		F.Px(X0, 1, CRAB)
		for K in range(3):
			F.Px(5 + S * 3, 4 + K, CRAB_D) if K else None
			F.Px(5 + S * 4, 5 + K * 0, CRAB_D)
	F.Outline((70, 24, 22))
	if bFlip:
		F.P = F.P[::-1].copy()
	return F


def DrawCrabPile():
	C = _IceTray(22, 15)
	for DX, DY, bFlip in ((1, 1, False), (10, 0, True), (5, 5, False), (12, 6, False), (0, 7, True)):
		_Paste(C, _CrabSprite(bFlip), DX, DY)
	return C


def DrawShellPile():
	C = FCanvas(16, 12)
	C.Ellipse(8, 6, 7.4, 5.2, ICE)
	Rng = np.random.default_rng(21)
	for K in range(10):
		X, Y = 3 + Rng.uniform(0, 10), 3 + Rng.uniform(0, 6)
		Col = [(236, 214, 190), (120, 110, 150), (240, 180, 160)][K % 3]
		C.Ellipse(X, Y, 1.6, 1.2, Col)
		C.Px(X - 0.6, Y - 0.6, (255, 250, 240))
	C.Outline((70, 60, 70))
	return C


def DrawFishHanging():
	# 처마 밑에 매단 생선 줄 (세운 판 — 정면): 줄 + 거꾸로 매단 생선 5마리
	C = FCanvas(40, 14)
	C.Line(0, 1, 39, 1, (110, 82, 50))
	for K in range(5):
		X = 4 + K * 8
		Pal = FISH[K % 3]
		C.Px(X, 2, (110, 82, 50))
		# 세로 생선 (머리 아래)
		for Y in range(3, 12):
			T = (Y - 3) / 8.0
			W = 1.6 * math.sin(math.pi * min(1.0, T * 1.05 + 0.1))
			for DX in range(-2, 3):
				if abs(DX) <= W:
					C.Px(X + DX, Y, Pal[1] if DX < 0 else (Pal[2] if DX > 0 else Pal[0]))
		C.Px(X - 1, 3, Pal[2])
		C.Px(X + 1, 3, Pal[2])
		C.Px(X, 10, (30, 30, 40))
	C.Outline((40, 40, 50))
	return C


def DrawShell(Index):
	C = FCanvas(6, 5)
	if Index == 0:
		Poly(C, [(0, 4), (3, 0), (6, 4)], (240, 214, 196))
		for X in (1, 3, 5):
			C.Line(3, 1, X, 4, (200, 160, 140))
	else:
		C.Ellipse(3, 2.5, 2.6, 2.0, (186, 170, 210))
		C.Px(2, 1, (240, 230, 250))
	C.Outline((90, 70, 70))
	return C


def DrawStarfish():
	C = FCanvas(8, 8)
	for K in range(5):
		A_ = math.radians(-90 + K * 72)
		C.Line(4, 4, 4 + math.cos(A_) * 3.4, 4 + math.sin(A_) * 3.4, (236, 120, 80))
	C.Rect(3, 3, 4, 4, (255, 170, 110))
	C.Outline((110, 50, 40))
	return C


# ---- 그림 표지판 (글자 없이 그림 + 화살표) -------------------------------------------------------------------------------
def _Board(W, H):
	C = FCanvas(W, H)
	C.Rect(0, 0, W - 1, H - 1, WOOD)
	C.Rect(0, 0, W - 1, 0, WOOD_L)
	C.Rect(0, H - 1, W - 1, H - 1, WOOD_D)
	for Y in (3, 6):
		if Y < H - 1:
			C.Rect(1, Y, W - 2, Y, (142, 82, 44))
	return C


def _Arrow(C, X, Y, Dir, Col=(250, 236, 200)):
	# Dir: "R" "L" "U"
	if Dir == "R":
		C.Rect(X, Y, X + 3, Y, Col)
		Poly(C, [(X + 3, Y - 2.5), (X + 6, Y + 0.5), (X + 3, Y + 3.5)], Col)
	elif Dir == "L":
		C.Rect(X + 3, Y, X + 6, Y, Col)
		Poly(C, [(X + 3.5, Y - 2.5), (X + 0.5, Y + 0.5), (X + 3.5, Y + 3.5)], Col)
	elif Dir == "D":
		C.Rect(X + 2, Y - 3, X + 2, Y, Col)
		Poly(C, [(X - 0.5, Y + 0.5), (X + 2.5, Y + 3.5), (X + 5.5, Y + 0.5)], Col)
	else:
		C.Rect(X + 2, Y, X + 2, Y + 3, Col)
		Poly(C, [(X - 0.5, Y + 0.5), (X + 2.5, Y - 2.5), (X + 5.5, Y + 0.5)], Col)


def DrawSign(Kind):
	C = _Board(20, 9)
	Ink = (64, 36, 22)
	if Kind == "Village":      # 집 + 위 화살표 (언덕 너머 하르트 마을)
		Poly(C, [(2, 4.5), (6, 1), (10, 4.5)], (198, 76, 60))
		C.Rect(3, 4, 9, 7, (250, 236, 200))
		C.Rect(5, 5, 6, 7, Ink)
		_Arrow(C, 13, 3, "U")
	elif Kind == "Market":     # 생선 + 오른쪽 화살표 (어시장·부두)
		_Fish(C, 6, 4.5, 8, 0, FISH[1])
		_Arrow(C, 12, 4, "R")
	elif Kind == "Danger":     # 해골 + 오른쪽 화살표 (마물 구역)
		C.Ellipse(5, 3.8, 3.2, 2.9, (240, 236, 226))
		C.Rect(4, 6, 6, 7, (240, 236, 226))
		C.Px(4, 3, Ink)
		C.Px(6, 3, Ink)
		C.Px(5, 5, Ink)
		_Arrow(C, 12, 4, "R", (255, 140, 110))
	elif Kind == "Harbor":     # 닻 + 아래 화살표 (남쪽 시냇가 길 → 갈매기 항구)
		W = (60, 110, 180)
		C.Rect(5, 2, 5, 7, W)
		C.Rect(3, 3, 7, 3, W)
		C.Px(2, 6, W)
		C.Px(3, 7, W)
		C.Px(4, 7, W)
		C.Px(8, 6, W)
		C.Px(7, 7, W)
		C.Px(6, 7, W)
		C.Px(5, 1, W)
		_Arrow(C, 12, 4, "D")
	elif Kind == "Lighthouse":  # 등대 + 왼쪽 화살표
		C.Rect(4, 2, 6, 7, (250, 240, 230))
		C.Rect(4, 4, 6, 4, (200, 60, 50))
		C.Rect(3, 1, 7, 1, (60, 60, 70))
		C.Px(5, 0, (255, 220, 120))
		_Arrow(C, 11, 4, "L")
	C.Outline((50, 30, 20))
	return C


def DrawAnchorEmblem():
	# 아치 문 판의 닻 문장 (둥근 판)
	C = FCanvas(14, 14)
	C.Ellipse(7, 7, 6.6, 6.6, SEA_BLUE)
	C.Ellipse(6, 6, 5.0, 5.0, (76, 140, 200))
	W = (240, 236, 226)
	C.Rect(6, 3, 7, 10, W)
	C.Rect(4, 4, 9, 4, W)
	C.Px(6, 2, W)
	C.Px(7, 2, W)
	for X, Y in ((3, 8), (4, 9), (5, 10), (10, 8), (9, 9), (8, 10)):
		C.Px(X, Y, W)
	C.Outline((30, 40, 60))
	return C


def DrawFlag(Kind, Frame):
	# 펄럭이는 깃발 (장대 오른쪽으로 — 피벗 = 왼쪽 위 장대 끝): 열마다 사인 물결
	W, H = 24, 15
	C = FCanvas(W + 1, H + 4)
	Base = (36, 34, 40) if Kind == "Pirate" else (54, 108, 178)
	Light = (64, 60, 70) if Kind == "Pirate" else (96, 150, 214)
	for X in range(W):
		Off = math.sin(X * 0.42 - Frame * math.pi * 0.5) * (X / W) * 2.2
		for Y in range(H):
			YY = int(round(Y + Off + 1))
			Shade = math.cos(X * 0.42 - Frame * math.pi * 0.5)
			C.Px(X, YY, Light if Shade > 0.45 else Base)
	# 문양 (물결 따라 옮김)
	def Mark(X, Y, Col):
		Off = math.sin(X * 0.42 - Frame * math.pi * 0.5) * (X / W) * 2.2
		C.Px(X, int(round(Y + Off + 1)), Col)
	if Kind == "Pirate":
		Bone = (236, 232, 220)
		for DX in range(-3, 4):
			for DY in range(-3, 3):
				if (DX / 3.4) ** 2 + (DY / 3.0) ** 2 <= 1.0:
					Mark(12 + DX, 6 + DY, Bone)
		Mark(11, 6, Base)
		Mark(13, 6, Base)
		Mark(12, 8, Base)
		for K in range(-5, 6):
			Mark(12 + K, 11 + (K // 3) * 0, Bone if abs(K) > 1 else Bone)
		for K in range(-4, 5):
			Mark(12 + K, 10 + abs(K) // 2 - 1 if K < 0 else 10 + K // 2 - 1, Bone)
	else:
		W_ = (240, 236, 226)
		for Y in range(3, 12):
			Mark(12, Y, W_)
		for X in range(9, 16):
			Mark(X, 4, W_)
		for K in range(4):
			Mark(9 + K // 2, 10 + K // 2 - 1, W_)
			Mark(15 - K // 2, 10 + K // 2 - 1, W_)
		Mark(12, 2, W_)
	C.Outline((24, 22, 30))
	return C


def DrawLensCrate():
	# 해적 야영지의 열린 나무 상자 (등대 렌즈가 들어 있던 — 정면, 피벗 발)
	C = FCanvas(20, 16)
	C.Rect(1, 5, 18, 15, WOOD)
	C.Rect(1, 5, 18, 6, WOOD_L)
	C.Rect(1, 10, 18, 10, WOOD_D)
	C.Rect(1, 15, 18, 15, WOOD_D)
	C.Rect(2, 5, 2, 15, WOOD_D)
	C.Rect(17, 5, 17, 15, WOOD_D)
	Poly(C, [(0, 5), (4, 0), (19, 0), (19, 2), (5, 2), (2, 5)], (176, 110, 60))  # 비스듬히 열린 뚜껑
	C.Rect(4, 3, 16, 4, (60, 44, 30))
	C.Px(9, 3, (200, 200, 170))
	C.Outline((50, 30, 20))
	return C


def DrawLens(Frame):
	# 등대 렌즈 (퀘스트 물건): 놋쇠 테 + 푸른빛 유리 + 반짝임 (2프레임)
	C = FCanvas(14, 14)
	C.Ellipse(7, 7, 6.4, 6.4, (196, 150, 70))
	C.Ellipse(7, 7, 5.0, 5.0, (150, 210, 240))
	C.Ellipse(6, 6, 3.4, 3.4, (196, 236, 255))
	for R in (2.0, 3.6):
		for K in range(12):
			A_ = K / 12 * math.pi * 2
			C.Px(7 + math.cos(A_) * R, 7 + math.sin(A_) * R, (120, 186, 220))
	C.Px(5, 4, (255, 255, 255))
	C.Px(4, 5, (255, 255, 255))
	if Frame == 1:
		C.Px(10, 3, (255, 255, 230))
		C.Px(11, 2, (255, 255, 230))
		C.Px(9, 2, (255, 255, 230))
	C.Outline((70, 50, 20))
	return C


# ---- 갈매기 (장식 — 서 있기 / 날기) ---------------------------------------------------------------------------------------------
GULL_W, GULL_G, GULL_D = (244, 244, 240), (176, 184, 196), (120, 128, 142)
BEAK = (250, 196, 60)


def DrawGullStand(Frame):
	# 옆모습 (오른쪽), 0/1 고개 까딱
	C = FCanvas(14, 12)
	C.Ellipse(6, 7, 4.4, 2.8, GULL_W)
	Poly(C, [(2, 6), (8, 5), (9, 7), (1, 8)], GULL_G)
	C.Rect(1, 7, 2, 7, (40, 40, 46))
	HX = 10 if Frame == 0 else 10
	HY = 4 if Frame == 0 else 5
	C.Ellipse(HX, HY, 2.0, 1.8, GULL_W)
	C.Rect(HX + 2, HY, HX + 3, HY, BEAK)
	C.Px(HX + 1, HY - 1, (30, 30, 36))
	C.Rect(5, 10, 5, 11, BEAK)
	C.Rect(7, 10, 7, 11, BEAK)
	C.Outline((60, 64, 74))
	return C


def DrawGullFly(Frame):
	# 날아가는 갈매기 (옆에서 본 M자 날개 — 4프레임 날갯짓)
	C = FCanvas(18, 10)
	Lift = (-3, -1, 2, -1)[Frame]
	C.Ellipse(9, 6, 3.6, 1.6, GULL_W)
	C.Rect(13, 5, 14, 6, GULL_W)
	C.Px(15, 6, BEAK)
	for S in (-1, 1):
		X0 = 9 + S * 1
		C.Line(X0, 5, X0 + S * 4, 5 + Lift, GULL_G)
		C.Line(X0 + S * 4, 5 + Lift, X0 + S * 8, 6 + Lift * 0.6, GULL_D)
	C.Outline((60, 64, 74))
	return C


# ---- 그물 텍스처 (마스크 — 격자 실 + 매듭) --------------------------------------------------------------------------------------
def WriteNetTexture(Path):
	Size = 128
	Img = np.zeros((Size, Size, 4), dtype=np.uint8)
	Cell = 16
	for Y in range(Size):
		for X in range(Size):
			U, V = (X + Y) % Cell, (X - Y) % Cell
			if U < 4 or V < 4:
				Shade = 200 + ((X * 7 + Y * 3) % 30)
				Img[Y, X] = (Shade, Shade - 12, Shade - 40, 255)
			if (X + Y) % Cell < 5 and (X - Y) % Cell < 5:
				Img[Y, X] = (150, 130, 90, 255)
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	Image.fromarray(Img, "RGBA").save(Path, optimize=True)


# ---- 항구 주민 (정면 대기 4프레임 — 0 기본 · 1 숨 들이쉼(몸 1도트 위) · 2 손 물건 움직임 · 3 눈 깜빡임) -------------------------------------
SKIN, SKIN_D, SKIN_L = A.SKIN, A.SKIN_D, (255, 228, 196)
BLUSH, EYE = A.BLUSH, A.EYE
BRASS, BRASS_D = (232, 190, 88), (168, 120, 48)


def _Shade3(Pal):
	return Pal if len(Pal) == 3 else (Pal, Pal, Pal)


class FFolk:
	# 32x32 정면 인물 붓: 위에서 아래로 머리(Top)·몸통·다리. 빛은 왼쪽 위 — 각 부위 왼쪽 열 밝게, 오른쪽 열 어둡게
	def __init__(self, Frame, Lift=0):
		self.C = FCanvas(32, 32)
		self.F = Frame
		self.B = 1 if Frame == 1 else 0        # 숨쉬기: 상체를 1도트 올린다 (다리는 그대로)
		self.Top = 3 + Lift - self.B

	def Legs(self, Pants, Shoes, Skirt=None, Gap=True):
		C = self.C
		PL, PM, PD = _Shade3(Pants)
		if Skirt:
			SL, SM, SD = _Shade3(Skirt)
			Poly(C, [(11, 23), (21, 23), (23, 30), (9, 30)], SM)
			C.Rect(10, 23, 12, 29, SL)
			C.Rect(20, 25, 22, 29, SD)
			C.Rect(9, 29, 22, 29, SD)
			C.Rect(11, 30, 13, 31, Shoes)
			C.Rect(18, 30, 20, 31, Shoes)
			return
		C.Rect(12, 24, 15, 29, PM)
		C.Rect(16, 24, 19, 29, PD)
		C.Rect(12, 24, 12, 29, PL)
		if Gap:
			C.Rect(15, 26, 16, 29, (0, 0, 0), 0)
		C.Rect(11, 30, 15, 31, Shoes)
		C.Rect(16, 30, 20, 31, Shoes)
		C.Px(11, 30, tuple(min(255, V + 40) for V in Shoes))

	def Torso(self, Coat, Under=None, Belt=None, Wide=0):
		C, T = self.C, self.Top
		CL, CM, CD = _Shade3(Coat)
		Y0, Y1 = T + 13, 25
		C.Rect(11 - Wide, Y0, 20 + Wide, Y1, CM)
		C.Rect(11 - Wide, Y0, 12 - Wide, Y1, CL)
		C.Rect(19 + Wide, Y0, 20 + Wide, Y1, CD)
		C.Rect(11 - Wide, Y1, 20 + Wide, Y1, CD)
		if Under:
			UL, UM, UD = _Shade3(Under)
			C.Rect(14, Y0, 17, Y1 - 1, UM)
			C.Rect(14, Y0, 14, Y1 - 1, UL)
			C.Rect(17, Y0 + 1, 17, Y1 - 1, UD)
		if Belt:
			C.Rect(11 - Wide, 22, 20 + Wide, 22, Belt)
		return Y0

	def Arms(self, Sleeve, Hands=True, Wide=0, LeftUp=0, RightUp=0):
		C, T = self.C, self.Top
		SL, SM, SD = _Shade3(Sleeve)
		Y0 = T + 13
		C.Rect(9 - Wide, Y0, 10 - Wide, 22 - LeftUp, SM)
		C.Rect(9 - Wide, Y0, 9 - Wide, 22 - LeftUp, SL)
		C.Rect(21 + Wide, Y0, 22 + Wide, 22 - RightUp, SD)
		if Hands:
			C.Rect(9 - Wide, 23 - LeftUp, 10 - Wide, 24 - LeftUp, SKIN)
			C.Rect(21 + Wide, 23 - RightUp, 22 + Wide, 24 - RightUp, SKIN_D)

	def Head(self, Hair, Style="Short", Blink=None, Beard=None, Mouth=True, Old=False):
		C, T = self.C, self.Top
		HL, HM, HD = _Shade3(Hair)
		Blink = (self.F == 3) if Blink is None else Blink
		# 얼굴 (모서리 깎은 사각) + 귀 + 목
		C.Rect(14, T + 12, 17, T + 13, SKIN_D)
		C.Rect(11, T + 4, 20, T + 11, SKIN)
		C.Rect(12, T + 3, 19, T + 3, SKIN)
		C.Rect(12, T + 12, 19, T + 12, SKIN_D)
		C.Rect(11, T + 11, 11, T + 11, (0, 0, 0), 0)
		C.Rect(20, T + 11, 20, T + 11, (0, 0, 0), 0)
		C.Px(10, T + 7, SKIN)
		C.Px(10, T + 8, SKIN_D)
		C.Px(21, T + 7, SKIN_D)
		C.Px(21, T + 8, SKIN_D)
		C.Rect(19, T + 5, 20, T + 11, SKIN_D)
		C.Px(12, T + 5, SKIN_L)
		# 머리카락
		if Style == "Short":
			C.Rect(11, T + 0, 20, T + 3, HM)
			C.Rect(10, T + 1, 21, T + 5, HM)
			C.Rect(12, T + 0, 16, T + 0, HL)
			C.Rect(11, T + 1, 13, T + 2, HL)
			C.Rect(19, T + 2, 21, T + 6, HD)
			C.Rect(13, T + 4, 15, T + 4, HM)  # 앞머리 끝
		elif Style == "Bun":
			C.Ellipse(16, T - 1, 3.0, 2.4, HM)
			C.Px(15, T - 2, HL)
			C.Rect(11, T + 0, 20, T + 3, HM)
			C.Rect(10, T + 1, 21, T + 6, HM)
			C.Rect(11, T + 1, 13, T + 2, HL)
			C.Rect(19, T + 2, 21, T + 8, HD)
			C.Rect(10, T + 6, 10, T + 9, HM)
		elif Style == "Braid":
			C.Rect(11, T + 0, 20, T + 3, HM)
			C.Rect(10, T + 1, 21, T + 5, HM)
			C.Rect(11, T + 1, 14, T + 1, HL)
			C.Rect(19, T + 2, 21, T + 6, HD)
			for K in range(5):
				C.Px(21 + (K % 2), T + 7 + K * 2, HM)
				C.Px(22 - (K % 2), T + 8 + K * 2, HD)
		elif Style == "Bald":
			C.Rect(11, T + 1, 20, T + 4, SKIN)
			C.Rect(12, T + 0, 19, T + 0, SKIN)
			C.Rect(13, T + 1, 15, T + 1, SKIN_L)
			C.Rect(10, T + 3, 10, T + 7, HD)
			C.Rect(21, T + 3, 21, T + 7, HD)
		# 눈 (깜빡임 = 가로 한 줄) + 볼 + 입
		EY = T + 7
		for EX in (13, 18):
			if Blink:
				C.Rect(EX, EY + 1, EX, EY + 1, EYE)
			else:
				C.Rect(EX, EY, EX, EY + 1, EYE)
				C.Px(EX, EY, (90, 80, 110))
		if Old:
			C.Rect(12, EY - 1, 14, EY - 1, HL)
			C.Rect(17, EY - 1, 19, EY - 1, HL)
		else:
			C.Px(12, EY + 3, BLUSH)
			C.Px(19, EY + 3, BLUSH)
		if Beard:
			BL, BM, BD = _Shade3(Beard)
			Poly(C, [(11, T + 9), (20, T + 9), (19, T + 15), (16, T + 17), (12, T + 15)], BM)
			C.Rect(12, T + 9, 13, T + 13, BL)
			C.Rect(18, T + 10, 19, T + 14, BD)
			C.Rect(14, T + 10, 17, T + 10, BD)  # 입 자리
		elif Mouth:
			C.Rect(15, T + 10, 16, T + 10, SKIN_D)

	def Done(self, Out=(36, 24, 40)):
		self.C.Outline(Out)
		return self.C


def DrawKeeper(F):
	# 등대지기 노아: 흰 수염 노인, 남색 피코트(놋쇠 단추), 붉은 털모자, 오른손에 등불(흔들림 — 프레임 2)
	K = FFolk(F)
	C, T = K.C, K.Top
	K.Legs(((96, 90, 110), (76, 70, 90), (56, 52, 70)), (60, 44, 34))
	K.Torso(((70, 92, 150), (48, 66, 118), (32, 44, 84)), Wide=1)
	for Y in (T + 15, T + 18, 22):
		C.Px(15, Y, BRASS)
		C.Px(17, Y, BRASS_D)
	C.Rect(10, T + 13, 21, T + 14, (40, 56, 100))  # 깃
	K.Arms(((70, 92, 150), (48, 66, 118), (32, 44, 84)), Wide=1, RightUp=1)
	K.Head(((240, 240, 236), (208, 208, 212), (150, 150, 160)), "Bald", Beard=((246, 246, 240), (214, 214, 220), (160, 160, 170)), Old=True)
	# 털모자 (반구 + 접은 단 + 방울)
	C.Rect(11, T - 1, 20, T + 3, (178, 52, 48))
	C.Rect(12, T - 2, 19, T - 2, (178, 52, 48))
	C.Rect(10, T + 3, 21, T + 4, (130, 34, 36))
	C.Rect(11, T - 1, 13, T + 1, (214, 90, 80))
	C.Ellipse(16, T - 3, 1.6, 1.4, (236, 230, 220))
	# 등불 (손에서 매달림 — 프레임 2에서 1도트 흔들림)
	SX = 25 + (1 if F == 2 else 0)
	C.Line(23, 22, SX, 23, (60, 50, 40))
	C.Rect(SX - 2, 24, SX + 2, 24, (40, 36, 34))
	C.Rect(SX - 2, 25, SX + 2, 29, (255, 214, 120))
	C.Rect(SX - 1, 26, SX + 1, 28, (255, 246, 196))
	C.Rect(SX - 2, 25, SX - 2, 29, (40, 36, 34))
	C.Rect(SX + 2, 25, SX + 2, 29, (40, 36, 34))
	C.Rect(SX - 2, 30, SX + 2, 30, (40, 36, 34))
	return K.Done()


def DrawHarborMaster(F):
	# 항만장 마르타: 적갈색 올림머리, 흰 항만장 모자(검은 챙·금 휘장), 흰-하늘 제복(금 견장·단추), 왼손에 망원경
	K = FFolk(F)
	C, T = K.C, K.Top
	K.Legs(((60, 70, 110), (44, 52, 88), (30, 36, 64)), (40, 34, 40))
	Coat = ((244, 246, 250), (210, 222, 238), (150, 168, 196))
	K.Torso(Coat)
	C.Rect(15, T + 13, 16, 24, (40, 60, 120))  # 앞여밈 남색 띠
	for Y in (T + 15, T + 18, 22):
		C.Px(14, Y, BRASS)
		C.Px(17, Y, BRASS)
	C.Rect(9, T + 13, 11, T + 13, BRASS)
	C.Rect(20, T + 13, 22, T + 13, BRASS_D)
	K.Arms(Coat, LeftUp=2 if F == 2 else 1)
	# 망원경 (왼손, 프레임 2에 살짝 듦)
	LY = (21 if F == 2 else 22)
	C.Line(5, LY - 3, 10, LY, BRASS)
	C.Line(5, LY - 2, 10, LY + 1, BRASS_D)
	C.Px(4, LY - 4, (60, 50, 40))
	K.Head(((196, 104, 70), (156, 74, 50), (106, 48, 36)), "Bun")
	# 모자 (흰 윗판 + 검은 챙 + 금 휘장)
	C.Rect(10, T - 1, 21, T + 2, (248, 248, 250))
	C.Rect(10, T - 1, 21, T - 1, (220, 226, 236))
	C.Rect(9, T + 3, 22, T + 3, (24, 24, 30))
	C.Rect(15, T, 16, T + 1, BRASS)
	return K.Done()


def DrawFisher(F):
	# 어부 토마: 노란 방수모(뒤챙)·노란 비옷, 초록 장화, 어깨에 멘 낚싯대(낚싯줄·찌가 프레임마다 흔들림)
	K = FFolk(F)
	C, T = K.C, K.Top
	Oil = ((255, 226, 96), (238, 190, 40), (186, 134, 24))
	K.Legs(((90, 80, 70), (70, 60, 52), (50, 42, 36)), (54, 92, 60))
	C.Rect(11, 28, 20, 29, (54, 92, 60))  # 장화 목
	K.Torso(Oil, Wide=1)
	for Y in (T + 15, T + 18, 21):
		C.Px(16, Y, (120, 90, 30))
	K.Arms(Oil, Wide=1, RightUp=3)
	K.Head(((120, 84, 56), (90, 60, 40), (62, 40, 28)), "Short")
	# 방수모 (둥근 윗부분 + 넓은 챙, 뒤로 처짐)
	C.Rect(11, T - 1, 20, T + 2, Oil[1])
	C.Rect(12, T - 2, 19, T - 2, Oil[1])
	C.Rect(12, T - 1, 14, T, Oil[0])
	C.Rect(8, T + 3, 23, T + 3, Oil[1])
	C.Rect(8, T + 3, 10, T + 3, Oil[0])
	C.Rect(21, T + 3, 23, T + 4, Oil[2])
	# 낚싯대 (오른손 → 오른쪽 위로 길게) + 줄 + 찌
	C.Line(22, 20, 30, 3, (120, 84, 50))
	C.Px(30, 2, (80, 56, 34))
	Bob = (0, 1, 2, 1)[F]
	C.Line(30, 3, 30, 12 + Bob, (220, 220, 220))
	C.Rect(29, 13 + Bob, 31, 14 + Bob, (230, 60, 50))
	C.Px(30, 15 + Bob, (250, 250, 250))
	return K.Done()


def DrawFishmonger(F):
	# 생선 장수 베라: 물방울 무늬 붉은 두건, 걷어 올린 소매, 흰 앞치마, 왼손에 든 생선(프레임 2에 꼬리 퍼덕)
	K = FFolk(F)
	C, T = K.C, K.Top
	Dress = ((120, 150, 200), (86, 116, 170), (58, 82, 128))
	K.Legs(Dress, (80, 52, 36), Skirt=Dress)
	K.Torso(Dress)
	C.Rect(12, T + 15, 19, 27, (246, 242, 230))  # 앞치마
	C.Rect(12, T + 15, 13, 27, (255, 252, 244))
	C.Rect(18, T + 16, 19, 27, (214, 206, 190))
	C.Px(14, 21, (190, 150, 140))
	C.Px(17, 24, (190, 160, 150))
	K.Arms((SKIN_L, SKIN, SKIN_D), LeftUp=4)
	C.Rect(9, T + 13, 10, T + 15, Dress[1])
	C.Rect(21, T + 13, 22, T + 15, Dress[2])
	K.Head(((150, 90, 60), (116, 66, 42), (80, 44, 30)), "Short")
	# 두건 (붉은 바탕 + 흰 점 + 뒤 매듭)
	C.Rect(10, T - 1, 21, T + 3, (210, 60, 58))
	C.Rect(11, T - 2, 20, T - 2, (210, 60, 58))
	C.Rect(10, T + 3, 21, T + 3, (160, 40, 44))
	for X, Y in ((12, 0), (15, 1), (18, 0), (13, 2), (20, 2)):
		C.Px(X, T + Y, (250, 236, 226))
	C.Rect(21, T + 4, 22, T + 5, (160, 40, 44))
	# 생선 (왼손에 거꾸로 — 머리 아래)
	Flap = 1 if F == 2 else 0
	FishPal = FISH[1]
	for Y in range(13, 21):
		W = 1 if Y in (13, 20) else 2
		for DX in range(-W, W):
			C.Px(7 + DX, Y + Flap * (1 if Y < 15 else 0) - 3, FishPal[1] if DX < 0 else FishPal[0])
	C.Rect(5 - Flap, 9 - Flap, 9 + Flap, 9 - Flap, FishPal[2])
	C.Px(7, 16, (20, 24, 32))
	return K.Done()


def DrawInnkeeper(F):
	# 여관 주인 한나: 땋은 갈색 머리, 초록 원피스, 흰 앞치마, 쟁반에 맥주잔(프레임 2에 거품 출렁)
	K = FFolk(F)
	C, T = K.C, K.Top
	Dress = ((110, 170, 110), (76, 136, 82), (50, 98, 60))
	K.Legs(Dress, (90, 56, 36), Skirt=Dress)
	K.Torso(Dress, Wide=1)
	C.Rect(12, T + 16, 19, 27, (248, 244, 232))
	C.Rect(18, T + 16, 19, 27, (216, 208, 192))
	C.Rect(12, T + 13, 19, T + 13, (248, 244, 232))
	K.Arms(Dress, Wide=1, LeftUp=3, RightUp=3)
	# 쟁반 + 잔
	C.Rect(6, 18, 14, 18, (150, 110, 70))
	C.Rect(7, 13, 10, 17, (250, 220, 120))
	C.Rect(7, 13, 10, 13, (255, 250, 240))
	C.Rect(7, 12 - (1 if F == 2 else 0), 9, 12, (255, 250, 240))
	C.Rect(11, 14, 11, 16, (220, 200, 160))
	K.Head(((176, 112, 64), (140, 84, 48), (96, 56, 34)), "Braid")
	return K.Done()


def DrawSailor(F):
	# 선원 잭: 흰 수병모, 파랑·흰 줄무늬 셔츠, 붉은 목수건, 팔짱(프레임 2에 고개 까딱 — 모자 기울기)
	K = FFolk(F)
	C, T = K.C, K.Top
	K.Legs(((228, 214, 186), (200, 184, 150), (160, 144, 110)), (50, 40, 40))
	K.Torso(((244, 246, 250), (224, 230, 240), (176, 186, 204)))
	for Y in range(T + 14, 25, 2):
		C.Rect(11, Y, 20, Y, (60, 90, 160))
	C.Rect(13, T + 13, 18, T + 14, (210, 52, 48))
	C.Px(15, T + 15, (210, 52, 48))
	C.Px(16, T + 16, (160, 36, 36))
	# 팔짱 (가슴 앞을 가로지르는 팔)
	C.Rect(9, T + 13, 10, T + 17, (224, 230, 240))
	C.Rect(21, T + 13, 22, T + 17, (176, 186, 204))
	C.Rect(10, T + 17, 21, T + 19, (230, 236, 246))
	C.Rect(10, T + 19, 21, T + 19, (60, 90, 160))
	C.Rect(10, T + 17, 11, T + 18, SKIN)
	C.Rect(20, T + 17, 21, T + 18, SKIN_D)
	K.Head(((60, 46, 40), (40, 30, 28), (26, 20, 20)), "Short")
	Tilt = 1 if F == 2 else 0
	C.Rect(11, T - 1 + Tilt, 20, T + 1, (250, 250, 252))
	C.Rect(10, T + 2, 21, T + 2, (200, 210, 226))
	C.Rect(12, T - 2 + Tilt, 19, T - 2 + Tilt, (250, 250, 252))
	return K.Done()


def DrawGhostSailor(F):
	# 유령 선원 (밤에만): 창백한 청록 몸, 낡은 선원 모자, 빛나는 눈, 아래가 연기처럼 흩어짐 (프레임마다 둥실)
	K = FFolk(F, Lift=(0, -1, -2, -1)[F])
	C, T = K.C, K.Top
	Body = ((190, 246, 238), (130, 210, 210), (80, 150, 160))
	Y0 = K.Torso(Body, Wide=1)
	C.Rect(13, Y0 + 2, 18, Y0 + 2, (90, 160, 170))
	K.Arms(Body, Hands=False, Wide=1)
	K.Head(((150, 220, 220), (110, 186, 190), (70, 130, 140)), "Short", Blink=False, Mouth=False)
	for EX in (13, 18):
		C.Rect(EX, T + 7, EX, T + 8, (40, 70, 80))
		C.Px(EX, T + 7, (255, 255, 210))
	C.Px(15, T + 10, (60, 110, 120))
	C.Px(16, T + 10, (60, 110, 120))
	# 낡은 선원 모자 (찢긴 챙 끝)
	C.Rect(11, T - 1, 20, T + 2, (120, 168, 176))
	C.Rect(12, T - 2, 19, T - 2, (120, 168, 176))
	C.Rect(10, T + 2, 21, T + 2, (84, 128, 140))
	C.Px(21, T + 3, (84, 128, 140))
	C.Px(10, T + 3, (84, 128, 140))
	# 아래: 다리 대신 흩어지는 꼬리 (체크무늬로 듬성듬성)
	for Y in range(26, 32):
		for X in range(10 - (Y - 26) // 2, 22 + (Y - 26) // 3):
			if (X + Y + F) % (2 if Y < 29 else 3) == 0:
				C.Px(X, Y, Body[1] if Y < 29 else Body[2])
	for X in range(10, 22):
		C.Px(X, 25, Body[2])
	return K.Done((40, 80, 96))


def DrawVillageInnkeeper(F):
	# 하르트 여관 주인 로렌: 콧수염, 갈색 조끼 + 흰 셔츠, 앞치마, 맥주잔 닦기(프레임 2에 천이 움직임)
	K = FFolk(F)
	C, T = K.C, K.Top
	K.Legs(((96, 80, 70), (74, 60, 52), (54, 42, 36)), (60, 40, 30))
	K.Torso(((240, 236, 226), (222, 214, 200), (176, 166, 150)), Wide=1)
	C.Rect(10, T + 13, 13, 22, (150, 96, 56))
	C.Rect(18, T + 13, 21, 22, (110, 68, 40))
	C.Rect(12, 22, 19, 27, (232, 226, 214))
	K.Arms(((240, 236, 226), (222, 214, 200), (176, 166, 150)), Wide=1, LeftUp=4, RightUp=3)
	C.Rect(5, 15, 8, 19, (250, 220, 120))
	C.Rect(5, 15, 8, 15, (255, 250, 240))
	C.Rect(9, 16, 9, 18, (220, 200, 160))
	CX = 22 + (1 if F == 2 else 0)
	C.Rect(CX, 17, CX + 3, 19, (246, 246, 240))
	K.Head(((150, 100, 66), (116, 74, 48), (80, 50, 34)), "Short", Mouth=False)
	C.Rect(13, T + 9, 18, T + 9, (116, 74, 48))
	C.Px(12, T + 10, (116, 74, 48))
	C.Px(19, T + 10, (80, 50, 34))
	return K.Done()


FOLK = {"Keeper": DrawKeeper, "HarborMaster": DrawHarborMaster, "Fisher": DrawFisher, "Fishmonger": DrawFishmonger, "Innkeeper": DrawInnkeeper,
		"Sailor": DrawSailor, "GhostSailor": DrawGhostSailor, "VillageInnkeeper": DrawVillageInnkeeper}
FOLK_TIMES = [0.7, 0.45, 0.55, 0.12]
PORTRAIT_BOX = (6, 0, 26, 20)


# ---- 항구 적 (HD2DEnemy.lua 규약: <종류>_Idle/Move/Windup/Attack 플립북) ---------------------------------------------------------
def DrawCrab(Pose, Frame):
	# 바위 게 (정면 — 좌우 대칭이라 뒤집어도 같다): 붉은 등딱지 + 눈자루 + 큰 집게 둘 + 다리 셋씩. 피벗 = 발
	C = FCanvas(28, 20)
	Bob = {"Idle": (0, 1)[Frame % 2], "Move": (0, 1, 0, 1)[Frame % 4]}.get(Pose, 0)
	Low = 2 if Pose == "Windup" else (1 if Pose == "Attack" else 0)
	CY = 11 + Bob + Low
	# 다리 (몸 뒤에 먼저)
	for S in (-1, 1):
		for K in range(3):
			Phase = (K + Frame + (0 if S > 0 else 1)) % 2 if Pose == "Move" else 0
			X0 = 14 + S * (3 + K * 2)
			X1 = 14 + S * (7 + K * 2)
			Y1 = 18 + (1 if Phase else 0) - (1 if K == 2 else 0)
			C.Line(X0, CY + 2, X1, CY + 4, CRAB_D)
			C.Line(X1, CY + 4, X1 + S, Y1, CRAB_D)
	# 등딱지
	C.Ellipse(14, CY, 7.6, 4.8, CRAB)
	C.Ellipse(13, CY - 1.5, 5.2, 2.6, CRAB_L)
	C.Rect(9, CY + 3, 19, CY + 4, CRAB_D)
	for X in (11, 14, 17):
		C.Px(X, CY - 3, (255, 196, 160))
	C.Px(10, CY + 1, CRAB_D)
	C.Px(18, CY + 1, CRAB_D)
	# 눈자루 + 눈
	for EX in (11, 17):
		C.Rect(EX, CY - 7, EX, CY - 4, CRAB_D)
		C.Rect(EX - 1, CY - 9, EX + 1, CY - 7, (250, 250, 250))
		C.Px(EX + (1 if EX > 14 else 0), CY - 8, (20, 20, 30))
	# 집게: (팔 끝 X, Y, 벌림)
	if Pose == "Windup":
		Arms = [(4, CY - 8, 3), (24, CY - 8, 3)]
	elif Pose == "Attack":
		Arms = [(8 + Frame, CY - 2, 0), (20 - Frame, CY - 2, 0)]
	else:
		Up = (1 if (Pose == "Idle" and Frame % 2) else 0)
		Arms = [(4, CY - 2 - Up, 1), (24, CY - 2 - Up, 1)]
	for (AX, AY, Open), S in zip(Arms, (-1, 1)):
		C.Line(14 + S * 6, CY, AX, AY + 2, CRAB_D)
		C.Ellipse(AX + 0.5, AY + 1.5, 3.4, 2.8, CRAB)
		C.Px(AX - S, AY, CRAB_L)
		C.Px(AX - S, AY + 1, CRAB_L)
		# 집게 날 둘 (위·아래 — 벌림만큼 벌어진다)
		for W_ in (0, 1):
			C.Line(AX + S * W_, AY - 1, AX + S * (3 + W_), AY - 3 - Open, CRAB if W_ == 0 else CRAB_L)
		C.Line(AX + S, AY + 3, AX + S * 4, AY + 2 + Open, CRAB_D)
	C.Outline((60, 20, 22))
	return C


PIRATE_SKIN, PIRATE_SKIN_D = (226, 170, 128), (182, 124, 92)
CUTLASS, CUTLASS_D = (220, 228, 238), (150, 160, 180)


def _Cutlass(C, X0, Y0, X1, Y1, Curve=1.0, Width=1):
	# 휜 해적 칼 (손잡이 X0,Y0 → 칼끝 X1,Y1): 등(밝음)·날(그늘) 두 줄을 이은 선으로 + 금빛 코등이·손잡이
	N = max(int(max(abs(X1 - X0), abs(Y1 - Y0))), 1)
	DX, DY = (X1 - X0) / N, (Y1 - Y0) / N
	Pts = []
	for K in range(N + 1):
		T = K / N
		Bend = math.sin(T * math.pi * 0.85) * Curve * 1.6
		Pts.append((X0 + (X1 - X0) * T - DY * Bend, Y0 + (Y1 - Y0) * T + DX * Bend))
	for (AX, AY), (BX, BY) in zip(Pts[:-1], Pts[1:]):
		C.Line(round(AX - DY), round(AY + DX), round(BX - DY), round(BY + DX), CUTLASS_D)
		if Width > 1:
			C.Line(round(AX - DY * 2), round(AY + DX * 2), round(BX - DY * 2), round(BY + DX * 2), CUTLASS_D)
	for (AX, AY), (BX, BY) in zip(Pts[:-1], Pts[1:]):
		C.Line(round(AX), round(AY), round(BX), round(BY), CUTLASS)
	C.Px(X0 - DX, Y0 - DY, (90, 60, 40))
	C.Px(X0, Y0, BRASS)
	C.Px(X0 + DY, Y0 - DX, BRASS_D)
	C.Px(X0 - DY, Y0 + DX, BRASS_D)


def DrawPirate(Pose, Frame):
	# 해적 졸개 (옆모습, 오른쪽을 봄): 붉은 물방울 두건(뒤 매듭), 안대, 줄무늬 셔츠 + 가죽 조끼, 허리띠, 휜 칼
	C = FCanvas(32, 32)
	B = (Frame % 2) if Pose == "Idle" else ((0, 1, 0, 1)[Frame % 4] if Pose == "Move" else 0)
	Step = (2, 0, -2, 0)[Frame % 4] if Pose == "Move" else (3 if Pose == "Attack" else 0)
	Lean = {"Windup": -1, "Attack": 2}.get(Pose, 0)
	Pants, PantsD, Boot = (72, 62, 84), (52, 44, 62), (40, 30, 26)
	# 다리 (뒷다리 어둡게)
	C.Rect(12 - Step, 22, 14 - Step, 28, PantsD)
	C.Rect(11 - Step, 29, 14 - Step, 30, Boot)
	C.Rect(16 + Step, 22, 18 + Step, 28, Pants)
	C.Rect(16 + Step, 29, 19 + Step, 30, Boot)
	C.Px(16 + Step, 29, (70, 54, 44))
	# 몸통: 줄무늬 셔츠 + 조끼 + 허리띠
	T = 13 + B
	X0 = 11 + Lean
	C.Rect(X0, T, X0 + 8, 22, (236, 236, 240))
	for Y in range(T + 1, 22, 2):
		C.Rect(X0, Y, X0 + 8, Y, (200, 60, 60))
	C.Rect(X0, T, X0 + 2, 21, (110, 72, 44))
	C.Rect(X0 + 7, T, X0 + 8, 21, (90, 58, 36))
	C.Rect(X0, 21, X0 + 8, 22, (60, 40, 30))
	C.Px(X0 + 6, 21, BRASS)
	# 머리 (옆얼굴: 코·턱수염·안대·귀걸이) + 두건
	H = 3 + B
	HX = 12 + Lean
	C.Ellipse(HX + 4.5, H + 5.5, 4.6, 4.8, PIRATE_SKIN)
	C.Rect(HX + 2, H + 7, HX + 7, H + 10, PIRATE_SKIN)
	C.Rect(HX + 8, H + 6, HX + 9, H + 7, PIRATE_SKIN)          # 코
	C.Px(HX + 9, H + 8, PIRATE_SKIN_D)
	C.Rect(HX + 3, H + 9, HX + 7, H + 10, (70, 50, 40))       # 수염
	C.Px(HX + 2, H + 8, (70, 50, 40))
	C.Rect(HX + 5, H + 4, HX + 7, H + 5, (24, 20, 24))        # 안대
	C.Line(HX + 1, H + 3, HX + 6, H + 4, (24, 20, 24))
	C.Px(HX + 2, H + 7, BRASS)                                # 귀걸이
	C.Rect(HX + 1, H + 0, HX + 8, H + 2, (200, 50, 50))
	C.Rect(HX + 0, H + 2, HX + 8, H + 2, (150, 34, 36))
	C.Px(HX + 3, H + 1, (250, 236, 226))
	C.Px(HX + 6, H + 0, (250, 236, 226))
	C.Rect(HX - 2, H + 2, HX - 1, H + 3, (200, 50, 50))       # 매듭 꼬리
	C.Px(HX - 3, H + 4, (150, 34, 36))
	# 팔 + 칼
	if Pose == "Windup":
		C.Rect(HX - 1, T, HX + 0, T + 3, PIRATE_SKIN)
		_Cutlass(C, HX - 1, T - 1, HX - 7, T - 9, -1.0)
	elif Pose == "Attack":
		C.Rect(HX + 5, T + 3, HX + 9, T + 4, PIRATE_SKIN)
		_Cutlass(C, HX + 10, T + 3, HX + 18 + Frame, T + 2 + Frame, 1.0)
		if Frame == 0:
			for K in range(6):  # 휘두른 자국
				C.Px(HX + 12 + K, T - 3 + K // 2, (255, 255, 255))
	else:
		C.Rect(HX + 4, T + 1, HX + 5, T + 5 + B, PIRATE_SKIN)
		_Cutlass(C, HX + 6, T + 6 + B, HX + 13, T + 1 + B, 1.0)
	C.Outline((32, 22, 26))
	return C


SG_W, SG_G, SG_D = (246, 246, 242), (170, 180, 194), (110, 118, 134)


def DrawSeagull(Pose, Frame):
	# 사나운 갈매기 (정면 비스듬 — 몸 가운데 피벗, 공중): 흰 몸 + 회색 날개 + 노란 부리 + 성난 눈썹·붉은 눈
	C = FCanvas(30, 22)
	if Pose == "Attack":
		Wing = 3   # 접고 내리꽂음
	elif Pose == "Windup":
		Wing = -5
	else:
		Wing = (-4, -1, 3, -1)[Frame % 4]
	BY = 11 + (1 if Pose == "Attack" else 0)
	# 날개 (어깨 → 날개 끝: 위/아래로)
	for S in (-1, 1):
		SX = 15 + S * 3
		TX = 15 + S * (13 if Pose != "Attack" else 7)
		TY = BY + Wing
		Poly(C, [(SX, BY - 1), (TX, TY - 2), (TX - S * 1, TY + 1), (SX, BY + 3)], SG_G)
		C.Line(SX, BY + 2, TX - S, TY + 1, SG_D)
		C.Px(TX, TY - 2, (40, 40, 48))
		C.Px(TX - S, TY - 2, (40, 40, 48))
	# 몸·머리
	C.Ellipse(15, BY + 2, 4.0, 4.6, SG_W)
	C.Ellipse(15, BY - 4, 3.2, 3.0, SG_W)
	C.Px(13, BY - 5, (255, 255, 255))
	C.Rect(13, BY - 4, 13, BY - 4, (220, 40, 40))
	C.Rect(17, BY - 4, 17, BY - 4, (220, 40, 40))
	C.Line(12, BY - 6, 14, BY - 5, (40, 40, 48))  # 성난 눈썹
	C.Line(18, BY - 6, 16, BY - 5, (40, 40, 48))
	Open = 1 if Pose in ("Windup", "Attack") else 0
	C.Rect(14, BY - 2, 16, BY - 2, BEAK)
	C.Px(15, BY - 1 + Open, (230, 120, 40))
	C.Rect(13, BY + 6, 13, BY + 7, BEAK)
	C.Rect(17, BY + 6, 17, BY + 7, BEAK)
	C.Outline((54, 58, 70))
	return C


def DrawGhost(Pose, Frame):
	# 떠도는 망령 (공중 — 가운데 피벗): 해진 천을 뒤집어쓴 창백한 혼, 빛나는 눈, 흐느적이는 아랫자락 + 팔
	C = FCanvas(26, 30)
	Wave = Frame % 3
	Up = {"Windup": -2, "Attack": 1}.get(Pose, 0)
	Body, BodyL, BodyD = (196, 236, 246), (236, 252, 255), (128, 176, 204)
	CX, Top = 13, 3 + Up
	C.Ellipse(CX, Top + 6, 7.0, 6.4, Body)
	C.Rect(CX - 7, Top + 6, CX + 7, Top + 19, Body)
	C.Ellipse(CX - 2, Top + 4, 3.0, 2.4, BodyL)
	C.Rect(CX + 5, Top + 6, CX + 7, Top + 19, BodyD)
	# 너덜너덜한 아랫자락 (프레임마다 물결)
	for K in range(5):
		X = CX - 7 + K * 3
		L = 3 + ((K + Wave) % 3)
		C.Rect(X, Top + 20, X + 1, Top + 19 + L, Body if K < 4 else BodyD)
		C.Px(X + 2, Top + 20, BodyD)
	# 눈·입 (밤빛)
	Glow = (255, 120, 110) if Pose in ("Windup", "Attack") else (255, 250, 190)
	for EX in (CX - 3, CX + 2):
		C.Rect(EX, Top + 6, EX + 1, Top + 8, (30, 46, 70))
		C.Px(EX, Top + 6, Glow)
	C.Ellipse(CX, Top + 12, 1.4, 1.8, (30, 46, 70))
	# 팔 (흐느적 — 예비 동작은 위로, 공격은 앞으로 뻗음)
	for S in (-1, 1):
		if Pose == "Windup":
			C.Line(CX + S * 6, Top + 10, CX + S * 11, Top + 2, Body)
			C.Line(CX + S * 6, Top + 11, CX + S * 11, Top + 3, BodyD)
		elif Pose == "Attack":
			C.Line(CX + S * 6, Top + 11, CX + S * 12, Top + 13, Body)
		else:
			C.Line(CX + S * 6, Top + 10, CX + S * 10, Top + 15 + (Wave if S > 0 else 2 - Wave), Body)
	C.Outline((40, 70, 100))
	return C


HARBOR_ENEMY_FRAMES = {
	# 종류: { 동작: (그리기 인자 목록, 초당 프레임 또는 프레임별 길이, 반복) } — 박쥐처럼 공중 종류는 가운데 피벗
	"Crab":    {"Idle": ([("Idle", 0), ("Idle", 1)], 3.0, "Loop"), "Move": ([("Move", I) for I in range(4)], 12.0, "Loop"),
				"Windup": ([("Windup", 0)], 1.0, "Loop"), "Attack": ([("Attack", 0), ("Attack", 1)], 10.0, "Once")},
	"Pirate":  {"Idle": ([("Idle", 0), ("Idle", 1)], 3.0, "Loop"), "Move": ([("Move", I) for I in range(4)], 10.0, "Loop"),
				"Windup": ([("Windup", 0)], 1.0, "Loop"), "Attack": ([("Attack", 0), ("Attack", 1)], 10.0, "Once")},
	"Seagull": {"Idle": ([("Idle", I) for I in range(4)], 9.0, "Loop"), "Move": ([("Move", I) for I in range(4)], 13.0, "Loop"),
				"Windup": ([("Windup", 0)], 1.0, "Loop"), "Attack": ([("Attack", 0)], 1.0, "Loop")},
	"Ghost":   {"Idle": ([("Idle", I) for I in range(3)], 5.0, "Loop"), "Move": ([("Move", I) for I in range(3)], 7.0, "Loop"),
				"Windup": ([("Windup", 0), ("Windup", 1)], 6.0, "Loop"), "Attack": ([("Attack", 0)], 1.0, "Loop")},
}
HARBOR_ENEMY_DRAW = {"Crab": DrawCrab, "Pirate": DrawPirate, "Seagull": DrawSeagull, "Ghost": DrawGhost}
HARBOR_ENEMY_PIVOT = {"Crab": (0.5, 0.0), "Pirate": (0.5, 0.0), "Seagull": (0.5, 0.5), "Ghost": (0.5, 0.5)}


# ---- 보스 "해적 선장 바렌" (64x64, 옆모습 — 오른쪽을 봄, 왼쪽은 SetSpriteFlip) -------------------------------------------------------
COAT, COAT_L, COAT_D = (182, 40, 46), (224, 74, 68), (124, 22, 32)
HAT, HAT_L = (34, 30, 40), (68, 62, 76)
BEARD, BEARD_L = (30, 26, 32), (70, 62, 74)
CAPT_SKIN, CAPT_SKIN_D = (224, 168, 126), (178, 120, 88)
CAPTAIN_POSES = {"Dormant": 1, "Idle": 2, "Walk": 4, "Raise": 1, "Slash": 2, "Aim": 1, "Throw": 1}


def _Keg(C, X, Y, Spark=0):
	# 화약통 (작은 나무통 + 쇠테 + 불붙은 심지)
	C.Rect(X, Y, X + 8, Y + 8, (150, 96, 54))
	C.Rect(X, Y, X + 1, Y + 8, (190, 128, 74))
	C.Rect(X + 7, Y, X + 8, Y + 8, (104, 64, 36))
	for YY in (Y + 2, Y + 6):
		C.Rect(X, YY, X + 8, YY, (60, 60, 66))
	C.Line(X + 4, Y - 1, X + 6, Y - 3, (60, 50, 40))
	C.Px(X + 6 + Spark, Y - 4, (255, 230, 120))
	C.Px(X + 7, Y - 4 - Spark, (255, 150, 60))


def _Pistol(C, X, Y):
	# 부싯돌 권총 (오른쪽을 겨눔): 나무 손잡이 + 쇠 총열
	C.Rect(X, Y, X + 2, Y + 3, (110, 70, 40))
	C.Rect(X + 1, Y - 1, X + 9, Y, (70, 72, 80))
	C.Rect(X + 1, Y - 1, X + 9, Y - 1, (130, 134, 146))
	C.Px(X + 3, Y - 2, BRASS)


def _Limb(C, A_, B_, R, ColD, ColM, ColL=None):
	# 굵은 팔다리 (A → B, 반지름 R): 어두운 바탕 원을 잇고 → 가운데 색을 조금 위·왼쪽으로 겹친다 (빛 = 왼쪽 위)
	N = int(max(abs(B_[0] - A_[0]), abs(B_[1] - A_[1]), 1))
	for Col, Off, RR in ((ColD, (0.0, 0.0), R), (ColM, (-0.4, -0.5), R - 0.7)) + (((ColL, (-0.9, -0.9), max(0.6, R - 1.6)),) if ColL else ()):
		for K in range(N + 1):
			T = K / N
			CX, CY = A_[0] + (B_[0] - A_[0]) * T + Off[0], A_[1] + (B_[1] - A_[1]) * T + Off[1]
			for Y in range(int(CY - RR - 1), int(CY + RR + 2)):
				for X in range(int(CX - RR - 1), int(CX + RR + 2)):
					if (X + 0.5 - CX) ** 2 + (Y + 0.5 - CY) ** 2 <= RR * RR:
						C.Px(X, Y, Col)


def DrawCaptain(Pose, Frame):
	# 해적 선장 바렌: 붉은 긴 외투(허리를 조이고 자락이 퍼짐, 금 테·금 견장·큰 금 소매), 검은 삼각모(흰 해골 휘장·붉은 깃털),
	#   검은 턱수염(금 구슬), 큰 코·성난 눈썹·흉터, 왼손 쇠갈고리, 오른손 휜 칼 / 권총 / 화약통
	C = FCanvas(64, 64)
	B = (Frame % 2) if Pose == "Idle" else ((0, 1, 0, 1)[Frame % 4] if Pose == "Walk" else 0)
	Step = (3, 0, -3, 0)[Frame % 4] if Pose == "Walk" else (5 if Pose == "Slash" else 0)
	Lean = {"Slash": 3, "Raise": -2, "Throw": -2, "Aim": 1}.get(Pose, 0)
	Sleep = Pose == "Dormant"
	Pants, PantsD = (62, 58, 74), (40, 36, 50)
	BootD, BootM, BootL = (26, 20, 22), (44, 34, 34), (80, 64, 60)
	# ---- 다리 + 장화 (뒤 다리 먼저, 어둡게)
	HipX = 30 + Lean
	for (DX, Col, Dark) in ((-3 - Step, PantsD, True), (3 + Step, Pants, False)):
		KneeX = HipX + DX
		_Limb(C, (HipX + DX * 0.3, 44), (KneeX, 54), 3.0, PantsD, Col)
		C.Rect(KneeX - 3, 54, KneeX + 3, 61, BootD if Dark else BootM)
		C.Rect(KneeX - 3, 62, KneeX + 5, 63, BootD if Dark else BootM)
		C.Rect(KneeX - 4, 53, KneeX + 4, 55, BootM if Dark else BootL)   # 접은 장화 목
		if not Dark:
			C.Px(KneeX - 2, 57, BootL)
	# ---- 외투: 어깨(넓음) → 허리(조임) → 자락(퍼짐, 뒤로 조금 날림)
	Y0 = 25 + B
	X0 = 21 + Lean
	Flutter = (0, 1, 0, -1)[Frame % 4] if Pose == "Walk" else 0
	Poly(C, [(X0 - 1, Y0), (X0 + 19, Y0), (X0 + 17, Y0 + 13), (X0 + 22, 53), (X0 - 6 - Flutter, 53), (X0 + 1, Y0 + 13)], COAT)
	Poly(C, [(X0 - 1, Y0), (X0 + 4, Y0), (X0 + 4, Y0 + 13), (X0 - 2 - Flutter, 53), (X0 - 6 - Flutter, 53), (X0 + 1, Y0 + 13)], COAT_L)
	C.Line(X0 + 19, Y0, X0 + 17, Y0 + 13, COAT_D)
	C.Line(X0 + 17, Y0 + 13, X0 + 22, 53, COAT_D)
	C.Line(X0 + 16, Y0 + 13, X0 + 21, 53, COAT_D)
	# 앞여밈: 검은 조끼 + 흰 셔츠 주름 + 금 테 + 단추
	C.Rect(X0 + 12, Y0 + 1, X0 + 17, Y0 + 15, (38, 32, 44))
	C.Rect(X0 + 14, Y0 + 1, X0 + 17, Y0 + 6, (240, 238, 232))
	C.Px(X0 + 15, Y0 + 3, (200, 196, 190))
	C.Line(X0 + 12, Y0 + 1, X0 + 12, Y0 + 13, GOLD)
	C.Line(X0 + 12, Y0 + 13, X0 + 15, 53, GOLD)
	C.Line(X0 - 6 - Flutter, 53, X0 + 22, 53, GOLD)
	for Y in (Y0 + 4, Y0 + 8, Y0 + 12):
		C.Px(X0 + 10, Y, GOLD_L)
	# 허리띠 (금 버클) + 뒤 허리 권총
	C.Rect(X0 + 1, Y0 + 13, X0 + 17, Y0 + 14, (88, 56, 34))
	C.Rect(X0 + 13, Y0 + 13, X0 + 15, Y0 + 14, GOLD)
	_Pistol(C, X0 + 2, Y0 + 13)
	# ---- 머리: 얼굴(큰 코·눈·흉터) + 턱수염 + 삼각모
	HX, HY = 29 + Lean, 9 + B + (1 if Sleep else 0)
	C.Rect(HX + 1, HY + 4, HX + 12, HY + 14, CAPT_SKIN)
	C.Rect(HX + 2, HY + 3, HX + 11, HY + 3, CAPT_SKIN)
	C.Rect(HX + 10, HY + 4, HX + 12, HY + 14, CAPT_SKIN_D)
	C.Rect(HX + 13, HY + 7, HX + 15, HY + 10, CAPT_SKIN)          # 코 (매부리)
	C.Px(HX + 15, HY + 11, CAPT_SKIN_D)
	C.Px(HX + 13, HY + 11, CAPT_SKIN_D)
	C.Rect(HX + 2, HY + 7, HX + 3, HY + 10, CAPT_SKIN_D)          # 귀
	C.Px(HX + 2, HY + 11, GOLD)                                   # 귀걸이
	if Sleep:
		C.Rect(HX + 8, HY + 7, HX + 11, HY + 7, (30, 20, 24))
	else:
		C.Rect(HX + 9, HY + 6, HX + 11, HY + 7, (250, 248, 240))
		C.Rect(HX + 10, HY + 6, HX + 10, HY + 7, (30, 20, 24))
		C.Line(HX + 7, HY + 4, HX + 12, HY + 5, BEARD)            # 성난 눈썹
	C.Line(HX + 6, HY + 5, HX + 8, HY + 11, (176, 96, 86))        # 흉터
	Poly(C, [(HX + 2, HY + 11), (HX + 14, HY + 11), (HX + 13, HY + 17), (HX + 9, HY + 23), (HX + 4, HY + 18)], BEARD)
	C.Line(HX + 5, HY + 12, HX + 8, HY + 19, BEARD_L)
	C.Rect(HX + 10, HY + 11, HX + 15, HY + 12, BEARD)             # 콧수염
	if Pose in ("Slash", "Aim", "Throw"):
		C.Rect(HX + 11, HY + 13, HX + 13, HY + 13, (240, 236, 220))  # 이를 드러냄
	C.Px(HX + 9, HY + 22, GOLD)
	C.Px(HX + 6, HY + 18, GOLD)
	# 삼각모: 높은 머리 + 넓은 챙(앞뒤 끝이 들림) + 금 테 + 해골 휘장 + 붉은 깃털
	C.Rect(HX, HY - 4, HX + 13, HY + 2, HAT)
	C.Rect(HX + 2, HY - 6, HX + 11, HY - 4, HAT)
	C.Rect(HX + 2, HY - 5, HX + 4, HY, HAT_L)
	C.Rect(HX - 6, HY + 2, HX + 19, HY + 3, HAT)
	C.Line(HX - 6, HY + 2, HX - 9, HY - 2, HAT)
	C.Line(HX + 19, HY + 2, HX + 21, HY - 1, HAT)
	C.Line(HX - 6, HY + 3, HX + 19, HY + 3, GOLD)
	C.Ellipse(HX + 8.5, HY - 1.5, 2.0, 1.8, (240, 236, 224))        # 해골
	C.Px(HX + 7, HY - 2, HAT)
	C.Px(HX + 9, HY - 2, HAT)
	C.Line(HX + 6, HY + 1, HX + 11, HY + 1, (240, 236, 224))         # 뼈 가로
	for K in range(10):                                              # 깃털 (뒤로 휨)
		YY = HY - 5 + (K * K) // 14
		C.Px(HX - 1 - K, YY, (226, 54, 54))
		C.Px(HX - 1 - K, YY + 1, (160, 30, 36))
	# ---- 팔: 뒤팔(갈고리) → 앞팔 (칼·권총·화약통). 견장·금 소매
	SX, SY = X0 + 3, Y0 + 3
	FX, FY = X0 + 15, Y0 + 3
	Hook = (210, 216, 228)
	if Pose == "Throw":
		HandB = (SX - 1, SY - 14)
		_Limb(C, (SX, SY), HandB, 2.6, COAT_D, COAT)
		_Keg(C, HandB[0] - 5, HandB[1] - 10, Frame)
	else:
		HandB = (SX - 3, SY + 13)
		_Limb(C, (SX, SY), HandB, 2.6, COAT_D, COAT)
		C.Rect(HandB[0] - 2, HandB[1] - 1, HandB[0] + 2, HandB[1], GOLD)
		C.Line(HandB[0], HandB[1] + 1, HandB[0], HandB[1] + 4, Hook)
		C.Line(HandB[0], HandB[1] + 4, HandB[0] + 3, HandB[1] + 3, Hook)
		C.Px(HandB[0] + 3, HandB[1] + 2, Hook)
	if Sleep:
		_Limb(C, (FX, FY), (FX - 8, FY + 8), 2.8, COAT_D, COAT, COAT_L)   # 팔짱
		C.Rect(FX - 10, FY + 7, FX - 7, FY + 9, GOLD)
		_Cutlass(C, X0 + 2, Y0 + 17, X0 - 5, Y0 + 28, 0.5)              # 칼집에 꽂은 칼
	else:
		if Pose == "Raise":
			Hand = (FX - 3, FY - 15)
		elif Pose == "Slash":
			Hand = (FX + 12, FY + 3 + Frame * 5)
		elif Pose == "Aim":
			Hand = (FX + 13, FY + 1)
		else:
			Hand = (FX + 5, FY + 13)
		_Limb(C, (FX, FY), Hand, 2.8, COAT_D, COAT, COAT_L)
		C.Rect(Hand[0] - 2, Hand[1] - 2, Hand[0] + 1, Hand[1] + 1, GOLD)     # 금 소매
		C.Rect(Hand[0], Hand[1] - 1, Hand[0] + 2, Hand[1] + 1, CAPT_SKIN)
		if Pose == "Raise":
			_Cutlass(C, Hand[0] - 1, Hand[1] - 2, Hand[0] - 15, Hand[1] - 6, -1.2, 2)
		elif Pose == "Slash":
			_Cutlass(C, Hand[0] + 3, Hand[1], 63, Hand[1] - 3 + Frame * 10, 1.2, 2)
			if Frame == 0:
				for K in range(14):
					C.Px(Hand[0] + 3 + K, Hand[1] - 12 + (K * K) // 20, (255, 255, 255))
					C.Px(Hand[0] + 2 + K, Hand[1] - 10 + (K * K) // 20, (255, 226, 196))
		elif Pose == "Aim":
			_Pistol(C, Hand[0] + 2, Hand[1])
		else:
			_Cutlass(C, Hand[0] + 2, Hand[1] + 1, Hand[0] + 15, Hand[1] + 8 - B, 1.2, 2)
	C.Rect(FX - 2, FY - 1, FX + 2, FY, GOLD)                              # 견장
	C.Px(FX - 2, FY + 1, GOLD_D)
	C.Px(FX + 2, FY + 1, GOLD_D)
	C.Outline((24, 16, 22))
	return C


# ---- 항구 효과 (가운데 피벗) -------------------------------------------------------------------------------------------------------
def DrawBullet():
	C = FCanvas(10, 5)
	C.Rect(6, 1, 8, 3, (60, 62, 70))
	C.Px(6, 1, (160, 164, 176))
	for X in range(0, 6):
		C.Px(X, 2, (255, 230, 170) if X > 3 else (220, 200, 170))
	C.Outline((30, 26, 30))
	return C


def DrawKegFx(Frame):
	C = FCanvas(14, 16)
	_Keg(C, 2, 5, Frame)
	C.Outline((40, 26, 20))
	return C


def DrawCannonball():
	C = FCanvas(10, 10)
	C.Ellipse(5, 5, 4.2, 4.2, (50, 52, 60))
	C.Ellipse(4, 4, 1.8, 1.6, (110, 114, 128))
	C.Outline((20, 20, 26))
	return C


def DrawMuzzle(Frame):
	C = FCanvas(14, 12)
	R = (3.5, 5.5)[Frame]
	for K in range(8):
		A_ = K / 8 * math.pi * 2
		C.Line(7, 6, 7 + math.cos(A_) * R, 6 + math.sin(A_) * R * 0.8, (255, 220, 120) if K % 2 else (255, 150, 70))
	C.Rect(6, 5, 8, 7, (255, 255, 230))
	return C


# ---- 아이템 아이콘 (16x16) · 시계 해/달 --------------------------------------------------------------------------------------------
def DrawWorldIcon(Name):
	C = FCanvas(16, 16)
	if Name == "Harpoon":
		C.Line(2, 14, 11, 5, (150, 104, 60))
		C.Line(3, 14, 12, 5, (110, 74, 44))
		Poly(C, [(11, 3), (15, 1), (13, 5)], (220, 228, 238))
		C.Px(10, 4, (170, 180, 196))
		C.Px(13, 6, (170, 180, 196))
		C.Rect(1, 13, 2, 14, (200, 180, 140))
	elif Name == "GrilledFish":
		C.Line(1, 14, 14, 1, (170, 130, 80))
		F = _FishSprite(((214, 140, 70), (246, 196, 120), (150, 86, 40)), False, 9)
		_Paste(C, F, 1, 4)
		for X in (5, 8, 11):
			C.Line(X, 6, X - 1, 9, (90, 50, 28))
	elif Name == "Coat":
		Poly(C, [(4, 2), (7, 2), (8, 4), (9, 2), (12, 2), (14, 8), (12, 8), (12, 15), (4, 15), (4, 8), (2, 8)], (66, 104, 170))
		C.Rect(4, 9, 11, 15, (46, 76, 130))
		C.Rect(7, 4, 8, 15, (230, 230, 236))
		for Y in (7, 10, 13):
			C.Px(6, Y, BRASS)
	elif Name == "Pearl":
		C.Ellipse(8, 10, 5, 4.5, (200, 210, 220))
		for Y in range(16):
			for X in range(16):
				if (X + 0.5 - 8) ** 2 / 9.0 + (Y + 0.5 - 10) ** 2 / 6.8 < 1.0:
					C.P[Y, X] = (0, 0, 0, 0)
		C.Ellipse(8, 4.5, 2.8, 2.6, (250, 248, 240))
		C.Px(7, 3, (255, 255, 255))
		C.Px(9, 5, (210, 200, 220))
	elif Name == "Compass":
		C.Ellipse(8, 8, 6.6, 6.6, BRASS)
		C.Ellipse(8, 8, 5.0, 5.0, (246, 240, 220))
		C.Line(8, 8, 11, 4, (210, 50, 50))
		C.Line(8, 8, 5, 12, (60, 60, 70))
		C.Px(8, 8, BRASS_D)
		C.Rect(7, 0, 9, 1, BRASS)
	elif Name == "Lens":
		L = DrawLens(1)
		C.P[1:15, 1:15] = L.P[0:14, 0:14]
		return C
	elif Name == "Sun":
		C.Ellipse(8, 8, 4.2, 4.2, (255, 214, 90))
		C.Ellipse(7, 7, 2.0, 2.0, (255, 246, 190))
		for K in range(8):
			A_ = K / 8 * math.pi * 2
			C.Line(8 + math.cos(A_) * 5.5, 8 + math.sin(A_) * 5.5, 8 + math.cos(A_) * 7, 8 + math.sin(A_) * 7, (255, 190, 70))
	elif Name == "Moon":
		C.Ellipse(8, 8, 6.0, 6.0, (236, 236, 210))
		C.Ellipse(11, 6, 5.0, 5.0, (0, 0, 0), 0)
		for Y in range(16):
			for X in range(16):
				if (X + 0.5 - 11) ** 2 + (Y + 0.5 - 6) ** 2 < 25.0:
					C.P[Y, X] = (0, 0, 0, 0)
		C.Px(4, 9, (200, 200, 180))
	C.Outline((24, 16, 28))
	return C


WORLD_ICONS = ("Harpoon", "GrilledFish", "Coat", "Pearl", "Compass", "Lens", "Sun", "Moon")


# ---- 아틀라스 쓰기 --------------------------------------------------------------------------------------------------------------
def WriteHarborProps(Folder):
	Props = FAtlas(256)
	for Kind in ("A", "B", "C"):
		Props.Add(f"FishPile{Kind}", DrawFishPile(Kind), (0.5, 0.5))
	Props.Add("CrabPile", DrawCrabPile(), (0.5, 0.5))
	Props.Add("ShellPile", DrawShellPile(), (0.5, 0.5))
	Props.Add("FishHanging", DrawFishHanging(), (0.5, 1.0))
	Props.Add("Shell0", DrawShell(0), (0.5, 0.5))
	Props.Add("Shell1", DrawShell(1), (0.5, 0.5))
	Props.Add("Starfish", DrawStarfish(), (0.5, 0.5))
	for Kind in ("Village", "Market", "Danger", "Lighthouse", "Harbor"):
		Props.Add(f"Sign{Kind}", DrawSign(Kind), (0.5, 0.5))
	Props.Add("SignAnchor", DrawAnchorEmblem(), (0.5, 0.5))
	for Kind in ("Harbor", "Pirate"):
		for F in range(4):
			Props.Add(f"Flag{Kind}{F}", DrawFlag(Kind, F), (0.0, 1.0))
	Props.Add("LensCrate", DrawLensCrate(), (0.5, 0.0))
	for F in range(2):
		Props.Add(f"Lens{F}", DrawLens(F), (0.5, 0.0))
	for F in range(2):
		Props.Add(f"GullStand{F}", DrawGullStand(F), (0.5, 0.0))
	for F in range(4):
		Props.Add(f"GullFly{F}", DrawGullFly(F), (0.5, 0.5))
	Props.Save(Folder, "HarborProps")
	for Kind in ("Harbor", "Pirate"):
		WriteFlipbook(Folder, f"HarborFlag_{Kind}", "HarborProps.esprite", [f"Flag{Kind}{F}" for F in range(4)], 7.0)
	WriteFlipbook(Folder, "Harbor_Lens", "HarborProps.esprite", ["Lens0", "Lens1"], 0, "Loop", [0.7, 0.25])
	WriteFlipbook(Folder, "Harbor_GullStand", "HarborProps.esprite", ["GullStand0", "GullStand0", "GullStand1"], 0, "Loop", [1.4, 0.5, 0.35])
	WriteFlipbook(Folder, "Harbor_GullFly", "HarborProps.esprite", [f"GullFly{F}" for F in range(4)], 8.0)


def WriteHarborCharacters(Folder):
	# 항구 주민 (4프레임 대기)
	Npc = FAtlas(33 * 8 + 1)
	for Id, Draw in FOLK.items():
		for F in range(4):
			Npc.Add(f"{Id}{F}", Draw(F))
	Npc.Save(Folder, "HarborNpcs")
	for Id in FOLK:
		WriteFlipbook(Folder, f"Npc_{Id}", "HarborNpcs.esprite", [f"{Id}{F}" for F in range(4)], 0, "Loop", FOLK_TIMES)
	# 항구 적 (HD2DEnemy.lua 동작 이름)
	Enemy = FAtlas(256)
	Drawn = set()
	for Kind, Anims in HARBOR_ENEMY_FRAMES.items():
		for Anim, (Args, Rate, Loop) in Anims.items():
			Slices = []
			for Pose, F in Args:
				Name = f"{Kind}{Pose}{F}"
				if Name not in Drawn:
					Drawn.add(Name)
					Enemy.Add(Name, HARBOR_ENEMY_DRAW[Kind](Pose, F), HARBOR_ENEMY_PIVOT[Kind])
				Slices.append(Name)
			if isinstance(Rate, list):
				WriteFlipbook(Folder, f"{Kind}_{Anim}", "HarborEnemies.esprite", Slices, 0, Loop, Rate)
			else:
				WriteFlipbook(Folder, f"{Kind}_{Anim}", "HarborEnemies.esprite", Slices, Rate, Loop)
	Enemy.Save(Folder, "HarborEnemies")
	# 보스 해적 선장
	Boss = FAtlas(65 * 6 + 1)
	for Pose, Count in CAPTAIN_POSES.items():
		for F in range(Count):
			Boss.Add(f"{Pose}{F}", DrawCaptain(Pose, F))
	Boss.Save(Folder, "PirateCaptain")
	for Pose, Count in CAPTAIN_POSES.items():
		Rate = {"Idle": 2.2, "Walk": 7.0, "Slash": 14.0, "Throw": 6.0}.get(Pose, 1.0)
		WriteFlipbook(Folder, f"PirateCaptain_{Pose}", "PirateCaptain.esprite", [f"{Pose}{F}" for F in range(Count)], Rate, "Once" if Pose == "Slash" else "Loop")
	# 항구 효과 (가운데 피벗)
	Fx = FAtlas(128)
	Fx.Add("Bullet", DrawBullet(), (0.5, 0.5))
	for F in range(2):
		Fx.Add(f"Keg{F}", DrawKegFx(F), (0.5, 0.5))
		Fx.Add(f"Muzzle{F}", DrawMuzzle(F), (0.5, 0.5))
	Fx.Add("Cannonball", DrawCannonball(), (0.5, 0.5))
	Fx.Save(Folder, "HarborFx")
	WriteFlipbook(Folder, "HarborFx_Keg", "HarborFx.esprite", ["Keg0", "Keg1"], 10.0)
	WriteFlipbook(Folder, "HarborFx_Muzzle", "HarborFx.esprite", ["Muzzle0", "Muzzle1"], 18.0, "Once")


def WriteWorldUi(UiFolder):
	for Name in WORLD_ICONS:
		UpscaleSave(DrawWorldIcon(Name), os.path.join(UiFolder, "Icons", f"{Name}.png"))
	for Id, Draw in FOLK.items():
		UpscaleSave(A.DrawPortrait(Draw(0), PORTRAIT_BOX), os.path.join(UiFolder, "Portraits", f"{Id}.png"), 6)


def WriteWorldArt(Content):
	# HD2DGameplay.WriteAll이 부른다 (어느 맵 생성기로 돌려도 같은 결과)
	SpriteFolder = os.path.join(Content, "Sprites", "HD2D")
	WriteHarborProps(SpriteFolder)
	WriteHarborCharacters(SpriteFolder)
	WriteWorldUi(os.path.join(Content, "UI", "Demo", "HD2D"))
