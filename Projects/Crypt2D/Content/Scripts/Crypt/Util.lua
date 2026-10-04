-- Crypt2D 공용 도우미 (Script.Require("Scripts/Crypt/Util.lua")). 가변 상태를 두지 않는다 (모든 스크립트가 공유하는 모듈 값).
--   2D 평면 = 월드 X(오른쪽)·Z(위), 깊이 Y. 2D 각 = 화면 반시계 + (도) → 회전 Quat.FromAxisAngle(+Y, -각) (Physics2DMath 규약)
local U = {}

U.Cell = 64 -- 타일 한 칸 (cm) = 16 도트 × 4cm
U.Dot  = 4  -- 도트 한 개 (cm)

function U.Clamp(V, Lo, Hi)
	if V < Lo then return Lo end
	if V > Hi then return Hi end
	return V
end

function U.Lerp(A, B, T)
	return A + (B - A) * T
end

function U.Sign(V)
	if V > 0 then return 1 elseif V < 0 then return -1 end
	return 0
end

function U.Approach(Current, Target, Step)
	if Current < Target then return math.min(Current + Step, Target) end
	return math.max(Current - Step, Target)
end

-- 프레임 독립 지수 보간 계수 (Rate = 1/초)
function U.Smooth(Rate, Dt)
	return 1.0 - math.exp(-Rate * Dt)
end

function U.Length(X, Z)
	return math.sqrt(X * X + Z * Z)
end

function U.Normalize(X, Z)
	local L = math.sqrt(X * X + Z * Z)
	if L < 1e-5 then return 1.0, 0.0, 0.0 end
	return X / L, Z / L, L
end

-- 방향 → 각 (도, +X = 0, 반시계 +)
function U.AngleOf(X, Z)
	return math.deg(math.atan(Z, X))
end

function U.DirOf(Degrees)
	local R = math.rad(Degrees)
	return math.cos(R), math.sin(R)
end

function U.AngleDelta(A, B)
	local D = (B - A) % 360
	if D > 180 then D = D - 360 end
	return D
end

function U.Rot2D(Degrees)
	return Quat.FromAxisAngle(Vector3(0, 1, 0), -Degrees)
end

function U.V(X, Z, Y)
	return Vector3(X, Y or 0, Z)
end

-- ---- 결정적 난수 (xorshift64*, Lua 5.4 정수). 같은 시드 → 같은 층·같은 적 배치
local Rng = {}
Rng.__index = Rng

function U.Rng(Seed)
	local State = math.tointeger(Seed) or 1
	if State == 0 then State = 0x2545F491 end
	local Self = setmetatable({ State = State }, Rng)
	for _ = 1, 4 do Self:NextInt() end
	return Self
end

function Rng:NextInt()
	local X = self.State
	X = X ~ (X >> 12)
	X = X ~ (X << 25)
	X = X ~ (X >> 27)
	self.State = X
	return (X * 0x2545F4914F6CDD1D) >> 11 -- 53비트 양수
end

-- [0, 1)
function Rng:Next()
	return (self:NextInt() & 0xFFFFFFFFFFFFF) / 0x10000000000000
end

-- [Lo, Hi] 정수
function Rng:Int(Lo, Hi)
	return Lo + math.floor(self:Next() * (Hi - Lo + 1))
end

function Rng:Range(Lo, Hi)
	return Lo + self:Next() * (Hi - Lo)
end

function Rng:Chance(P)
	return self:Next() < P
end

function Rng:Pick(List)
	if #List == 0 then return nil end
	return List[self:Int(1, #List)]
end

-- 가중치 뽑기: Items = { {Value, Weight}, ... }
function Rng:Weighted(Items)
	local Total = 0
	for _, Item in ipairs(Items) do Total = Total + Item[2] end
	if Total <= 0 then return nil end
	local R = self:Next() * Total
	for _, Item in ipairs(Items) do
		R = R - Item[2]
		if R < 0 then return Item[1] end
	end
	return Items[#Items][1]
end

function Rng:Shuffle(List)
	for I = #List, 2, -1 do
		local J = self:Int(1, I)
		List[I], List[J] = List[J], List[I]
	end
	return List
end

return U
