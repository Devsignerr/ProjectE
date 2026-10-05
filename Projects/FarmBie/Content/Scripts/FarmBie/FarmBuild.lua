-- FarmBie 하우징·설치물 (FarmGame에 섞이는 메서드 모음). 설치물 정의는 Data/FarmBie/Buildables.etable, 크리스탈 단계는 CrystalLevels.etable.
--   건설 모드(Build 입력 B): 핫바 자리에 건설 막대(BuildBar) — 1~9로 고르고, 도구 사용 = 빈 칸에 설치 / 설치물이 있으면 철거(재료 절반 돌려받음),
--     R = 벽·문 방향(0/90도), E = 수리(낮에만, 잃은 내구도 비율만큼 재료) — 수리는 건설 모드 밖에서도 발 앞 설치물에 E
--   설치물 = 프리팹 Prefabs/FarmBie/Build_<종류>.eprefab (FarmStructureComponent + 모델 [+ 충돌]) — 칸·내구도를 Lua가 채우고 C++ 디펜스(F7)가 읽는다.
--     디펜스가 내구도 0에서 Destroyed를 켜면 UpdateStructures가 엔티티를 지우고 칸을 비운다
--   크리스탈: 하나, 낮에만 E로 들어 옮김(들고 있으면 머리 위) → 빈 칸에서 E로 내려놓음. 건설 막대의 "크리스탈 강화"를 들고 크리스탈에 도구 사용 = 다음 단계
--   온실: 고정 부지(FarmMap.Greenhouse)에서 E로 짓기 → 그 칸들의 작물은 계절이 바뀌어도 시들지 않는다(IsGreenhouseTile)
--   저장: 설치물 목록(종류·칸·방향·내구도) + 크리스탈(칸·단계·내구도) + 온실 여부. 그림은 농장 맵에서만 만든다
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Build = {}

local Prefabs = "Prefabs/FarmBie/"
local BarIds = { "WallWood", "WallStone", "WallIron", "Gate", "Spike", "Mine", "Turret", "Special", "CrystalUp" }
local SpecialIds = { "SkullMine", "ScreamMine", "FangSpike" } -- 작업대에서 만든 작물 덫 (가진 것 먼저)

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function Build:InitBuild()
	self.Structures = {}       -- 칸 번호 → { Id, TX, TY, Rot, Hp, Entity, Comp }
	self.BuildMode = false
	self.BuildIndex = 1
	self.BuildRot = 0
	self.Crystal = self.Crystal or nil
	self.bGreenhouse = false
	self.GreenhouseTiles = {}
	if self.MapId ~= "Farm" then return end
	local M = self.Map
	local G = M.Greenhouse
	local CX = M.OriginX + (G[1] + G[3] * 0.5) * M.Tile
	local CY = M.OriginY + (G[2] + G[4] * 0.5) * M.Tile
	self.GreenhouseCenter = Vector3(CX, CY, 0)
	self:AddInteractable({ Pos = Vector3(CX, CY + G[4] * 50 + 40, 0), Radius = 260, Prompt = function()
		if self.bGreenhouse or self.BuildMode then return nil end
		return string.format("E  온실 짓기 (%s, %d골드)", self:CostText(M.GreenhouseCost), M.GreenhouseGold)
	end, Act = function() self:BuildGreenhouse() end })
	-- 크리스탈 들기 (낮)
	self:AddInteractable({ Pos = Vector3(0, 0, 0), Radius = 170, Dynamic = "Crystal", Prompt = function()
		if self.CarryingCrystal or self.BuildMode or self.Phase ~= "Day" or not self.Crystal then return nil end
		return "E  크리스탈 들기 (옮기기)"
	end, Act = function() self:PickUpCrystal() end })
end

