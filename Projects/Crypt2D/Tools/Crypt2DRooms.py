# Crypt2D 방 템플릿 (40 x 24 칸, 칸 = 16px 타일 = 64cm) + 자동 타일 규칙 (Lua Scripts/Crypt/Dungeon.lua와 같은 규칙 — 미리보기 씬용 파이썬 판).
#
# 템플릿은 그리기 명령(상자·발판·표식)으로 기술하고, 생성 스크립트가 행 문자열(위 → 아래)로 Scripts/Crypt/Rooms.lua에 쓴다.
# 문자: '#' 벽(타일), '.' 빈칸, '=' 원웨이 발판, 표식(빈칸 취급): g 지상 근접 적, w 마법사, f 비행 적, e 아무 지상 적, t 횃불,
#       c 상자, p 플레이어 시작, x 출구(다음 층 계단), b 보스, m 상인, s 상점 진열대(물건 자리).
# 공통 규약 (Lua 문 뚫기가 기대함): 0~1행 바닥, 22~23행 천장, 0~1·38~39열 벽.
#   왼쪽/오른쪽 문 = 그 벽의 2~5행, 아래 문 = 18~21열 0행 비움 + 1행 원웨이(위에서 S+Space로 내려감, 아래에서 뛰어 올라옴),
#   위 문 = 18~21열 22~23행 비움 + 20행 17~22열 착지 발판(Lua가 덧붙임). 템플릿 안에서 위 문까지 오르는 길은 한 번 점프(3.5칸)·
#   두 번 점프(7칸) 높이 안에 발판을 둔다 (단계 높이 ≤ 5칸).
# 위 문 경로 보장 (FindUpPath — 생성 때 검사, 실패하면 오류): 위 문이 생길 수 있는 템플릿(Kind ∉ UpDoorless)은 바닥(1행 윗면)에서
#   착지 발판(20행 17~22열)까지 "설 수 있는 면" 그래프 경로가 있어야 한다. 면 = '#' 또는 '='이고 위 3칸(캐릭터 2.5칸)에 '#'가 없는 칸의
#   가로 연속 구간. 면 높이 = '#' 윗면 타일은 아래 절반 다각형이라 Y + 0.5, '=' 원웨이는 Full이라 Y + 1 (Dungeon.lua 자동 타일과 같다).
#   오르기 = 높이 차 ≤ MaxRise(5칸 — 두 번 점프 8칸 안 여유)이고 가로 틈 ≤ MaxRiseGap(4칸), 대상이 '#'면 아래에서 뚫고
#   오를 수 없으므로 출발 면이 대상 가로 범위 밖으로 나와 있어야 한다(옆에서 올라탐). 머리 공간(캐릭터 2.5칸): 원웨이 아래로 겹치면 겹친 열 중
#   하나가, 아니면 출발 면 끝 ~ 대상 바로 옆 열이 출발 면 높이 ~ 대상 윗면 + 2.5칸에 '#'가 없어야 한다 (방 천장 아래 위 문 착지 발판은
#   위 문 구멍 아래에서만 오른다). 내려가기 = 가로 틈 ≤ MaxDropGap(8칸).
#   찾은 경로(면 목록)는 Rooms.lua의 UpPath로 나가 자동 조종(AutoPilot Climb/Explore)이 따라간다.
import math

RoomWidth  = 40
RoomHeight = 24

# 자동 타일 번호 (교회 타일셋 tileset.png 21열 — Crypt.etileset). 윗면 세 묶음 = (왼끝, 가운데, 오른끝), 바로 아래 몸통 = +21
TopGroups  = [(210, 211, 212), (214, 215, 216), (218, 219, 220)]
FillTile   = 106  # 단색 (Full)
CornerTile = 258  # 단색 + 안쪽 모서리 오각형 충돌 (기본 = 오른쪽 위 대각선이 빔)
PlatformTiles = (223, 225)  # 원웨이 발판 (번갈아)
GateTiles  = ((148, 149), (169, 170))  # 잠긴 문 돌 (2 x 2 반복, 위 행 먼저)
BrickTiles = (31, 52, 73)  # 뒷벽 벽돌 (위 → 아래 3행 반복)
TorchTiles = (121, 142)  # 횃불 받침 (위 = 불꽃, 아래 = 그릇)


