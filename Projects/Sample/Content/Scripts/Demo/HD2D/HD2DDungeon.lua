-- HD-2D 데모 관리자 확장 ③ 던전 장치 (HD2DGame.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 관리자 self에). 동굴 유적(HD2DCave)이 쓴다.
--   가시 함정판: 관리자 속성 Traps = "x,y,z,반폭X,반폭Y;..." — 씬의 "Trap_<번호>" 가시 묶음(BuildHD2DCave.py가 솟은 자리로 둔다)을 주기마다
--     내림(안전) → 경고(판 위 붉은 판 깜빡임) → 솟음(판 위면 피해). 주기는 TrapCycle (판마다 조금씩 늦어 물결처럼 솟는다). 게임 시간 — 메뉴로 멈춘다.
--   보스 방 문: Gate = "x,y,z,창살 이동", Arena = "x,y,반지름". 보스가 살아 있을 때 플레이어가 문 안쪽(+X)으로 보스 방에 들어서면 CaveGate 프리팹을
--     세우고 창살을 올려 닫는다(루트 상자 콜라이더가 길목을 막음). 보스를 쓰러뜨리면(OnDungeonBossKilled) 창살을 내리고 지운다.
--     쓰러져 입구로 돌아가는 등 플레이어가 문 바깥에 있으면 다시 연다 (갇혀서 못 들어가지 않게).
--   수정 가시 분출(수정 거미 여왕 패턴): SpawnEruption(자리, 지연, 반지름, 피해) — 바닥 경고 원 → 지연 뒤 수정 가시가 솟아 반지름 안이면 피해.
local Dungeon = {}

local function Flat(V) return Vector3(V.X, V.Y, 0) end

-- 함정 주기 (초): 0 ~ Warn 내려가 있음(안전), Warn ~ Up 경고, Up ~ Period 솟음. 판 번호마다 Stagger초씩 늦다
Dungeon.TrapCycle = { Period = 3.4, Warn = 1.9, Up = 2.7, Stagger = 0.12, Sink = 34.0, Damage = 16 }

