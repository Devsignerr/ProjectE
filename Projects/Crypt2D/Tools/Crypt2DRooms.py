# Crypt2D 방 템플릿 (40 x 24 칸, 칸 = 16px 타일 = 64cm) + 자동 타일 규칙 (Lua Scripts/Crypt/Dungeon.lua와 같은 규칙 — 미리보기 씬용 파이썬 판).
#
# 템플릿은 그리기 명령(상자·발판·표식)으로 기술하고, 생성 스크립트가 행 문자열(위 → 아래)로 Scripts/Crypt/Rooms.lua에 쓴다.
# 문자: '#' 벽(타일), '.' 빈칸, '=' 원웨이 발판, 표식(빈칸 취급): g 지상 근접 적, w 마법사, f 비행 적, e 아무 지상 적, t 횃불,
#       c 상자, p 플레이어 시작, x 출구(다음 층 계단), b 보스.
# 공통 규약 (Lua 문 뚫기가 기대함): 0~1행 바닥, 22~23행 천장, 0~1·38~39열 벽.
#   왼쪽/오른쪽 문 = 그 벽의 2~5행, 아래 문 = 18~21열 0행 비움 + 1행 원웨이(위에서 S+Space로 내려감, 아래에서 뛰어 올라옴),
#   위 문 = 18~21열 22~23행 비움 + 20행 17~22열 착지 발판(Lua가 덧붙임). 템플릿 안에서 위 문까지 오르는 길은 한 번 점프(3.5칸)·
#   두 번 점프(7칸) 높이 안에 발판을 둔다 (단계 높이 ≤ 5칸).
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
		return self


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
		.Mark('g', (10, 2), (19, 2), (29, 2)).Mark('e', (4, 8), (35, 8)).Mark('w', (7, 15)).Mark('f', (9, 19), (28, 19))
		.Mark('t', (3, 10), (36, 10), (20, 9)))
	T.append(FTemplate("Towers", "Combat")
		.Box(6, 8, 11, 9).Box(28, 8, 33, 9).Box(15, 13, 24, 14)
		.Plat(13, 17, 4).Plat(22, 26, 4).Plat(3, 6, 13).Plat(33, 36, 13).Plat(8, 12, 17).Plat(27, 31, 17)
		.Mark('g', (4, 2), (20, 2), (35, 2)).Mark('w', (19, 15)).Mark('e', (30, 10)).Mark('f', (8, 19), (31, 19))
		.Mark('t', (3, 6), (36, 6), (20, 18)))
	T.append(FTemplate("Arena", "Combat")
		.Plat(6, 11, 5).Plat(28, 33, 5).Plat(15, 24, 8).Plat(11, 15, 11).Plat(24, 28, 11)
		.Plat(13, 18, 14).Plat(21, 26, 14).Plat(8, 13, 17).Plat(26, 31, 17)
		.Mark('f', (8, 19), (20, 18), (32, 19), (20, 12)).Mark('g', (10, 2), (30, 2)).Mark('w', (20, 9))
		.Mark('t', (3, 9), (36, 9), (20, 4)))
	T.append(FTemplate("Gallery", "Combat")
		.Box(2, 10, 12, 11).Box(27, 10, 37, 11)
		.Plat(16, 23, 4).Plat(13, 17, 7).Plat(22, 26, 7).Plat(6, 10, 15).Plat(29, 33, 15).Plat(14, 25, 16)
		.Mark('e', (6, 12), (32, 12)).Mark('g', (10, 2), (29, 2)).Mark('f', (19, 13), (12, 19), (27, 19)).Mark('w', (20, 17))
		.Mark('t', (4, 5), (35, 5), (7, 18), (32, 18)))
	# ---- 보물 방 (상자) / 출구 방 (전투 + 계단) / 보스 방
	T.append(FTemplate("Treasure", "Treasure")
		.Plat(6, 10, 5).Plat(29, 33, 5).Plat(15, 24, 8).Plat(10, 14, 11).Plat(25, 29, 11).Plat(15, 24, 14)
		.Plat(8, 12, 17).Plat(27, 31, 17)
		.Mark('c', (20, 2)).Mark('t', (14, 4), (26, 4), (5, 12), (34, 12)))
	T.append(FTemplate("Exit", "Exit")
		.Plat(6, 10, 5).Plat(29, 33, 5).Plat(15, 24, 8).Plat(10, 14, 11).Plat(25, 29, 11).Plat(15, 24, 14)
		.Plat(8, 12, 17).Plat(27, 31, 17)
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