UpDoorless = ("Boss", "Test")  # 위 문이 생기지 않는 방 (보스 층은 가로 두 칸, 검증 코스 전투 방은 가로)
MaxRise    = 5
MaxRiseGap = 4
MaxDropGap = 8
HeadCells  = 2.5  # 캐릭터 높이 (160cm = 2.5칸)


class FTemplate:
	def __init__(self, Name, Kind):
		self.Name  = Name
		self.Kind  = Kind
		self.Cells = [['.' for _ in range(RoomWidth)] for _ in range(RoomHeight)]  # [y][x], y = 0 아래
		self.Box(0, 0, RoomWidth - 1, 1)
		self.Box(0, RoomHeight - 2, RoomWidth - 1, RoomHeight - 1)
		self.Box(0, 0, 1, RoomHeight - 1)
		self.Box(RoomWidth - 2, 0, RoomWidth - 1, RoomHeight - 1)

	def Box(self, X0, Y0, X1, Y1, Char='#'):
		for Y in range(min(Y0, Y1), max(Y0, Y1) + 1):
			for X in range(min(X0, X1), max(X0, X1) + 1):
				self.Cells[Y][X] = Char
		return self

	def Plat(self, X0, X1, Y):
		return self.Box(X0, Y, X1, Y, '=')

	def Mark(self, Char, *Points):
		for (X, Y) in Points:
			if self.Cells[Y][X] != '.':
				raise ValueError(f"{self.Name}: 표식 '{Char}' ({X}, {Y}) 자리가 비어 있지 않음 ('{self.Cells[Y][X]}')")
			self.Cells[Y][X] = Char
		return self

	def Rows(self):
		# 위 → 아래
		return [''.join(self.Cells[Y]) for Y in range(RoomHeight - 1, -1, -1)]

	def Validate(self):
		Errors = []
		def Empty(X, Y):
			return self.Cells[Y][X] not in '#='
		for Y in range(2, 6):  # 문 앞 빈칸 (왼쪽/오른쪽 문 통로)
			for X in (2, 3, 36, 37):
				if not Empty(X, Y):
					Errors.append(f"문 앞 ({X}, {Y})가 막힘")
		for X in range(17, 23):  # 위 문 착지 발판 자리 + 위
			for Y in (19, 20, 21):
				if not Empty(X, Y):
					Errors.append(f"위 문 착지 자리 ({X}, {Y})가 막힘")
		for X in range(18, 22):  # 아래 문 위
			if not Empty(X, 2):
				Errors.append(f"아래 문 위 ({X}, 2)가 막힘")
		if Errors:
			raise ValueError(f"템플릿 {self.Name}: " + "; ".join(Errors))
		self.UpPath = None
		if self.Kind not in UpDoorless:
			self.UpPath = FindUpPath(self)
			if self.UpPath is None:
				raise ValueError(f"템플릿 {self.Name}: 바닥에서 위 문 착지 발판(20행 17~22열)까지 오르는 발판 경로가 없음 "
				                 f"(단계 높이 ≤ {MaxRise}칸, 가로 틈 ≤ {MaxRiseGap}칸)")
		return self


def FindSurfaces(Template, bUpDoor):
	# 설 수 있는 면 구간 [(X0, X1, Y, bSolid)] — Y = 면 칸 행. 위 문이 있으면 착지 발판을 더한 상태로. 반환: (면 목록, 칸)
	Cells = [Row[:] for Row in Template.Cells]
	if bUpDoor:
		for X in range(18, 22):
			Cells[22][X] = '.'
			Cells[23][X] = '.'
		for X in range(17, 23):
			if Cells[20][X] == '.':
				Cells[20][X] = '='

	def Blocked(X, Y):
		return Y < RoomHeight and Cells[Y][X] == '#'

	Surfaces = []
	for Y in range(1, RoomHeight - 2):
		X = 2
		while X < RoomWidth - 2:
			C = Cells[Y][X]
			if C in '#=' and not any(Blocked(X, Y + D) for D in (1, 2, 3)):
				X0 = X
				Solid = C == '#'
				while X + 1 < RoomWidth - 2 and Cells[Y][X + 1] == C and not any(Blocked(X + 1, Y + D) for D in (1, 2, 3)):
					X += 1
				Surfaces.append((X0, X, Y, Solid))
			X += 1
	return Surfaces, Cells


