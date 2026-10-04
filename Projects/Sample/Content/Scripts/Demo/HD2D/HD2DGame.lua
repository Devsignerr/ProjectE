-- HD-2D 데모 게임 관리 (Scenes/Demo/HD2D.escene의 "HD2DGame" 엔티티 — Tools/DemoMap/BuildHD2D.py가 배치).
--   슬라임 등록·소환·부활, 효과 스프라이트(FxSprite 프리팹 조각 — 스크립트 없이 여기서 수명 관리), 화면 흔들림, 자동 플레이 결과 판정.
--   좌표: 카메라가 +Y 쪽에서 -Y를 내려다본다 → 화면 오른쪽 = +X, 화면 위(안쪽) = -Y. 스프라이트는 XZ 평면(앞면 +Y)에 서 있다.
--   다른 스크립트는 Scene.Find("HD2DGame"):GetScript()로 쓴다 (Player = HD2DPlayer.lua, 슬라임 = HD2DSlime.lua).
local HD2DGame = {
	Properties = {
		SlimePrefab     = "Prefabs/Demo/HD2D/Slime.eprefab",
		FxPrefab        = "Prefabs/Demo/HD2D/FxSprite.eprefab",
		SpawnPoints     = "",   -- "x,y,z;x,y,z;..." (z = 캡슐 중심 높이 — 생성 스크립트가 지면 높이로 계산)
		SlimeCount      = 6,
		RespawnTime     = 8.0,  -- 초
		RespawnMinDistance = 900.0, -- 플레이어에게서 이만큼 떨어진 자리에서만 부활 (cm)
		AutoPlay        = false, -- 자동 검증: 플레이어가 스스로 싸우고 AutoPlaySeconds 뒤 결과를 로그로 남긴다
		AutoPlaySeconds = 40.0,
	},
}

local FxSprite = "Sprites/HD2D/Fx.esprite"
local FxLife = { Slash = 0.18, Spark = 0.17, Dust = 0.29, Poof = 0.36 }

