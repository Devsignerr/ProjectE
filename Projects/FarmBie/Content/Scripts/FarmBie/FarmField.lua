-- FarmBie 밭 (FarmGame에 섞이는 메서드 모음). 농장 격자(Data/FarmBie/FarmMap.edata — 생성기 상수와 같음) 위의 간 칸·작물.
--   칸 self.Tiles[번호] = { TX, TY, Wet, Fert(0 없음/1 기본/2 고급), Crop = { Id, R(희귀도), Age(물 준 날 수), Dead } | nil, Soil, Plant, FertMark(엔티티) }
--   도구 (발 앞 대상 칸 — TargetTile): 괭이 = 갈기 / 시든 작물 걷기, 물뿌리개 = 물 주기(우물에서 채움), 씨앗 = 심기(계절 맞을 때), 비료 = 뿌리기(심기 전·막 심은 칸)
--   수확: 다 자란 작물에 상호작용(E) 또는 도구 사용 → 작물 + 확률 씨앗(한 단계 위 UpChance×비료, 같은 단계 SeedReturnChance, 계절 전용 희귀종 ExclusiveChance×비료)
--   하루: 아침마다 물 준 칸만 Age +1 → 물 마름. 계절이 바뀌면 온실 밖 작물은 시든다(IsGreenhouseTile — F6 하우징이 채움)
--   그림: 흙 칸 = 바닥에 눕힌 스프라이트(Field.esprite), 작물 = 전체 빌보드 스프라이트(Crops.esprite) — Scene.Create로 즉시 만든다
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Field = {}

local Sprites = "Sprites/FarmBie/"
local FlatRot = Quat(-0.70710678, 0, 0, 0.70710678) -- 스프라이트 평면(XZ)을 바닥(XY)에
local SeasonIds = { "Spring", "Summer", "Autumn", "Winter" }

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function Field:InitField()
	local M = D.Values("FarmMap.edata")
	self.Map = M
	self.Farming = D.Values("Farming.edata")
	self.Tiles = {}
	self.Cursor = Scene.Find("TileCursor")
	self.CursorSprite = self.Cursor and self.Cursor:GetComponent("SpriteComponent")
	if self.Properties.Map ~= "Farm" then return end -- 밭 그림·우물은 농장에서만 (밭 상태는 저장으로 이어짐)
	local Well = Vector3(M.Well[1], M.Well[2], 0)
	self:AddInteractable({ Pos = Well, Radius = 300, Prompt = function()
		if self:CountItem("Can") > 0 and self.Water < self.Farming.CanCapacity then return "E  물뿌리개 채우기" end
		return nil
	end, Act = function()
		self.Water = self.Farming.CanCapacity
		self.Report.Refills = (self.Report.Refills or 0) + 1
		self:Hud():Toast("UI/FarmBie/Icons/Can.png", "물뿌리개를 가득 채웠다")
	end })
end

-- ---- 격자
function Field:TileIndex(TX, TY)
	return TY * self.Map.Width + TX
end

function Field:TileOf(Pos)
	local M = self.Map
	local TX = math.floor((Pos.X - M.OriginX) / M.Tile)
	local TY = math.floor((Pos.Y - M.OriginY) / M.Tile)
	if TX < 0 or TY < 0 or TX >= M.Width or TY >= M.Height then return nil end
	return TX, TY
end

function Field:TileCenter(TX, TY)
	local M = self.Map
	return Vector3(M.OriginX + (TX + 0.5) * M.Tile, M.OriginY + (TY + 0.5) * M.Tile, 0)
end

function Field:GetTile(TX, TY)
	return self.Tiles[self:TileIndex(TX, TY)]
end

-- 발 앞 대상 칸
function Field:TargetTile(Pos, Facing)
	return self:TileOf(Pos + Facing * self.Farming.ReachDistance)
end

function Field:GroundZ(Center)
	local Hit = Physics.Raycast(Vector3(Center.X, Center.Y, 600), Vector3(0, 0, -1), 1200)
	return Hit and Hit.position.Z or 0
end