def FindUpPath(Template):
	# 바닥(Y = 1, 2~37열을 덮는 구간)에서 착지 발판(Y = 20, 17~22열)까지 BFS. 반환: 면 목록 (출발 바닥 포함) 또는 None
	Surfaces, Cells = FindSurfaces(Template, True)
	Start  = next((I for I, S in enumerate(Surfaces) if S[2] == 1 and S[0] <= 20 <= S[1]), None)
	Target = next((I for I, S in enumerate(Surfaces) if S[2] == 20 and S[0] <= 19 <= S[1]), None)
	if Start is None or Target is None:
		return None

	def Gap(A, B):
		return max(0, B[0] - A[1] - 1, A[0] - B[1] - 1)

	def Top(S):
		return S[2] + (0.5 if S[3] else 1.0)

	def ColumnClear(X, Y0, Y1):
		return all(Y >= RoomHeight or Cells[Y][X] != '#' for Y in range(Y0, Y1 + 1))

	def HasHeadroom(A, B):
		Y0 = A[2] + 1  # 출발 면 바로 위 칸부터
		Y1 = int(math.floor(Top(B) + HeadCells - 1.0e-6))
		Overlap = range(max(A[0], B[0]), min(A[1], B[1]) + 1)
		if not B[3] and len(Overlap) > 0:
			return any(ColumnClear(X, Y0, Y1) for X in Overlap)  # 원웨이 아래에서 곧장 뛰어 통과
		if B[0] > A[0]:
			Columns = range(min(A[1], B[0] - 1), B[0])  # 오른쪽 대상: 출발 면 끝 ~ 대상 왼쪽 옆 열
		else:
			Columns = range(B[1] + 1, max(A[0], B[1] + 1) + 1)
		return len(Columns) > 0 and all(ColumnClear(X, Y0, Y1) for X in Columns)

	def CanMove(A, B):
		Rise = Top(B) - Top(A)
		if Rise > 0:
			if Rise > MaxRise or Gap(A, B) > MaxRiseGap:
				return False
			if B[3] and A[0] >= B[0] and A[1] <= B[1]:
				return False  # 단단한 단 바로 아래에서는 뚫고 오를 수 없다
			return HasHeadroom(A, B)
		return Gap(A, B) <= MaxDropGap

	Previous = {Start: None}
	Queue = [Start]
	while Queue:
		Current = Queue.pop(0)
		if Current == Target:
			break
		for Next in range(len(Surfaces)):
			if Next not in Previous and CanMove(Surfaces[Current], Surfaces[Next]):
				Previous[Next] = Current
				Queue.append(Next)
	if Target not in Previous:
		return None
	Path = []
	Node = Target
	while Node is not None:
		Path.append(Surfaces[Node])
		Node = Previous[Node]
	return list(reversed(Path))