function HD2DGame:OnStart()
	self.Slimes = {}       -- 엔티티 Id → { Entity, Script }
	self.Fx = {}           -- { Entity, Life, Max, Fade, Color }
	self.Respawns = {}     -- 남은 시간 목록
	self.Kills = 0
	self.Shake, self.ShakeTime = 0.0, 0.0
	self.Time = 0.0
	self.Seed = 12345
	self.Points = {}
	for Item in string.gmatch(self.Properties.SpawnPoints, "[^;]+") do
		local X, Y, Z = string.match(Item, "([-%d%.]+),([-%d%.]+),([-%d%.]+)")
		if X then
			self.Points[#self.Points + 1] = Vector3(tonumber(X), tonumber(Y), tonumber(Z))
		end
	end
	for Index = 1, math.min(self.Properties.SlimeCount, #self.Points) do
		self:SpawnSlime(self.Points[Index])
	end
	Log.Info("[HD2D] 시작: 슬라임 " .. math.min(self.Properties.SlimeCount, #self.Points) .. "마리, 소환 지점 " .. #self.Points .. "곳")
end

-- 결정적 난수 (0~1) — 자동 검증 화면이 실행마다 같도록 math.random을 쓰지 않는다
function HD2DGame:Random()
	self.Seed = (self.Seed * 1103515245 + 12345) % 2147483648
	return (self.Seed % 100000) / 100000.0
end

function HD2DGame:GetPlayer()
	if not self.PlayerScript then
		local P = Scene.Find("Player")
		self.PlayerScript = P and P:GetScript() or nil
	end
	return self.PlayerScript
end

-- ---- 슬라임
function HD2DGame:SpawnSlime(Position)
	Scene.SpawnPrefab(self.Properties.SlimePrefab, Position, function(E)
		-- 몸 색을 조금씩 다르게 (같은 종류 안의 변화)
		local Tints = { { 1, 1, 1 }, { 0.82, 1, 0.9 }, { 1, 0.95, 0.78 } }
		local T = Tints[1 + math.floor(self:Random() * #Tints) % #Tints]
		local Body = E:FindChild("Visual") and E:FindChild("Visual"):FindChild("Body")
		if Body then
			Body:GetComponent("SpriteComponent").Color = Vector4(T[1], T[2], T[3], 1)
		end
	end)
end

function HD2DGame:RegisterSlime(Script)
	self.Slimes[Script.entity.Id] = Script
end

function HD2DGame:UnregisterSlime(Script)
	self.Slimes[Script.entity.Id] = nil
end

function HD2DGame:OnSlimeKilled(Script)
	self.Kills = self.Kills + 1
	self.Respawns[#self.Respawns + 1] = self.Properties.RespawnTime
	Log.Info("[HD2D] 슬라임 처치 (누적 " .. self.Kills .. ")")
end

-- 중심 Center에서 수평 반경 Radius 안의 살아 있는 슬라임 스크립트 목록
function HD2DGame:FindSlimes(Center, Radius)
	local Found = {}
	for _, S in pairs(self.Slimes) do
		if S.entity:IsValid() and not S.bDead then
			local D = S.entity:GetWorldPosition() - Center
			D.Z = 0
			if D:Length() <= Radius then
				Found[#Found + 1] = S
			end
		end
	end
	return Found
end

function HD2DGame:NearestSlime(Center)
	local Best, BestDist = nil, 1.0e9
	for _, S in pairs(self.Slimes) do
		if S.entity:IsValid() and not S.bDead then
			local D = S.entity:GetWorldPosition() - Center
			D.Z = 0
			local L = D:Length()
			if L < BestDist then
				Best, BestDist = S, L
			end
		end
	end
	return Best, BestDist
end

function HD2DGame:UpdateRespawns(Dt)
	local Keep = {}
	for _, T in ipairs(self.Respawns) do
		T = T - Dt
		if T <= 0 then
			local Player = self:GetPlayer()
			local PP = Player and Player.entity:GetWorldPosition() or Vector3(0, 0, 0)
			-- 플레이어에게서 먼 지점 중 하나 (몇 번 굴려도 없으면 가장 먼 곳)
			local Pick, Far, FarDist = nil, nil, -1
			for _ = 1, 8 do
				local P = self.Points[1 + math.floor(self:Random() * #self.Points) % #self.Points]
				local D = P - PP
				D.Z = 0
				if D:Length() > FarDist then Far, FarDist = P, D:Length() end
				if D:Length() >= self.Properties.RespawnMinDistance then
					Pick = P
					break
				end
			end
			self:SpawnSlime(Pick or Far)
		else
			Keep[#Keep + 1] = T
		end
	end
	self.Respawns = Keep
end

-- ---- 효과 스프라이트
-- Opt: Rotation(화면 반시계 도), FlipX, FlipY, Scale, Color {r,g,b,a}, Blend(0 알파/2 가산/3 마스크), Life, Lit
function HD2DGame:SpawnFx(Name, Position, Opt)
	Opt = Opt or {}
	self:SpawnSprite({ Sprite = FxSprite, Flipbook = "Sprites/HD2D/Fx_" .. Name .. ".eflipbook", Position = Position, Rotation = Opt.Rotation,
	                   FlipX = Opt.FlipX, FlipY = Opt.FlipY, Scale = Opt.Scale, Color = Opt.Color, Blend = Opt.Blend or 0, Lit = Opt.Lit,
	                   Life = Opt.Life or FxLife[Name] or 0.3 })
end

-- 잔상: 지금 슬라이스의 사본이 색 알파를 잃으며 사라진다
function HD2DGame:SpawnAfterimage(Sprite, Slice, Position, FlipX, Color)
	self:SpawnSprite({ Sprite = Sprite, Slice = Slice, Position = Position, FlipX = FlipX, Blend = 0, Life = 0.22, Fade = true,
	                   Color = Color or { 0.55, 0.8, 1.0, 0.55 } })
end

function HD2DGame:SpawnSprite(Desc)
	Scene.SpawnPrefab(self.Properties.FxPrefab, Desc.Position, function(E)
		local S = E:GetComponent("SpriteComponent")
		S.Sprite = Desc.Sprite
		S.Slice = Desc.Slice or ""
		S.Blend = Desc.Blend or 0
		S.Lit = Desc.Lit == true
		local C = Desc.Color or { 1, 1, 1, 1 }
		S.Color = Vector4(C[1], C[2], C[3], C[4])
		S.Visible = true
		if Desc.FlipX or Desc.FlipY then E:SetSpriteFlip(Desc.FlipX == true, Desc.FlipY == true) end
		if Desc.Rotation then E:SetRotation(Quat.FromAxisAngle(Vector3(0, 1, 0), -Desc.Rotation)) end
		if Desc.Scale then E:SetScale(Vector3(Desc.Scale, 1, Desc.Scale)) end
		if Desc.Flipbook then E:PlayFlipbook(Desc.Flipbook) end
		self.Fx[#self.Fx + 1] = { Entity = E, Life = Desc.Life, Max = Desc.Life, Fade = Desc.Fade, Color = C }
	end)
end

function HD2DGame:UpdateFx(Dt)
	local Keep = {}
	for _, F in ipairs(self.Fx) do
		F.Life = F.Life - Dt
		if F.Life <= 0 or not F.Entity:IsValid() then
			if F.Entity:IsValid() then F.Entity:Destroy() end
		else
			if F.Fade then
				local C = F.Color
				F.Entity:GetComponent("SpriteComponent").Color = Vector4(C[1], C[2], C[3], C[4] * (F.Life / F.Max))
			end
			Keep[#Keep + 1] = F
		end
	end
	self.Fx = Keep
end

-- ---- 화면 흔들림 (카메라는 플레이어 스크립트가 놓으며 이 오프셋을 더한다)
function HD2DGame:AddShake(Amount, Time)
	self.Shake = math.max(self.Shake, Amount)
	self.ShakeTime = math.max(self.ShakeTime, Time)
end

function HD2DGame:GetShakeOffset()
	if self.ShakeTime <= 0 then return Vector3(0, 0, 0) end
	local A = self.Shake * math.min(1.0, self.ShakeTime / 0.15)
	return Vector3(math.sin(self.Time * 91.0) * A, 0, math.cos(self.Time * 73.0) * A)
end

function HD2DGame:OnUpdate(Dt)
	self.Time = self.Time + Dt
	self.ShakeTime = math.max(0.0, self.ShakeTime - Dt)
	self:UpdateFx(Dt)
	self:UpdateRespawns(Dt)
	if self.Properties.AutoPlay and not self.bReported and self.Time >= self.Properties.AutoPlaySeconds then
		self.bReported = true
		self:ReportAutoPlay()
	end
end

-- 자동 플레이 결과: 이동·공격·대시·처치·부활·피격 경로를 한 번씩은 지났는지
function HD2DGame:ReportAutoPlay()
	local P = self:GetPlayer()
	local Stats = P and P.Stats or {}
	local Failures = {}
	local function Expect(Cond, What)
		if not Cond then Failures[#Failures + 1] = What end
	end
	Expect(P ~= nil, "플레이어 없음")
	Expect((Stats.Distance or 0) > 1500, "이동 거리 부족 " .. math.floor(Stats.Distance or 0))
	Expect((Stats.Attacks or 0) >= 4, "공격 횟수 부족 " .. (Stats.Attacks or 0))
	Expect((Stats.Hits or 0) >= 3, "명중 부족 " .. (Stats.Hits or 0))
	Expect((Stats.Dashes or 0) >= 2, "대시 횟수 부족 " .. (Stats.Dashes or 0))
	Expect(self.Kills >= 2, "처치 부족 " .. self.Kills)
	Log.Info(string.format("[HD2D] 자동 플레이: 이동 %.0fcm, 공격 %d(명중 %d), 대시 %d, 처치 %d, 피격 %d, 체력 %d",
		Stats.Distance or 0, Stats.Attacks or 0, Stats.Hits or 0, Stats.Dashes or 0, self.Kills, Stats.Damaged or 0, P and P.Health or 0))
	if #Failures == 0 then
		Log.Info("[HD2D] 결과: 실패 0건")
	else
		Log.Error("[HD2D] 결과: 실패 " .. #Failures .. "건 — " .. table.concat(Failures, ", "))
	end
end

return HD2DGame