-- 갈 수 있는 칸: 울타리 안 + 건물·소품·울타리 콜라이더가 없는 곳
function Field:CanTill(TX, TY)
	local M = self.Map
	local C = self:TileCenter(TX, TY)
	if C.X < M.FarmMinX or C.X > M.FarmMaxX or C.Y < M.FarmMinY or C.Y > M.FarmMaxY then return false end
	if self:GetTile(TX, TY) then return false end
	if self.StructureAt and (self:StructureAt(TX, TY) or self:IsCrystalTile(TX, TY) or self:InGreenhouseSite(TX, TY) and not self.bGreenhouse) then return false end
	-- 놀이 영역 안 지형은 평평(±10cm) — 높이 0 기준 상자 (지면 레이캐스트는 지붕 콜라이더에 먼저 맞을 수 있다)
	local P = self:Player()
	local Hits = Physics.OverlapBox(Vector3(C.X, C.Y, 95), Vector3(44, 44, 60), nil, P and P.entity or nil)
	return #Hits == 0
end

function Field:IsGreenhouseTile(TX, TY)
	return self.GreenhouseTiles ~= nil and self.GreenhouseTiles[self:TileIndex(TX, TY)] == true
end

-- ---- 그림
function Field:MakeSprite(Name, Pos, Sheet, Slice, bFlat)
	local E = Scene.Create(Name)
	E:AddComponent("SpriteComponent")
	local S = E:GetComponent("SpriteComponent")
	S.Sprite = Sprites .. Sheet
	S.Slice = Slice
	S.Blend = bFlat and 0 or 3
	S.AlphaCutoff = 0.5
	S.Lit = true
	S.CastShadows = not bFlat
	S.Billboard = bFlat and 0 or 1
	E:SetPosition(Pos)
	if bFlat then E:SetRotation(FlatRot) end
	return E, S
end

function Field:CropStage(T)
	local Crop = T.Crop
	if Crop.Dead then return "Dead" end
	local Row = D.ByName("Crops.etable")[Crop.Id]
	if Crop.Age >= Row.Days then return "Mature" end
	if Crop.Age == 0 then return "Seed" end
	if Crop.Age < Row.Days * 0.5 then return "Sprout" end
	return "Grow"
end

function Field:RefreshTile(T)
	if self.MapId ~= "Farm" then return end
	local C = self:TileCenter(T.TX, T.TY)
	if not T.Z then T.Z = self:GroundZ(C) end
	local Variant = (T.TX * 7 + T.TY * 3) % 3
	local SoilSlice = (T.Wet and "SoilWet" or "Soil") .. Variant
	if not T.Soil then
		T.Soil, T.SoilSprite = self:MakeSprite("Soil", Vector3(C.X, C.Y, T.Z + 1.2), "Field.esprite", SoilSlice, true)
	end
	T.SoilSprite.Slice = SoilSlice
	-- 비료 표시
	if T.Fert > 0 and not T.FertMark then
		T.FertMark, T.FertSprite = self:MakeSprite("Fert", Vector3(C.X, C.Y, T.Z + 1.6), "Field.esprite", "FertBasic", true)
	end
	if T.FertMark then
		T.FertSprite.Visible = T.Fert > 0
		T.FertSprite.Slice = T.Fert == 2 and "FertPremium" or "FertBasic"
	end
	-- 작물
	if T.Crop then
		local Stage = self:CropStage(T)
		local Slice = ({ Seed = "Seed", Sprout = T.Crop.Id .. "_Sprout", Grow = T.Crop.Id .. "_Grow", Dead = "Withered" })[Stage]
			or (T.Crop.Id .. "_" .. T.Crop.R)
		if not T.Plant then
			-- 칸 가운데보다 살짝 아래(+Y, 카메라 쪽)에 세운다 — 위 칸 작물이 아래 칸 작물을 가리지 않게 정렬
			T.Plant, T.PlantSprite = self:MakeSprite("Crop", Vector3(C.X, C.Y + 18, T.Z + 1.0), "Crops.esprite", Slice, false)
		end
		T.PlantSprite.Slice = Slice
		T.PlantSprite.Visible = true
	elseif T.Plant then
		Scene.Destroy(T.Plant)
		T.Plant, T.PlantSprite = nil, nil
	end
end

function Field:DestroyTileVisuals(T)
	for _, Key in ipairs({ "Soil", "FertMark", "Plant" }) do
		if T[Key] then Scene.Destroy(T[Key]) end
		T[Key] = nil
	end
end