def BuildTemplates():
	T = []
	# ---- 시작 방 (적 없음). 자동 검증: 바닥 대시 → 단(28~32열, 6~7행 — 윗면 8칸, 두 번 점프로만) → 원웨이(14~18열 4행) 내려가기
	T.append(FTemplate("Start", "Start")
		.Plat(14, 18, 4).Plat(7, 11, 8).Plat(12, 16, 11).Plat(7, 11, 14).Plat(26, 30, 14).Plat(15, 24, 17)
		.Box(28, 6, 32, 7)
		.Mark('p', (6, 2)).Mark('t', (6, 18), (34, 18), (20, 9)))
	# ---- 전투 방 6종
	T.append(FTemplate("Hall", "Combat")
		.Plat(9, 13, 4).Plat(28, 32, 4).Box(13, 7, 17, 8).Box(24, 7, 28, 8)
		.Plat(5, 9, 11).Plat(16, 25, 11).Plat(30, 34, 11).Plat(12, 16, 14).Plat(25, 29, 14)
		.Plat(5, 9, 17).Plat(16, 25, 17).Plat(30, 34, 17)
		.Mark('g', (5, 2), (15, 2), (28, 2), (34, 2)).Mark('w', (21, 2)).Mark('f', (7, 19), (34, 19), (5, 12))
		.Mark('t', (3, 17), (36, 17), (20, 14)))
	T.append(FTemplate("Pillars", "Combat")
		.Box(10, 6, 12, 17).Box(29, 6, 31, 17)
		.Plat(4, 8, 6).Plat(3, 7, 10).Plat(4, 8, 14).Plat(32, 35, 6).Plat(32, 36, 10).Plat(32, 35, 14)
		.Plat(15, 20, 4).Plat(18, 23, 8).Plat(16, 25, 11).Plat(18, 23, 14).Plat(16, 25, 17)
		.Mark('g', (7, 2), (18, 2), (32, 2)).Mark('w', (20, 12)).Mark('f', (14, 19), (25, 19))
		.Mark('t', (5, 16), (34, 16), (20, 6)))
	T.append(FTemplate("Steps", "Combat")
		.Box(2, 6, 7, 7).Box(32, 6, 37, 7).Box(5, 13, 10, 14).Box(29, 13, 34, 14)
		.Plat(17, 22, 4).Plat(13, 16, 7).Plat(23, 26, 7).Plat(11, 15, 10).Plat(25, 29, 10)
		.Plat(16, 23, 14).Plat(9, 14, 17).Plat(25, 30, 17)
		.Plat(17, 22, 17)  # 위 문 착지 발판은 천장 아래라 위 문 구멍 밑에서만 오른다 (FindUpPath 머리 공간)
		.Mark('g', (10, 2), (19, 2), (29, 2)).Mark('e', (4, 8), (35, 8)).Mark('w', (7, 15)).Mark('f', (9, 19), (28, 19))
		.Mark('t', (3, 10), (36, 10), (20, 9)))
	T.append(FTemplate("Towers", "Combat")
		.Box(6, 8, 11, 9).Box(28, 8, 33, 9).Box(15, 13, 24, 14)
		.Plat(13, 17, 4).Plat(22, 26, 4).Plat(3, 6, 13).Plat(33, 36, 13).Plat(8, 12, 17).Plat(27, 31, 17)
		.Plat(17, 22, 17)  # 가운데 탑 위 → 위 문 착지 발판 (FindUpPath — 탑 윗면에서 착지 발판까지 6칸이라 한 단 더)
		.Mark('g', (4, 2), (20, 2), (35, 2)).Mark('w', (19, 15)).Mark('e', (30, 10)).Mark('f', (8, 19), (31, 19))
		.Mark('t', (3, 6), (36, 6), (20, 18)))
	T.append(FTemplate("Arena", "Combat")
		.Plat(6, 11, 5).Plat(28, 33, 5).Plat(15, 24, 8).Plat(11, 15, 11).Plat(24, 28, 11)
		.Plat(13, 18, 14).Plat(21, 26, 14).Plat(8, 13, 17).Plat(26, 31, 17)
		.Plat(17, 22, 17)  # 위 문 구멍 밑 발판 (FindUpPath 머리 공간)
		.Mark('f', (8, 19), (20, 18), (32, 19), (20, 12)).Mark('g', (10, 2), (30, 2)).Mark('w', (20, 9))
		.Mark('t', (3, 9), (36, 9), (20, 4)))
	T.append(FTemplate("Gallery", "Combat")
		.Box(2, 10, 12, 11).Box(27, 10, 37, 11)
		.Plat(16, 23, 4).Plat(13, 17, 7).Plat(22, 26, 7).Plat(6, 10, 15).Plat(29, 33, 15).Plat(14, 25, 16)
		.Mark('e', (6, 12), (32, 12)).Mark('g', (10, 2), (29, 2)).Mark('f', (19, 13), (12, 19), (27, 19)).Mark('w', (20, 17))
		.Mark('t', (4, 5), (35, 5), (7, 18), (32, 18)))
	# ---- 상점 방 (적 없음): 상인 + 진열대 3자리 (Shop.etable에서 층 조건·가중치로 뽑은 물건, F로 코인 구매)
	T.append(FTemplate("Shop", "Shop")
		.Plat(6, 10, 5).Plat(29, 33, 5).Plat(13, 17, 8).Plat(22, 26, 8).Plat(15, 24, 12).Plat(9, 13, 15).Plat(26, 30, 15).Plat(15, 24, 17)
		.Mark('m', (20, 2)).Mark('s', (11, 2), (15, 2), (25, 2)).Mark('t', (8, 9), (31, 9), (20, 14)))
	# ---- 보물 방 (상자) / 출구 방 (전투 + 계단) / 보스 방
	T.append(FTemplate("Treasure", "Treasure")
		.Plat(6, 10, 5).Plat(29, 33, 5).Plat(15, 24, 8).Plat(10, 14, 11).Plat(25, 29, 11).Plat(15, 24, 14)
		.Plat(8, 12, 17).Plat(27, 31, 17).Plat(17, 22, 17)
		.Mark('c', (20, 2)).Mark('t', (14, 4), (26, 4), (5, 12), (34, 12)))
	T.append(FTemplate("Exit", "Exit")
		.Plat(6, 10, 5).Plat(29, 33, 5).Plat(15, 24, 8).Plat(10, 14, 11).Plat(25, 29, 11).Plat(15, 24, 14)
		.Plat(8, 12, 17).Plat(27, 31, 17).Plat(17, 22, 17)
		.Mark('x', (20, 2)).Mark('g', (8, 2), (32, 2)).Mark('f', (12, 19), (28, 19)).Mark('w', (20, 9))
		.Mark('t', (14, 4), (26, 4)))
	T.append(FTemplate("Boss", "Boss")
		.Plat(5, 10, 6).Plat(29, 34, 6).Plat(14, 25, 10).Plat(4, 9, 14).Plat(30, 35, 14)
		.Mark('b', (20, 16)).Mark('t', (3, 9), (36, 9), (12, 18), (27, 18)))
	# ---- 자동 검증 코스: 전투 방(해골 하나) — 시작 방 오른쪽에 붙는다
	T.append(FTemplate("TestArena", "Test")
		.Plat(9, 13, 4).Plat(26, 30, 4).Plat(16, 23, 8)
		.Mark('g', (24, 2)).Mark('t', (5, 9), (34, 9)))
	for Template in T:
		Template.Validate()
	return T


