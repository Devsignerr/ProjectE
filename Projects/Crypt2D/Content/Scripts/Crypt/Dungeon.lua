-- Crypt2D 던전 층 조립 (Script.Require("Scripts/Crypt/Dungeon.lua")) — 순수 로직 (엔티티·타일맵을 직접 건드리지 않는다).
--   층 = 방 격자(GridW x GridH 칸, 칸 하나 = 방 템플릿 40 x 24 타일)에서 시드 난수로 키운 트리(인접 칸끼리 문으로 연결).
--   BuildTiles가 템플릿 벽을 큰 격자 하나에 찍고, 연결된 쪽 문을 뚫은 뒤 자동 타일(Crypt2DRooms.py AutoTile과 같은 규칙)로
--   지형 타일맵·뒷벽 타일맵 SetTiles 목록을 만든다. 타일맵 셀 (0, 0) = 격자 칸 (0, 0) 방의 왼쪽 아래.
--   문 규약(템플릿 공통): 왼쪽/오른쪽 = 벽 2~5행, 아래 = 18~21열 0행 비움 + 1행 원웨이, 위 = 18~21열 22~23행 비움 + 20행 17~22열 착지 발판.
--   잠금 = 이 방 쪽 문 칸을 문 돌 타일로 채움(GateCells), 열림 = 원래대로(OpenCells).
local Rooms = Script.Require("Scripts/Crypt/Rooms.lua")

local Dungeon = {}

Dungeon.W = Rooms.Width   -- 40
Dungeon.H = Rooms.Height  -- 24
Dungeon.GridW = 5
Dungeon.GridH = 4

-- 타일 번호 (교회 tileset.png 21열 — Crypt2DRooms.py와 같음)
local TopGroups     = { { 210, 211, 212 }, { 214, 215, 216 }, { 218, 219, 220 } }
local FillTile      = 106
local CornerTile    = 258
local PlatformTiles = { 223, 225 }
local GateTiles     = { { 148, 149 }, { 169, 170 } }
local BrickTiles    = { 31, 52, 73 }
local TorchFlame, TorchBowl = 121, 142
local Decor2x4 = {
	{ { 33, 34 }, { 54, 55 }, { 75, 76 }, { 96, 97 } },    -- 촛불 창문
	{ { 39, 40 }, { 60, 61 }, { 81, 82 }, { 102, 103 } },  -- 아치
}