-- ---- 도구 사용
-- 이 물건으로 이 칸에 무엇을 하나 (실행 전 판정 — 커서 색·안내·동작 고르기). 돌려줌: 행동 이름 | nil, 이유 글
function Field:PlanUse(Key, TX, TY)
	if not TX or self.MapId ~= "Farm" then return nil, "" end
	local T = self:GetTile(TX, TY)
	if T and T.Crop and not T.Crop.Dead and self:CropStage(T) == "Mature" then return "Harvest" end
	if not Key then return nil, "" end
	local Info = self:ItemInfo(Key)
	if Key == "Hoe" then
		if T and T.Crop and T.Crop.Dead then return "Clear" end
		if not T and self:CanTill(TX, TY) then return "Till" end
		return nil, T and "" or "여기는 갈 수 없다"
	elseif Key == "Can" then
		if self.Water <= 0 then return nil, "물이 없다 — 우물에서 채우자" end
		if T and not T.Wet then return "Water" end
		return nil, ""
	elseif Info.Kind == "Seed" then
		if not T or T.Crop then return nil, T and "" or "먼저 괭이로 갈자" end
		if Info.Crop.Season ~= SeasonIds[self.Season + 1] then return nil, "이 계절에는 자라지 않는다" end
		return "Plant"
	elseif Info.Kind == "Fertilizer" then
		if T and T.Fert == 0 and (not T.Crop or T.Crop.Age == 0) then return "Fertilize" end
		return nil, ""
	end
	return nil, ""
end

-- 실행 (플레이어 도구 동작의 효과 시점에 부른다). 성공하면 true
function Field:ApplyUse(Key, TX, TY)
	local Action, Why = self:PlanUse(Key, TX, TY)
	if not Action then
		if Why and Why ~= "" then self:Hud():Toast("", Why, { 1.0, 0.75, 0.55, 1.0 }) end
		return false
	end
	local R = self.Report
	local Index = self:TileIndex(TX, TY)
	local T = self.Tiles[Index]
	if Action == "Till" then
		T = { TX = TX, TY = TY, Wet = false, Fert = 0 }
		self.Tiles[Index] = T
		R.Tilled = (R.Tilled or 0) + 1
	elseif Action == "Clear" then
		T.Crop = nil
		T.Fert = 0
	elseif Action == "Water" then
		T.Wet = true
		self.Water = self.Water - 1
		R.Watered = (R.Watered or 0) + 1
	elseif Action == "Plant" then
		local Info = self:ItemInfo(Key)
		self:Take(Key, 1)
		T.Crop = { Id = Info.CropId, R = Info.Rarity, Age = 0, Dead = false }
		R.Planted = (R.Planted or 0) + 1
	elseif Action == "Fertilize" then
		self:Take(Key, 1)
		T.Fert = Key == "FertPremium" and 2 or 1
	elseif Action == "Harvest" then
		self:Harvest(T)
	end
	self:RefreshTile(T)
	if self.SyncCropTiles then self:SyncCropTiles() end
	return true
end

-- 결정적 난수 (저장된 씨앗으로 이어짐 — 자동 검증 재현)
function Field:Random()
	self.RandState = (self.RandState * 1103515245 + 12345) % 2147483648
	return self.RandState / 2147483648
end

function Field:Harvest(T)
	local Crop = T.Crop
	local Row = D.ByName("Crops.etable")[Crop.Id]
	local Rarities = D.Rows("Rarities.etable")
	local F = self.Farming
	local Mul = T.Fert == 2 and F.FertPremiumMul or (T.Fert == 1 and F.FertBasicMul or 1.0)
	self:Give(string.format("Crop:%s:%d", Crop.Id, Crop.R), 1)
	local R = self.Report
	R.Harvested = (R.Harvested or 0) + 1
	R["Harvest" .. Crop.R] = (R["Harvest" .. Crop.R] or 0) + 1
	-- 한 단계 위 씨앗
	if Crop.R < 3 and self:Random() < Rarities[Crop.R + 1].UpChance * Mul then
		self:Give(string.format("Seed:%s:%d", Crop.Id, Crop.R + 1), 1)
		R.UpSeeds = (R.UpSeeds or 0) + 1
		self:Hud():Announce("희귀 씨앗!", string.format("%s 씨앗 (%s)", Row.DisplayName, Rarities[Crop.R + 2].DisplayName), 2.5)
	end
	-- 계절 전용 희귀종 (일반 작물에서만)
	if not Row.Exclusive and Crop.R == 0 and self:Random() < F.ExclusiveChance * Mul then
		for _, Other in ipairs(D.Rows("Crops.etable")) do
			if Other.Exclusive and Other.Season == Row.Season then
				self:Give(string.format("Seed:%s:1", Other.Name), 1)
				R.ExclusiveSeeds = (R.ExclusiveSeeds or 0) + 1
				self:Hud():Announce("전설 속 씨앗!", Other.DisplayName .. " 씨앗을 발견했다", 3.0)
				break
			end
		end
	end
	if Row.Regrow > 0 then
		Crop.Age = Row.Days - Row.Regrow -- 다시 자라는 중
	else
		if self:Random() < F.SeedReturnChance then
			self:Give(string.format("Seed:%s:%d", Crop.Id, Crop.R), 1)
		end
		T.Crop = nil
		T.Fert = 0
	end
	if self.OnHarvest then self:OnHarvest(Row, Crop.R) end