# ---- 자동 타일 (Dungeon.lua AutoTile과 같은 규칙)
def AutoTile(IsSolid, IsPlatform, Cells):
	# IsSolid(x, y) / IsPlatform(x, y): 방 밖은 IsSolid가 참을 돌려야 한다 (바깥 벽이 바깥쪽 면을 그리지 않게).
	# Cells: 타일을 놓을 칸 목록 (x, y). 반환: {(x, y): (타일, FlipX, FlipY, Rotate90)}
	Out = {}

	def Variant(X, Y):
		return TopGroups[(X // 3 + Y) % 3]

	def IsTopExposed(X, Y):
		return IsSolid(X, Y) and not IsSolid(X, Y + 1)

	def IsBottomExposed(X, Y):
		return IsSolid(X, Y) and not IsSolid(X, Y - 1)

	def IsLeftExposed(X, Y):
		return IsSolid(X, Y) and not IsSolid(X - 1, Y)

	def IsRightExposed(X, Y):
		return IsSolid(X, Y) and not IsSolid(X + 1, Y)

	for (X, Y) in Cells:
		if IsPlatform(X, Y):
			Out[(X, Y)] = (PlatformTiles[X % 2], False, False, False)
			continue
		if not IsSolid(X, Y):
			continue
		Up, Down, Left, Right = IsSolid(X, Y + 1), IsSolid(X, Y - 1), IsSolid(X - 1, Y), IsSolid(X + 1, Y)
		Group = Variant(X, Y)
		if not Up:
			if not Left and Right:
				Out[(X, Y)] = (Group[0], False, False, False)
			elif not Right and Left:
				Out[(X, Y)] = (Group[2], False, False, False)
			else:
				Out[(X, Y)] = (Group[1], False, False, False)
		elif not Down:
			if not Left and Right:
				Out[(X, Y)] = (Group[0], False, True, False)
			elif not Right and Left:
				Out[(X, Y)] = (Group[2], False, True, False)
			else:
				Out[(X, Y)] = (Group[1], False, True, False)
		elif not Left:
			Out[(X, Y)] = (Group[1], False, False, True)       # 면이 왼쪽 (반시계 90°)
		elif not Right:
			Out[(X, Y)] = (Group[1], True, False, True)        # 면이 오른쪽 (회전 + 좌우 반전)
		elif IsTopExposed(X, Y + 1):
			Above = Variant(X, Y + 1)
			UpL, UpR = IsSolid(X - 1, Y + 1), IsSolid(X + 1, Y + 1)
			Index = 0 if (not UpL and UpR) else (2 if (not UpR and UpL) else 1)
			Out[(X, Y)] = (Above[Index] + 21, False, False, False)
		elif IsBottomExposed(X, Y - 1):
			Below = Variant(X, Y - 1)
			DnL, DnR = IsSolid(X - 1, Y - 1), IsSolid(X + 1, Y - 1)
			Index = 0 if (not DnL and DnR) else (2 if (not DnR and DnL) else 1)
			Out[(X, Y)] = (Below[Index] + 21, False, True, False)
		elif IsLeftExposed(X - 1, Y):
			Out[(X, Y)] = (Variant(X - 1, Y)[1] + 21, False, False, True)
		elif IsRightExposed(X + 1, Y):
			Out[(X, Y)] = (Variant(X + 1, Y)[1] + 21, True, False, True)
		elif not IsSolid(X + 1, Y + 1):
			Out[(X, Y)] = (CornerTile, False, False, False)
		elif not IsSolid(X - 1, Y + 1):
			Out[(X, Y)] = (CornerTile, True, False, False)
		elif not IsSolid(X + 1, Y - 1):
			Out[(X, Y)] = (CornerTile, False, True, False)
		elif not IsSolid(X - 1, Y - 1):
			Out[(X, Y)] = (CornerTile, True, True, False)
		else:
			Out[(X, Y)] = (FillTile, False, False, False)
	return Out


def TemplateTiles(Template, Doors=("L", "R", "U", "D")):
	# 미리보기용: 방 하나를 문을 뚫은 채 자동 타일 (방 밖 = 벽)
	Solid = set()
	Platform = set()
	for Y in range(RoomHeight):
		for X in range(RoomWidth):
			C = Template.Cells[Y][X]
			if C == '#':
				Solid.add((X, Y))
			elif C == '=':
				Platform.add((X, Y))
	if "L" in Doors:
		for Y in range(2, 6):
			Solid.discard((0, Y)); Solid.discard((1, Y))
	if "R" in Doors:
		for Y in range(2, 6):
			Solid.discard((38, Y)); Solid.discard((39, Y))
	if "D" in Doors:
		for X in range(18, 22):
			Solid.discard((X, 0)); Solid.discard((X, 1)); Platform.add((X, 1))
	if "U" in Doors:
		for X in range(18, 22):
			Solid.discard((X, 22)); Solid.discard((X, 23))
		for X in range(17, 23):
			if (X, 20) not in Solid:
				Platform.add((X, 20))

	def IsSolid(X, Y):
		if X < 0 or Y < 0 or X >= RoomWidth or Y >= RoomHeight:
			return True
		return (X, Y) in Solid

	Cells = [(X, Y) for Y in range(RoomHeight) for X in range(RoomWidth)]
	return AutoTile(IsSolid, lambda X, Y: (X, Y) in Platform, Cells)