function Dungeon:InitDungeon()
	self.Traps = {}
	self.Eruptions = {}
	for X, Y, Z, HX, HY in string.gmatch(self.Properties.Traps, "([-%d%.]+),([-%d%.]+),([-%d%.]+),([-%d%.]+),([-%d%.]+)") do
		local Index = #self.Traps + 1
		local T = { Index = Index, Pos = Vector3(tonumber(X), tonumber(Y), tonumber(Z)), HX = tonumber(HX), HY = tonumber(HY),
		            Offset = (Index - 1) * Dungeon.TrapCycle.Stagger, State = "Down", Lift = -Dungeon.TrapCycle.Sink }
		T.Entity = Scene.Find("Trap_" .. Index)
		if T.Entity then
			T.Base = T.Entity:GetWorldPosition()
			T.Entity:SetPosition(T.Base + Vector3(0, 0, T.Lift))
		else
			Log.Warn("[HD2D] 함정 가시 묶음이 없음: Trap_" .. Index)
		end
		self.Traps[Index] = T
	end
	if #self.Traps > 0 then
		self.TrapCenterX = (self.Traps[1].Pos.X + self.Traps[#self.Traps].Pos.X) * 0.5
	end
	if self.Properties.Gate ~= "" then
		local X, Y, Z, Travel = string.match(self.Properties.Gate, "([-%d%.]+),([-%d%.]+),([-%d%.]+),([-%d%.]+)")
		self.Gate = { Pos = Vector3(tonumber(X), tonumber(Y), tonumber(Z)), Travel = tonumber(Travel), State = "Open", T = 0 }
	end
	if self.Properties.Arena ~= "" then
		local X, Y, R = string.match(self.Properties.Arena, "([-%d%.]+),([-%d%.]+),([-%d%.]+)")
		self.Arena = { Pos = Vector3(tonumber(X), tonumber(Y), 0), Radius = tonumber(R) }
	end
end

-- 판 하나의 지금 상태와 그 상태 안에서 지난 시간: "Down" | "Warn" | "Up"
function Dungeon:TrapState(T, Time)
	local C = Dungeon.TrapCycle
	local Phase = ((Time or self.Time) - T.Offset) % C.Period
	if Phase < C.Warn then return "Down", Phase end
	if Phase < C.Up then return "Warn", Phase - C.Warn end
	return "Up", Phase - C.Up
end

-- 플레이어 발이 판 위인가
function Dungeon:OnTrap(T, Pos)
	return math.abs(Pos.X - T.Pos.X) <= T.HX + 22 and math.abs(Pos.Y - T.Pos.Y) <= T.HY and math.abs(Pos.Z - 85 - T.Pos.Z) < 70
end

function Dungeon:UpdateTraps(Dt, Player, PP)
	local C = Dungeon.TrapCycle
	for _, T in ipairs(self.Traps) do
		local State, Age = self:TrapState(T)
		if State ~= T.State then
			T.State = State
			if State == "Warn" then
				-- 판 위 붉은 경고 판 (깜빡임은 아래에서)
				T.Warn = self:SpawnSprite({ Sprite = "Sprites/HD2D/CaveFx.esprite", Slice = "WarnTile", Position = T.Pos + Vector3(0, 0, 4), Flat = true, Blend = 0,
				                            Life = C.Up - C.Warn + 0.05, Scale = { T.HX * 2.0 / 96.0, T.HY * 2.0 / 96.0 }, Color = { 1, 1, 1, 0.85 } })
			elseif State == "Up" then
				self.Report.TrapRises = self.Report.TrapRises + 1
				if PP and Flat(PP - T.Pos):Length() < 1600 then
					if T.Index == 1 then Audio.PlayOneShot("Audio/RPG/Swing3.wav", T.Pos, 0.7, 1.35) end
					self:SpawnFx("Dust", T.Pos + Vector3(0, T.HY * 0.5, 4), { Scale = 0.9 })
				end
			end
		end
		if State ~= "Warn" then
			T.Warn = nil
		elseif T.Warn and T.Warn.Entity and T.Warn.Entity:IsValid() then
			local Blink = math.floor(Age * 12) % 2 == 0
			T.Warn.Entity:GetComponent("SpriteComponent").Color = Vector4(1, 1, 1, Blink and 0.9 or 0.3)
		end
		-- 가시: 솟을 때 빠르게, 내려갈 때 천천히
		local Target = State == "Up" and 0.0 or -C.Sink
		local Step = (Target > T.Lift and 600.0 or 140.0) * Dt
		local New = T.Lift + math.max(-Step, math.min(Step, Target - T.Lift))
		if New ~= T.Lift and T.Entity then
			T.Lift = New
			T.Entity:SetPosition(T.Base + Vector3(0, 0, New))
		end
		if State == "Up" and Player and not Player.bDead and self:OnTrap(T, PP) then
			local Back = PP.X < (self.TrapCenterX or T.Pos.X) and 300 or -300 -- 함정 줄의 가까운 끝 쪽으로 밀어낸다
			if Player:TakeDamage(C.Damage, Vector3(PP.X + Back, PP.Y, PP.Z), { Color = { 1, 0.55, 0.45, 1 } }) then
				self.Report.TrapHits = self.Report.TrapHits + 1
				Log.Info("[HD2D] 가시 함정에 찔림 (판 " .. T.Index .. ")")
			end
		end
	end
end

-- ================================================================ 보스 방 문
function Dungeon:CloseGate()
	local G = self.Gate
	G.State, G.T = "Closing", 0
	self.Report.GateCloses = self.Report.GateCloses + 1
	Scene.SpawnPrefab("Prefabs/Demo/HD2D/CaveGate.eprefab", G.Pos, function(E)
		G.Entity = E
		G.Bars = E:FindChild("Bars")
		G.BarsOpen = G.Bars and G.Bars:GetPosition() or Vector3(0, 0, 0)
	end)
	self:AddShake(10, 0.5)
	Audio.PlayOneShot("Audio/RPG/HitHeavy.wav", G.Pos, 1.0, 0.7)
	for K = -2, 2 do
		self:SpawnFx("Dust", Vector3(G.Pos.X, G.Pos.Y + K * 180, G.Pos.Z - 195), { Scale = 1.6, FlipX = K < 0 })
	end
	self:Hud():Announce("문이 닫혔다!", "보스 방에 갇혔다 — 수정 거미 여왕을 쓰러뜨리자", 2.2)
	Log.Info("[HD2D] 보스 방 문 닫힘")
end

function Dungeon:OpenGate(bVictory)
	local G = self.Gate
	if G.State == "Open" or G.State == "Opening" then return end
	G.State, G.T = "Opening", 0
	self.Report.GateOpens = self.Report.GateOpens + 1
	Audio.PlayOneShot("Audio/RPG/HitHeavy.wav", G.Pos, 0.8, 0.85)
	self:AddShake(6, 0.4)
	if bVictory then
		Timer.After(1.2, function() self:Hud():Announce("문이 열렸다", "보스 방 뒤 제단의 상자도 살펴보자", 2.4) end, { Unscaled = true })
	end
	Log.Info("[HD2D] 보스 방 문 열림" .. (bVictory and " (보스 처치)" or " (플레이어가 바깥)"))
end

function Dungeon:IsGateClosed()
	return self.Gate ~= nil and (self.Gate.State == "Closing" or self.Gate.State == "Closed")
end

function Dungeon:UpdateGate(Dt, PP)
	local G = self.Gate
	if not G then return end
	if G.State == "Open" and PP and self.Arena and not self:IsBossDefeated() then
		-- 문 안쪽(+X, 콜라이더 반폭 + 캡슐 반지름보다 더)으로 보스 방에 들어섰다
		if PP.X > G.Pos.X + 110 and Flat(PP - self.Arena.Pos):Length() < self.Arena.Radius then
			self:CloseGate()
		end
	elseif G.State == "Closed" and PP and PP.X < G.Pos.X - 140 then
		self:OpenGate(false)
	end
	if (G.State == "Closing" or G.State == "Opening") and G.Bars then
		G.T = G.T + Dt
		local Dur = G.State == "Closing" and 0.45 or 0.9
		local A = math.min(1.0, G.T / Dur)
		local Ease = A * A * (3.0 - 2.0 * A)
		local Up = G.State == "Closing" and Ease or (1.0 - Ease)
		G.Bars:SetPosition(G.BarsOpen + Vector3(0, 0, G.Travel * Up))
		if A >= 1.0 then
			if G.State == "Closing" then
				G.State = "Closed"
			else
				G.State = "Open"
				if G.Entity and G.Entity:IsValid() then G.Entity:Destroy() end
				G.Entity, G.Bars = nil, nil
			end
		end
	end
end

function Dungeon:OnDungeonBossKilled()
	if self.Gate and self:IsGateClosed() then self:OpenGate(true) end
end

-- ================================================================ 수정 가시 분출
function Dungeon:SpawnEruption(Pos, Delay, Radius, Damage, HitOpt)
	local Warn = self:SpawnSprite({ Sprite = "Sprites/HD2D/Fx.esprite", Slice = "Warn", Position = Pos + Vector3(0, 0, 3), Flat = true, Blend = 0,
	                                Life = Delay + 0.05, Scale = Radius / 108.0, Color = { 0.7, 0.85, 1.4, 0.9 } })
	self.Eruptions[#self.Eruptions + 1] = { Pos = Pos, Delay = Delay, Radius = Radius, Damage = Damage, Warn = Warn, HitOpt = HitOpt }
end

function Dungeon:UpdateEruptions(Dt, Player, PP)
	local Keep = {}
	for _, R in ipairs(self.Eruptions) do
		R.Delay = R.Delay - Dt
		if R.Delay <= 0 then
			self:SpawnSprite({ Sprite = "Sprites/HD2D/CaveFx.esprite", Flipbook = "Sprites/HD2D/CaveFx_Shard.eflipbook", Position = R.Pos + Vector3(0, 6, 0),
			                   Blend = 3, Lit = false, Life = 0.85, Scale = 1.4 * R.Radius / 110.0 })
			self:SpawnFx("Spark", R.Pos + Vector3(0, 20, 60), { Blend = 2, Scale = 1.6, Color = { 0.6, 0.85, 1, 1 } })
			self:SpawnFx("Dust", R.Pos + Vector3(0, 8, 2), { Scale = 1.2 })
			self.Report.Eruptions = self.Report.Eruptions + 1
			if PP and Flat(PP - R.Pos):Length() < 1800 then
				Audio.PlayOneShot("Audio/RPG/HitHeavy.wav", R.Pos, 0.55, 1.6)
			end
			if Player and not Player.bDead and Flat(PP - R.Pos):Length() < R.Radius then
				Player:TakeDamage(R.Damage, R.Pos, { Color = { 0.6, 0.85, 1, 1 }, Status = R.HitOpt and R.HitOpt.Status, Chance = R.HitOpt and R.HitOpt.Chance })
			end
		else
			Keep[#Keep + 1] = R
		end
	end
	self.Eruptions = Keep
end

function Dungeon:UpdateDungeon(Dt)
	if #self.Traps == 0 and not self.Gate and #self.Eruptions == 0 then return end
	local Player = self:GetPlayer()
	local PP = Player and Player.entity:GetWorldPosition() or nil
	self:UpdateTraps(Dt, Player, PP)
	self:UpdateGate(Dt, PP)
	self:UpdateEruptions(Dt, Player, PP)
end

return Dungeon