end

-- ---- 하루·계절 (FarmTime 훅에서 부른다)
function Field:GrowField()
	local Grown = 0
	for _, T in pairs(self.Tiles) do
		if T.Crop and not T.Crop.Dead and T.Wet then
			T.Crop.Age = T.Crop.Age + 1
			Grown = Grown + 1
		end
		T.Wet = false
		self:RefreshTile(T)
	end
	if self.SyncCropTiles then self:SyncCropTiles() end
	return Grown
end

function Field:WitherField()
	local Count = 0
	for _, T in pairs(self.Tiles) do
		if T.Crop and not T.Crop.Dead and not self:IsGreenhouseTile(T.TX, T.TY) then
			T.Crop.Dead = true
			Count = Count + 1
		end
	end
	self.Report.Withered = (self.Report.Withered or 0) + Count
	if self.SyncCropTiles then self:SyncCropTiles() end
	return Count
end

-- ---- 대상 칸 표시 (플레이어가 매 프레임)
function Field:UpdateCursor(Pos, Facing)
	if not self.Cursor then return end
	if self.MapId ~= "Farm" then
		self.CursorSprite.Visible = false
		self.CursorAction = nil
		return
	end
	local S = self:SelectedItem()
	local TX, TY = self:TargetTile(Pos, Facing)
	local Action
	if self.BuildMode then
		Action = TX and self:PlanBuild(TX, TY)
	else
		Action = TX and self:PlanUse(S and S.Key, TX, TY)
	end
	local bShow = TX ~= nil and (S ~= nil or Action == "Harvest" or self.BuildMode) and self.Phase ~= "Sleep" and not self.CarryingCrystal
	self.CursorSprite.Visible = bShow
	if bShow then
		local C = self:TileCenter(TX, TY)
		local T = self:GetTile(TX, TY)
		self.Cursor:SetPosition(Vector3(C.X, C.Y, (T and T.Z or Pos.Z - 85) + 2.5))
		self.CursorSprite.Color = Action and Vector4(1, 1, 1, 0.9) or Vector4(1, 0.45, 0.4, 0.55)
	end
	self.CursorAction = Action
end

-- ---- 저장 조각
function Field:SaveField(T)
	local List = {}
	for _, Tile in pairs(self.Tiles) do
		List[#List + 1] = { TX = Tile.TX, TY = Tile.TY, Wet = Tile.Wet, Fert = Tile.Fert,
		                    Crop = Tile.Crop and { Id = Tile.Crop.Id, R = Tile.Crop.R, Age = Tile.Crop.Age, Dead = Tile.Crop.Dead } or nil }
	end
	T.Tiles = List
	T.RandState = self.RandState
end

function Field:LoadField(T)
	for _, Tile in pairs(self.Tiles) do self:DestroyTileVisuals(Tile) end
	self.Tiles = {}
	for _, S in ipairs(T.Tiles or {}) do
		local Tile = { TX = math.floor(S.TX), TY = math.floor(S.TY), Wet = S.Wet == true, Fert = math.floor(S.Fert or 0) }
		if S.Crop then
			Tile.Crop = { Id = S.Crop.Id, R = math.floor(S.Crop.R), Age = math.floor(S.Crop.Age), Dead = S.Crop.Dead == true }
		end
		self.Tiles[self:TileIndex(Tile.TX, Tile.TY)] = Tile
		self:RefreshTile(Tile)
	end
	self.RandState = math.floor(T.RandState or 12345)
end

return Field