-- ---- 템플릿 해석 (모듈 로드 때 한 번)
local Templates = {}
local ByName = {}
for _, T in ipairs(Rooms.Templates) do
	local Parsed = { Name = T.Name, Kind = T.Kind, Cells = {}, Markers = {}, UpPath = T.UpPath } -- UpPath: 위 문까지 면 목록 (Rooms.lua 머리 주석)
	for RowIndex, Row in ipairs(T.Rows) do
		local Y = Dungeon.H - RowIndex
		for X = 0, Dungeon.W - 1 do
			local C = Row:sub(X + 1, X + 1)
			if C == "#" or C == "=" then
				Parsed.Cells[Y * Dungeon.W + X] = C
			elseif C ~= "." then
				Parsed.Markers[#Parsed.Markers + 1] = { Char = C, X = X, Y = Y }
			end
		end
	end
	Templates[#Templates + 1] = Parsed
	ByName[T.Name] = Parsed
end

function Dungeon.Template(Name)
	return ByName[Name]
end

function Dungeon.TemplatesOfKind(Kind)
	local Out = {}
	for _, T in ipairs(Templates) do
		if T.Kind == Kind then Out[#Out + 1] = T end
	end
	return Out
end

local function Key(SX, SY)
	return SY * 100 + SX
end

local Neighbors = { { "R", 1, 0, "L" }, { "L", -1, 0, "R" }, { "U", 0, 1, "D" }, { "D", 0, -1, "U" } }

local function NewRoom(Layout, SX, SY, Kind)
	local Room = { Id = #Layout.Rooms + 1, SX = SX, SY = SY, Kind = Kind, Doors = {}, Depth = 0, State = "Idle",
	               Visited = false, Known = false, Enemies = {} }
	Layout.Rooms[#Layout.Rooms + 1] = Room
	Layout.ByKey[Key(SX, SY)] = Room
	return Room
end

local function Connect(Layout, A, Side)
	for _, N in ipairs(Neighbors) do
		if N[1] == Side then
			local B = Layout.ByKey[Key(A.SX + N[2], A.SY + N[3])]
			A.Doors[Side] = true
			B.Doors[N[4]] = true
			return B
		end
	end
end

function Dungeon.RoomAt(Layout, SX, SY)
	return Layout.ByKey[Key(SX, SY)]
end

-- 시드 난수로 방 RoomCount개를 키운다: 시작 칸에서 이미 있는 방 하나를 골라 빈 이웃으로 뻗는다 (트리 — 순환 없음).
-- 출구 = 시작에서 가장 먼 방, 보물 = 출구가 아닌 막다른 방 중 가장 먼 것, 상점 = 남은 막다른 방 중 가장 먼 것(없으면 없음), 나머지 = 전투
function Dungeon.GenerateLayout(Rng, RoomCount)
	local Layout = { Rooms = {}, ByKey = {} }
	local Start = NewRoom(Layout, Rng:Int(1, Dungeon.GridW - 2), Rng:Int(0, Dungeon.GridH - 1), "Start")
	local Guard = 0
	while #Layout.Rooms < RoomCount and Guard < 2000 do
		Guard = Guard + 1
		local From = Layout.Rooms[Rng:Int(1, #Layout.Rooms)]
		local N = Neighbors[Rng:Int(1, 4)]
		-- 세로 연결은 덜 뽑는다 (가로 이동이 기본, 세로는 원웨이 구멍·사다리 발판)
		if (N[1] == "U" or N[1] == "D") and Rng:Chance(0.45) then
			goto continue
		end
		local NX, NY = From.SX + N[2], From.SY + N[3]
		if NX >= 0 and NY >= 0 and NX < Dungeon.GridW and NY < Dungeon.GridH and Layout.ByKey[Key(NX, NY)] == nil then
			local Room = NewRoom(Layout, NX, NY, "Combat")
			Room.Depth = From.Depth + 1
			Connect(Layout, From, N[1])
		end
		::continue::
	end
	local Exit = Start
	for _, Room in ipairs(Layout.Rooms) do
		if Room.Depth > Exit.Depth then Exit = Room end
	end
	Exit.Kind = "Exit"
	local Treasure = nil
	for _, Room in ipairs(Layout.Rooms) do
		local DoorCount = 0
		for _ in pairs(Room.Doors) do DoorCount = DoorCount + 1 end
		if Room.Kind == "Combat" and DoorCount == 1 and (Treasure == nil or Room.Depth > Treasure.Depth) then
			Treasure = Room
		end
	end
	if Treasure == nil then
		for _, Room in ipairs(Layout.Rooms) do
			if Room.Kind == "Combat" then Treasure = Room break end
		end
	end
	if Treasure then Treasure.Kind = "Treasure" end
	local Shop = nil
	for _, Room in ipairs(Layout.Rooms) do
		local DoorCount = 0
		for _ in pairs(Room.Doors) do DoorCount = DoorCount + 1 end
		if Room.Kind == "Combat" and DoorCount == 1 and (Shop == nil or Room.Depth > Shop.Depth) then
			Shop = Room
		end
	end
	if Shop then Shop.Kind = "Shop" end
	-- 템플릿: 전투 방은 겹치지 않게 돌아가며
	local Combat = Rng:Shuffle(Dungeon.TemplatesOfKind("Combat"))
	local CombatIndex = 0
	for _, Room in ipairs(Layout.Rooms) do
		if Room.Kind == "Combat" then
			CombatIndex = CombatIndex + 1
			Room.Template = Combat[(CombatIndex - 1) % #Combat + 1]
		else
			Room.Template = Dungeon.TemplatesOfKind(Room.Kind)[1]
		end
	end
	Layout.Start = Start
	Layout.Exit = Exit
	return Layout
end

-- 보스 층: 시작 방 → 보스 방 (오른쪽)
function Dungeon.BossLayout()
	local Layout = { Rooms = {}, ByKey = {} }
	local Start = NewRoom(Layout, 1, 1, "Start")
	local Boss = NewRoom(Layout, 2, 1, "Boss")
	Boss.Depth = 1
	Connect(Layout, Start, "R")
	Start.Template = Dungeon.Template("Start")
	Boss.Template = Dungeon.Template("Boss")
	Layout.Start = Start
	Layout.Exit = Boss
	return Layout
end

-- 자동 검증 코스: 시작 → 시험 전투(해골 하나) → 보물 → 상점(보물 오른쪽), 출구(보물 위)
function Dungeon.TestLayout()
	local Layout = { Rooms = {}, ByKey = {} }
	local Start = NewRoom(Layout, 0, 1, "Start")
	local Arena = NewRoom(Layout, 1, 1, "Combat")
	local Treasure = NewRoom(Layout, 2, 1, "Treasure")
	local Exit = NewRoom(Layout, 2, 2, "Exit")
	local Shop = NewRoom(Layout, 3, 1, "Shop")
	Arena.Depth, Treasure.Depth, Exit.Depth, Shop.Depth = 1, 2, 3, 3
	Connect(Layout, Start, "R")
	Connect(Layout, Arena, "R")
	Connect(Layout, Treasure, "U")
	Connect(Layout, Treasure, "R")
	Start.Template = Dungeon.Template("Start")
	Arena.Template = Dungeon.Template("TestArena")
	Treasure.Template = Dungeon.Template("Treasure")
	Exit.Template = Dungeon.Template("Exit")
	Shop.Template = Dungeon.Template("Shop")
	Layout.Start = Start
	Layout.Exit = Exit
	return Layout
end

-- 위 문 경로 검증 (자동 조종 Climb): 시작 → 위로 방 세 개 (전투 방 템플릿을 적 없이 — Kind "Climb"은 들어가면 정리된 방)
function Dungeon.ClimbLayout()
	local Layout = { Rooms = {}, ByKey = {} }
	local Names = { "Start", "Towers", "Steps", "Hall" }
	local Previous = nil
	for Index, Name in ipairs(Names) do
		local Room = NewRoom(Layout, 2, Index - 1, Index == 1 and "Start" or "Climb")
		Room.Depth = Index - 1
		Room.Template = Dungeon.Template(Name)
		if Previous then Connect(Layout, Previous, "U") end
		Previous = Room
	end
	Layout.Start = Layout.Rooms[1]
	Layout.Exit = Previous
	return Layout
end

-- ---- 문 칸 (방 로컬 → 전역 셀)
function Dungeon.DoorCells(Room, Side)
	local OX, OY = Room.SX * Dungeon.W, Room.SY * Dungeon.H
	local Cells = {}
	if Side == "L" or Side == "R" then
		local X0 = Side == "L" and 0 or Dungeon.W - 2
		for Y = 2, 5 do
			for X = X0, X0 + 1 do Cells[#Cells + 1] = { OX + X, OY + Y } end
		end
	else
		local Y0 = Side == "D" and 0 or Dungeon.H - 2
		for Y = Y0, Y0 + 1 do
			for X = 18, 21 do Cells[#Cells + 1] = { OX + X, OY + Y } end
		end
	end
	return Cells
end

-- 잠금: 문 칸을 문 돌로 (2 x 2 무늬)
function Dungeon.GateTiles(Room)
	local Out = {}
	for Side in pairs(Room.Doors) do
		for _, C in ipairs(Dungeon.DoorCells(Room, Side)) do
			local Tile = GateTiles[(C[2] % 2 == 0) and 2 or 1][(C[1] % 2) + 1]
			Out[#Out + 1] = { C[1], C[2], Tile }
		end
	end
	return Out
end

-- 열림: 문 칸을 원래대로 (아래 문 1행 = 원웨이, 나머지 = 빈칸). 반환: SetTiles 목록, 지울 칸 목록
function Dungeon.OpenTiles(Room)
	local Set, Erase = {}, {}
	local OY = Room.SY * Dungeon.H
	for Side in pairs(Room.Doors) do
		for _, C in ipairs(Dungeon.DoorCells(Room, Side)) do
			if Side == "D" and C[2] == OY + 1 then
				Set[#Set + 1] = { C[1], C[2], PlatformTiles[(C[1] % 2) + 1] }
			else
				Erase[#Erase + 1] = C
			end
		end
	end
	return Set, Erase
end

-- ---- 층 전체 타일 + 방 정보
--   반환 { Terrain = SetTiles 목록, Back = 뒷벽 벽돌 목록, Decor = 장식(창문·아치·횃불 받침) 목록, Torches = {{X, Z, Room}} }
--   방마다 Room.Markers = { {Char, X, Z(칸 바닥), CX, CY} } (월드 cm), Room.Rect = {X0, Z0, X1, Z1}
function Dungeon.BuildTiles(Layout, Rng, Cell)
	local W, H = Dungeon.W, Dungeon.H
	local Solid, Platform = {}, {}
	local function K(X, Y) return Y * 4096 + X end

	for _, Room in ipairs(Layout.Rooms) do
		local OX, OY = Room.SX * W, Room.SY * H
		for Index, C in pairs(Room.Template.Cells) do
			local X, Y = Index % W, Index // W
			if C == "#" then Solid[K(OX + X, OY + Y)] = true else Platform[K(OX + X, OY + Y)] = true end
		end
		Room.Rect = { OX * Cell, OY * Cell, (OX + W) * Cell, (OY + H) * Cell }
		Room.Markers = {}
		for _, M in ipairs(Room.Template.Markers) do
			Room.Markers[#Room.Markers + 1] = { Char = M.Char, X = (OX + M.X + 0.5) * Cell, Z = (OY + M.Y) * Cell, CX = OX + M.X, CY = OY + M.Y }
		end
	end
	-- 문 뚫기 (양쪽 방 모두 Doors에 표시되어 있으므로 방마다 자기 쪽만)
	for _, Room in ipairs(Layout.Rooms) do
		local OX, OY = Room.SX * W, Room.SY * H
		for Side in pairs(Room.Doors) do
			for _, C in ipairs(Dungeon.DoorCells(Room, Side)) do
				Solid[K(C[1], C[2])] = nil
				if Side == "D" and C[2] == OY + 1 then Platform[K(C[1], C[2])] = true end
			end
			if Side == "U" then
				for X = 17, 22 do
					if not Solid[K(OX + X, OY + 20)] then Platform[K(OX + X, OY + 20)] = true end
				end
			end
		end
	end

	local function InRoom(X, Y)
		return Layout.ByKey[Key(X // W, Y // H)] ~= nil and X >= 0 and Y >= 0
	end
	local function IsSolid(X, Y)
		if not InRoom(X, Y) then return true end
		return Solid[K(X, Y)] == true
	end
	local function Variant(X, Y)
		return TopGroups[((X // 3) + Y) % 3 + 1]
	end
	local function IsTopExposed(X, Y) return IsSolid(X, Y) and not IsSolid(X, Y + 1) end
	local function IsBottomExposed(X, Y) return IsSolid(X, Y) and not IsSolid(X, Y - 1) end
	local function IsLeftExposed(X, Y) return IsSolid(X, Y) and not IsSolid(X - 1, Y) end
	local function IsRightExposed(X, Y) return IsSolid(X, Y) and not IsSolid(X + 1, Y) end
	local function EndIndex(L, R)
		if not L and R then return 1 elseif not R and L then return 3 end
		return 2
	end

	local Terrain, Back, Decor, Torches = {}, {}, {}, {}
	for _, Room in ipairs(Layout.Rooms) do
		local OX, OY = Room.SX * W, Room.SY * H
		for Y = OY, OY + H - 1 do
			for X = OX, OX + W - 1 do
				if Platform[K(X, Y)] then
					Terrain[#Terrain + 1] = { X, Y, PlatformTiles[(X % 2) + 1] }
				elseif IsSolid(X, Y) then
					local Up, Down, Left, Right = IsSolid(X, Y + 1), IsSolid(X, Y - 1), IsSolid(X - 1, Y), IsSolid(X + 1, Y)
					local G = Variant(X, Y)
					local Tile, FX, FY, R = FillTile, false, false, false
					if not Up then
						Tile = G[EndIndex(Left, Right)]
					elseif not Down then
						Tile, FY = G[EndIndex(Left, Right)], true
					elseif not Left then
						Tile, R = G[2], true
					elseif not Right then
						Tile, FX, R = G[2], true, true
					elseif IsTopExposed(X, Y + 1) then
						Tile = Variant(X, Y + 1)[EndIndex(IsSolid(X - 1, Y + 1), IsSolid(X + 1, Y + 1))] + 21
					elseif IsBottomExposed(X, Y - 1) then
						Tile, FY = Variant(X, Y - 1)[EndIndex(IsSolid(X - 1, Y - 1), IsSolid(X + 1, Y - 1))] + 21, true
					elseif IsLeftExposed(X - 1, Y) then
						Tile, R = Variant(X - 1, Y)[2] + 21, true
					elseif IsRightExposed(X + 1, Y) then
						Tile, FX, R = Variant(X + 1, Y)[2] + 21, true, true
					elseif not IsSolid(X + 1, Y + 1) then
						Tile = CornerTile
					elseif not IsSolid(X - 1, Y + 1) then
						Tile, FX = CornerTile, true
					elseif not IsSolid(X + 1, Y - 1) then
						Tile, FY = CornerTile, true
					elseif not IsSolid(X - 1, Y - 1) then
						Tile, FX, FY = CornerTile, true, true
					end
					Terrain[#Terrain + 1] = { X, Y, Tile, FX, FY, R }
				end
				-- 뒷벽 벽돌 (위 → 아래 3행 반복)
				Back[#Back + 1] = { X, Y, BrickTiles[3 - (Y % 3)] }
			end
		end
		-- 창문·아치 장식: 9~16행, 2 x 4가 모두 빈 곳에 띄엄띄엄
		local Used = {}
		local Tries = 0
		local Count = 0
		while Count < 3 and Tries < 40 do
			Tries = Tries + 1
			local LX = Rng:Int(3, W - 5)
			local LY = Rng:Int(9, 15)
			local Ok = true
			for DY = -1, 4 do
				for DX = -1, 2 do
					local X, Y = OX + LX + DX, OY + LY + DY
					if Solid[K(X, Y)] or Platform[K(X, Y)] or Used[K(X, Y)] then Ok = false end
				end
			end
			if Ok then
				local D = Decor2x4[Rng:Int(1, #Decor2x4)]
				for Row = 1, 4 do
					for Col = 1, 2 do
						local X, Y = OX + LX + Col - 1, OY + LY + 4 - Row
						Decor[#Decor + 1] = { X, Y, D[Row][Col] }
						for DY = -2, 2 do for DX = -3, 3 do Used[K(X + DX, Y + DY)] = true end end
					end
				end
				Count = Count + 1
			end
		end
		-- 횃불 받침 (표식 t: 그 칸 = 그릇, 위 칸 = 불꽃)
		for _, M in ipairs(Room.Markers) do
			if M.Char == "t" then
				Decor[#Decor + 1] = { M.CX, M.CY, TorchBowl }
				Decor[#Decor + 1] = { M.CX, M.CY + 1, TorchFlame }
				Torches[#Torches + 1] = { X = (M.CX + 0.5) * Cell, Z = (M.CY + 1.5) * Cell, Room = Room }
			end
		end
	end
	return { Terrain = Terrain, Back = Back, Decor = Decor, Torches = Torches }
end

return Dungeon