-- ---- 재료
function Build:ParseCost(Text)
	local List = {}
	for Key, Count in string.gmatch(Text or "", "([%w]+)%*(%d+)") do List[#List + 1] = { Key = Key, Count = tonumber(Count) } end
	return List
end

function Build:CostText(Text, Scale)
	local Parts = {}
	for _, C in ipairs(self:ParseCost(Text)) do
		local N = math.ceil(C.Count * (Scale or 1) - 1e-6)
		if N > 0 then Parts[#Parts + 1] = string.format("%s %d", self:ItemInfo(C.Key).Name, N) end
	end
	return table.concat(Parts, ", ")
end

function Build:HasCost(Text, Scale)
	for _, C in ipairs(self:ParseCost(Text)) do
		if self:CountItem(C.Key) < math.ceil(C.Count * (Scale or 1) - 1e-6) then return false end
	end
	return true
end

function Build:PayCost(Text, Scale)
	if not self:HasCost(Text, Scale) then return false end
	for _, C in ipairs(self:ParseCost(Text)) do
		local N = math.ceil(C.Count * (Scale or 1) - 1e-6)
		if N > 0 then self:Take(C.Key, N) end
	end
	return true
end

-- ---- 건설 막대
function Build:BuildEntry(Index)
	local Id = BarIds[Index]
	if not Id then return nil end
	if Id == "Special" then
		Id = SpecialIds[1]
		for _, S in ipairs(SpecialIds) do
			if self:CountItem(S) > 0 then Id = S break end
		end
	end
	if Id == "CrystalUp" then
		local Next = self.Crystal and D.Rows("CrystalLevels.etable")[self.Crystal.Level + 1]
		return { Id = Id, Name = "크리스탈 강화", Icon = "UI/FarmBie/Icons/CrystalUp.png",
		         Cost = Next and Next.Cost or "", Gold = Next and Next.Gold or 0, bMax = Next == nil }
	end
	local Row = D.ByName("Buildables.etable")[Id]
	return { Id = Id, Name = Row.DisplayName, Icon = "UI/FarmBie/Icons/" .. Id .. ".png", Cost = Row.Cost, Row = Row, Gold = 0 }
end

function Build:ToggleBuildMode()
	if self.MapId ~= "Farm" or self.CarryingCrystal then return end
	self.BuildMode = not self.BuildMode
	self:Hud():Show("BuildTitle", self.BuildMode)
	local E = self:BuildEntry(self.BuildIndex)
	self:Hud():ShowItemName(self.BuildMode and (E.Name .. "  —  " .. self:CostText(E.Cost)) or "")
	self.Report.BuildModes = (self.Report.BuildModes or 0) + 1
end

function Build:SelectBuild(Index)
	Index = ((Index - 1) % #BarIds) + 1
	self.BuildIndex = Index
	local E = self:BuildEntry(Index)
	local Cost = E.Id == "CrystalUp" and (E.bMax and "최고 단계" or (self:CostText(E.Cost) .. string.format(", %d골드", E.Gold))) or self:CostText(E.Cost)
	self:Hud():ShowItemName(E.Name .. "  —  " .. Cost)
end

-- ---- 칸 판정
function Build:StructureAt(TX, TY)
	return TX and self.Structures[self:TileIndex(TX, TY)] or nil
end

function Build:IsCrystalTile(TX, TY)
	return self.Crystal ~= nil and not self.CarryingCrystal and self.Crystal.TX == TX and self.Crystal.TY == TY
end

function Build:InGreenhouseSite(TX, TY)
	local G = self.Map.Greenhouse
	return TX >= G[1] and TX < G[1] + G[3] and TY >= G[2] and TY < G[2] + G[4]
end

function Build:CanPlaceAt(TX, TY)
	if not TX or self.MapId ~= "Farm" then return false, "" end
	local M = self.Map
	local C = self:TileCenter(TX, TY)
	if C.X < M.FarmMinX or C.X > M.FarmMaxX or C.Y < M.FarmMinY or C.Y > M.FarmMaxY then return false, "여기는 지을 수 없다" end
	if self:StructureAt(TX, TY) or self:IsCrystalTile(TX, TY) then return false, "" end
	if self:GetTile(TX, TY) then return false, "밭 위에는 지을 수 없다" end
	if self:InGreenhouseSite(TX, TY) then return false, "온실 부지다" end
	local Hits = Physics.OverlapBox(Vector3(C.X, C.Y, 95), Vector3(44, 44, 60))
	if #Hits > 0 then return false, "무언가 막고 있다" end
	return true, ""
end

-- 건설 모드에서 대상 칸에 할 일 → "Place" | "Demolish" | "Upgrade" | nil, 이유
function Build:PlanBuild(TX, TY)
	if not TX then return nil, "" end
	local E = self:BuildEntry(self.BuildIndex)
	if E.Id == "CrystalUp" then
		if not self:IsCrystalTile(TX, TY) then return nil, "크리스탈을 향해 쓰자" end
		if E.bMax then return nil, "이미 최고 단계" end
		if not self:HasCost(E.Cost) or self.Gold < E.Gold then return nil, "재료나 돈이 모자라다" end
		return "Upgrade"
	end
	local S = self:StructureAt(TX, TY)
	if S then return "Demolish" end
	local bOk, Why = self:CanPlaceAt(TX, TY)
	if not bOk then return nil, Why end
	if not self:HasCost(E.Cost) then return nil, "재료가 모자라다 (" .. self:CostText(E.Cost) .. ")" end
	return "Place"
end

function Build:ApplyBuild(TX, TY)
	local Action, Why = self:PlanBuild(TX, TY)
	if not Action then
		if Why and Why ~= "" then self:Hud():Toast("", Why, { 1.0, 0.75, 0.55, 1.0 }) end
		return false
	end
	local E = self:BuildEntry(self.BuildIndex)
	if Action == "Place" then
		self:PayCost(E.Cost)
		self:PlaceStructure(E.Id, TX, TY, (E.Row.Kind == "Wall" or E.Row.Kind == "Gate") and self.BuildRot or 0, E.Row.Hp)
		self.Report.Built = (self.Report.Built or 0) + 1
		self:Sfx("Equip", 0.7, 0.8)
	elseif Action == "Demolish" then
		local S = self:StructureAt(TX, TY)
		local Row = D.ByName("Buildables.etable")[S.Id]
		for _, C in ipairs(self:ParseCost(Row.Cost)) do
			local N = math.floor(C.Count * 0.5)
			if N > 0 then self:Give(C.Key, N, true) end
		end
		self:RemoveStructure(TX, TY)
		self.Report.Demolished = (self.Report.Demolished or 0) + 1
		self:Hud():Toast(E.Icon, Row.DisplayName .. " 철거 (재료 절반 돌려받음)")
	elseif Action == "Upgrade" then
		self:PayCost(E.Cost)
		self.Gold = self.Gold - E.Gold
		self:SetCrystalLevel(self.Crystal.Level + 1, true)
		self.Report.CrystalUps = (self.Report.CrystalUps or 0) + 1
		self:Sfx("Bell", 0.6, 1.3)
		self:Hud():Announce("크리스탈 강화!", string.format("%d단계 — 내구도 %d, 감지 반경 %dcm", self.Crystal.Level, self:CrystalRow().MaxHp, self:CrystalRow().DetectRadius), 3.0)
		self:SelectBuild(self.BuildIndex)
	end
	return true
end

-- ---- 설치물 엔티티
function Build:PlaceStructure(Id, TX, TY, Rot, Hp)
	local Index = self:TileIndex(TX, TY)
	local S = { Id = Id, TX = TX, TY = TY, Rot = Rot or 0, Hp = Hp }
	self.Structures[Index] = S
	self:SpawnStructure(S)
	if self.OnStructuresChanged then self:OnStructuresChanged() end
	return S
end

function Build:SpawnStructure(S)
	if self.MapId ~= "Farm" then return end
	local C = self:TileCenter(S.TX, S.TY)
	local Z = self:GroundZ(C)
	local Row = S.Id ~= "Crystal" and D.ByName("Buildables.etable")[S.Id] or nil
	S.bSpawning = true
	Scene.SpawnPrefab(Prefabs .. "Build_" .. S.Id .. ".eprefab", Vector3(C.X, C.Y, Z), function(E)
		S.bSpawning = false
		if S.bRemoved then
			Scene.Destroy(E)
			return
		end
		S.Entity = E
		S.Comp = E:GetComponent("FarmStructureComponent")
		S.Comp.TX, S.Comp.TY = S.TX, S.TY
		S.Comp.MaxHp = S.MaxHp or (Row and Row.Hp) or S.Comp.MaxHp
		S.Comp.Hp = S.Hp or S.Comp.MaxHp
		if Row then
			S.Comp.Damage, S.Comp.Radius, S.Comp.Cooldown, S.Comp.Range = Row.Damage, Row.Radius, Row.Cooldown, Row.Range
			if S.Id == "SkullMine" or S.Id == "ScreamMine" then S.Comp.Kind = "Mine" end -- 디펜스는 지뢰로 다룬다
			if S.Id == "FangSpike" then S.Comp.Kind = "Spike" end
		end
		if S.Rot ~= 0 then E:SetRotation(Quat.FromEuler(0, S.Rot, 0)) end
	end)
end

function Build:RemoveStructure(TX, TY)
	local Index = self:TileIndex(TX, TY)
	local S = self.Structures[Index]
	if not S then return end
	self.Structures[Index] = nil
	S.bRemoved = true
	if S.Entity then Scene.Destroy(S.Entity) end
	if self.OnStructuresChanged then self:OnStructuresChanged() end
end

-- 매 프레임: 디펜스가 부순 설치물 정리, 내구도 따라가기, 들고 있는 크리스탈
function Build:UpdateStructures()
	if self.MapId ~= "Farm" then return end
	for Index, S in pairs(self.Structures) do
		if S.Comp then
			S.Hp = S.Comp.Hp
			if S.Comp.Destroyed then
				self.Report.StructuresLost = (self.Report.StructuresLost or 0) + 1
				Log.Info(string.format("[FarmBie] 설치물 부서짐: %s (%d,%d)", S.Id, S.TX, S.TY))
				self:RemoveStructure(S.TX, S.TY)
			end
		end
	end
	local Cr = self.Crystal
	if Cr and Cr.Comp then
		Cr.Hp = Cr.Comp.Hp
		if self.CarryingCrystal and Cr.Entity then
			local P = self:Player()
			if P then Cr.Entity:SetPosition(P.entity:GetWorldPosition() + Vector3(0, 10, 95)) end
		end
	end
end

-- ---- 수리 (낮, 발 앞 설치물) — FarmGame:UpdateInteract가 상호작용 대상이 없을 때 묻는다
function Build:RepairPlan(TX, TY)
	local S = self:StructureAt(TX, TY)
	if not S or self.Phase ~= "Day" then return nil end
	local Row = D.ByName("Buildables.etable")[S.Id]
	local MaxHp = S.Comp and S.Comp.MaxHp or Row.Hp
	local Missing = 1 - (S.Hp or MaxHp) / MaxHp
	if Missing <= 0.001 then return nil end
	return S, Row, Missing
end

function Build:RepairPrompt(TX, TY)
	local S, Row, Missing = self:RepairPlan(TX, TY)
	if not S then return nil end
	return string.format("E  수리: %s (%s)", Row.DisplayName, self:CostText(Row.Cost, Missing))
end

function Build:Repair(TX, TY)
	local S, Row, Missing = self:RepairPlan(TX, TY)
	if not S then return false end
	if not self:PayCost(Row.Cost, Missing) then
		self:Hud():Toast("", "수리 재료가 모자라다 (" .. self:CostText(Row.Cost, Missing) .. ")", { 1.0, 0.6, 0.5, 1.0 })
		return false
	end
	S.Hp = S.Comp and S.Comp.MaxHp or Row.Hp
	if S.Comp then S.Comp.Hp = S.Hp end
	self.Report.Repaired = (self.Report.Repaired or 0) + 1
	self:Hud():Toast("UI/FarmBie/Icons/" .. S.Id .. ".png", Row.DisplayName .. " 수리 완료")
	return true
end

-- ---- 크리스탈
function Build:CrystalRow()
	return D.Rows("CrystalLevels.etable")[self.Crystal.Level]
end

function Build:SetCrystalLevel(Level, bHeal)
	self.Crystal.Level = Level
	local Row = self:CrystalRow()
	if bHeal then self.Crystal.Hp = Row.MaxHp end
	if self.Crystal.Comp then
		self.Crystal.Comp.MaxHp = Row.MaxHp
		self.Crystal.Comp.Hp = math.min(self.Crystal.Hp, Row.MaxHp)
	end
end

function Build:SpawnCrystal()
	local Cr = self.Crystal
	if self.MapId ~= "Farm" or not Cr then return end
	local C = self:TileCenter(Cr.TX, Cr.TY)
	Scene.SpawnPrefab(Prefabs .. "Build_Crystal.eprefab", Vector3(C.X, C.Y, self:GroundZ(C)), function(E)
		Cr.Entity = E
		Cr.Comp = E:GetComponent("FarmStructureComponent")
		Cr.Comp.TX, Cr.Comp.TY = Cr.TX, Cr.TY
		self:SetCrystalLevel(Cr.Level, false)
	end)
end

function Build:EnsureCrystal()
	if not self.Crystal then
		local S = self.Map.CrystalStart
		self.Crystal = { TX = S[1], TY = S[2], Level = 1, Hp = D.Rows("CrystalLevels.etable")[1].MaxHp }
	end
	self:SpawnCrystal()
end

function Build:CrystalPos()
	local Cr = self.Crystal
	return Cr and self:TileCenter(Cr.TX, Cr.TY) or nil
end

function Build:PickUpCrystal()
	if self.Phase ~= "Day" or not self.Crystal or not self.Crystal.Comp then return end
	self.CarryingCrystal = true
	self.Crystal.Comp.Blocks = false
	local Col = self.Crystal.Entity:FindChild("Collision")
	if Col and Col:HasComponent("BoxColliderComponent") then Col:RemoveComponent("BoxColliderComponent") end
	self:Hud():Toast("UI/FarmBie/Icons/CrystalUp.png", "크리스탈을 들었다 — 내려놓을 빈 칸에서 E")
	if self.OnStructuresChanged then self:OnStructuresChanged() end
end

function Build:PlaceCrystal(TX, TY)
	local bOk, Why = self:CanPlaceAt(TX, TY)
	if not bOk then
		self:Hud():Toast("", Why ~= "" and Why or "여기에는 놓을 수 없다", { 1.0, 0.7, 0.5, 1.0 })
		return false
	end
	local Cr = self.Crystal
	Cr.TX, Cr.TY = TX, TY
	local C = self:TileCenter(TX, TY)
	Cr.Entity:SetPosition(Vector3(C.X, C.Y, self:GroundZ(C)))
	Cr.Comp.TX, Cr.Comp.TY = TX, TY
	Cr.Comp.Blocks = true
	local Col = Cr.Entity:FindChild("Collision")
	if Col and not Col:HasComponent("BoxColliderComponent") then
		Col:AddComponent("BoxColliderComponent")
		Col:GetComponent("BoxColliderComponent").HalfExtents = Vector3(40, 40, 70)
	end
	self.CarryingCrystal = false
	self.Report.CrystalMoves = (self.Report.CrystalMoves or 0) + 1
	self:Hud():Toast("UI/FarmBie/Icons/CrystalUp.png", "크리스탈을 내려놓았다")
	if self.OnStructuresChanged then self:OnStructuresChanged() end
	return true
end

-- ---- 온실
function Build:BuildGreenhouse()
	local M = self.Map
	if self.bGreenhouse then return end
	if not self:HasCost(M.GreenhouseCost) or self.Gold < M.GreenhouseGold then
		self:Hud():Toast("", "재료나 돈이 모자라다 (" .. self:CostText(M.GreenhouseCost) .. ", " .. M.GreenhouseGold .. "골드)", { 1.0, 0.6, 0.5, 1.0 })
		return
	end
	self:PayCost(M.GreenhouseCost)
	self.Gold = self.Gold - M.GreenhouseGold
	self:SetGreenhouse(true)
	self:Hud():Announce("온실 완성!", "온실 안 작물은 계절이 바뀌어도 시들지 않는다", 3.0)
end

function Build:SetGreenhouse(bBuilt)
	self.bGreenhouse = bBuilt
	self.GreenhouseTiles = {}
	if not bBuilt then return end
	local G = self.Map.Greenhouse
	for X = G[1], G[1] + G[3] - 1 do
		for Y = G[2], G[2] + G[4] - 1 do self.GreenhouseTiles[self:TileIndex(X, Y)] = true end
	end
	if self.MapId == "Farm" and not self.GreenhouseEntity then
		local C = self.GreenhouseCenter
		Scene.SpawnPrefab(Prefabs .. "Build_Greenhouse.eprefab", Vector3(C.X, C.Y, self:GroundZ(C)), function(E) self.GreenhouseEntity = E end)
		for I = 0, 3 do
			local Stake = Scene.Find("GreenhouseStake_" .. I)
			if Stake then Stake:SetScale(Vector3(0.001, 0.001, 0.001)) end
		end
	end
end

-- ---- 저장 조각
function Build:SaveBuild(T)
	local List = {}
	for _, S in pairs(self.Structures) do List[#List + 1] = { Id = S.Id, TX = S.TX, TY = S.TY, Rot = S.Rot, Hp = S.Hp } end
	T.Structures = List
	local Cr = self.Crystal
	T.Crystal = Cr and { TX = Cr.TX, TY = Cr.TY, Level = Cr.Level, Hp = Cr.Hp } or nil
	T.Greenhouse = self.bGreenhouse
end

function Build:LoadBuild(T)
	for _, S in pairs(self.Structures) do
		S.bRemoved = true
		if S.Entity then Scene.Destroy(S.Entity) end
	end
	self.Structures = {}
	for _, S in ipairs(T.Structures or {}) do
		self:PlaceStructure(S.Id, math.floor(S.TX), math.floor(S.TY), S.Rot or 0, S.Hp)
	end
	if self.Crystal and self.Crystal.Entity then Scene.Destroy(self.Crystal.Entity) end
	self.Crystal = nil
	if T.Crystal then
		self.Crystal = { TX = math.floor(T.Crystal.TX), TY = math.floor(T.Crystal.TY), Level = math.floor(T.Crystal.Level or 1), Hp = T.Crystal.Hp }
	end
	self.CarryingCrystal = false
	if self.MapId == "Farm" then self:EnsureCrystal() end
	self:SetGreenhouse(T.Greenhouse == true)
end

return Build
