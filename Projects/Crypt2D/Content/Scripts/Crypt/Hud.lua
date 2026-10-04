-- Crypt2D HUD (씬의 HUD 엔티티 — UI/Crypt/HUD.eui). GameManager/Player가 메서드로 값을 넘기고, 메뉴 단추는 GameManager:OnMenu로 돌려준다.
--   체력·대시 칸·코인·처치·층, 무기 2칸(현재 칸 강조), 미니맵(방 격자 — 현재/방문/알려진 방, 출구·보물·보스 색, 문 연결선),
--   데미지 숫자(DmgTemplate 복제 풀 — Camera.WorldToScreen으로 매 프레임 자리, 위로 뜨며 사라짐), 알림, 상호작용 안내, 보스 체력바,
--   조준점(실제 마우스 커서 자리 — OS 커서는 숨김), 일시정지/사망/승리 화면.
local Hud = {
	Properties = {},
}

local PoolSize = 28

function Hud:OnStart()
	self.GM = Scene.Find("GameManager"):GetScript()
	self.Numbers = {}
	self.Free = {}
	self.ToastTime = 0
	self.LastPrompt = false
end

function Hud:W(Name)
	return self.entity:GetWidget(Name)
end

function Hud:SetHealth(Health, Max)
	self:W("HpBar").Percent = Max > 0 and Health / Max or 0
	self:W("HpText").Text = string.format("%d / %d", math.ceil(Health), math.floor(Max))
end

function Hud:SetDash(Charges, Max, Fraction)
	for I = 0, 2 do
		local Bar = self:W("Dash" .. I)
		local bShow = I < Max
		if Bar.Visible ~= bShow then Bar.Visible = bShow end
		if bShow then
			local Value = (I < Charges) and 1.0 or ((I == Charges) and Fraction * 0.999 or 0.0)
			Bar.Percent = Value
		end
	end
end

function Hud:SetGold(Gold, Kills)
	self:W("GoldText").Text = tostring(Gold)
	self:W("KillText").Text = "처치 " .. tostring(Kills)
end

function Hud:SetFloor(Text)
	self:W("FloorText").Text = Text
end

function Hud:SetWeapons(Ids, Slot)
	local CryptData = self.GM.Data
	for I = 0, 1 do
		local Def = CryptData.Weapon(Ids[I + 1])
		if Def then self:W("WeaponSlot" .. I .. "Icon").Texture = Def.Icon end
		local bCurrent = (I + 1) == Slot
		self:W("WeaponSlot" .. I .. "Bg").Color = bCurrent and Vector4(0.42, 0.28, 0.12, 0.95) or Vector4(0.05, 0.03, 0.08, 0.8)
		self:W("WeaponSlot" .. I).Opacity = bCurrent and 1.0 or 0.6
	end
	local Def = CryptData.Weapon(Ids[Slot])
	self:W("WeaponName").Text = Def and Def.DisplayName or ""
end

function Hud:ShowToast(Text, Time)
	local T = self:W("Toast")
	T.Text = Text
	T.Visible = true
	T.Opacity = 1.0
	self.ToastTime = Time or 1.5
end

function Hud:ShowPrompt(Text)
	local Key = Text or false
	if Key == self.LastPrompt then return end
	self.LastPrompt = Key
	self:W("Prompt").Visible = Text ~= nil
	if Text then self:W("PromptText").Text = Text end
end

function Hud:ShowBoss(Name, Fraction)
	local Panel = self:W("BossPanel")
	if Name == nil then
		Panel.Visible = false
		return
	end
	if not Panel.Visible then
		Panel.Visible = true
		self:W("BossName").Text = Name
	end
	self:W("BossBar").Percent = math.max(0, Fraction or 0)
end

function Hud:SetCrosshair(X, Y, bVisible)
	local C = self:W("Crosshair")
	C.Position = Vector2(X, Y)
	if C.Visible ~= bVisible then C.Visible = bVisible end
end

-- ---- 미니맵
local RoomColors = {
	Exit = Vector4(0.35, 0.85, 0.45, 1), Treasure = Vector4(1.0, 0.75, 0.3, 1), Boss = Vector4(0.9, 0.22, 0.3, 1),
	Shop = Vector4(0.45, 0.7, 1.0, 1),
}

function Hud:UpdateMinimap(Layout, Current)
	if Layout == nil then return end
	for Y = 0, 3 do
		for X = 0, 4 do
			local Room = Layout.ByKey[Y * 100 + X]
			local Cell = self:W(string.format("Map_%d_%d", X, Y))
			local bShow = Room ~= nil and Room.Known
			Cell.Visible = bShow
			if bShow then
				local C
				if Room == Current then
					C = Vector4(1.0, 0.95, 0.75, 1)
				elseif RoomColors[Room.Kind] and (Room.Visited or Room.Kind ~= "Combat") then
					C = RoomColors[Room.Kind]
					if not Room.Visited then C = Vector4(C.X * 0.55, C.Y * 0.55, C.Z * 0.55, 1) end
				elseif Room.Visited then
					C = Room.State == "Active" and Vector4(0.8, 0.35, 0.35, 1) or Vector4(0.5, 0.46, 0.58, 1)
				else
					C = Vector4(0.22, 0.2, 0.28, 1)
				end
				Cell.Color = C
			end
			if X < 4 then
				local Link = self:W(string.format("MapH_%d_%d", X, Y))
				local Other = Layout.ByKey[Y * 100 + X + 1]
				Link.Visible = Room ~= nil and Other ~= nil and Room.Doors.R == true and (Room.Visited or Other.Visited)
			end
			if Y < 3 then
				local Link = self:W(string.format("MapV_%d_%d", X, Y))
				local Other = Layout.ByKey[(Y + 1) * 100 + X]
				Link.Visible = Room ~= nil and Other ~= nil and Room.Doors.U == true and (Room.Visited or Other.Visited)
			end
		end
	end
end

-- ---- 데미지 숫자
function Hud:ShowDamage(X, Z, Text, Color, Scale)
	local Item = table.remove(self.Free)
	if Item == nil then
		if #self.Numbers >= PoolSize then
			Item = table.remove(self.Numbers, 1)
		else
			local Name = "Dmg_" .. tostring(#self.Numbers + #self.Free + 1)
			self.Created = (self.Created or 0) + 1
			Name = "Dmg_" .. tostring(self.Created)
			self.entity:CloneWidget("DmgTemplate", Name)
			Item = { Name = Name }
		end
	end
	local W = self:W(Item.Name)
	W.Text = Text
	W.Color = Vector4(Color[1], Color[2], Color[3], Color[4] or 1)
	W.FontSize = math.floor(28 * (Scale or 1))
	W.Opacity = 1.0
	W.Visible = true
	Item.X, Item.Z = X + self.GM.Rng:Range(-14, 14), Z
	Item.Age = 0
	self.Numbers[#self.Numbers + 1] = Item
	self:PlaceNumber(Item, W)
end

function Hud:PlaceNumber(Item, W)
	local SX, SY = Camera.WorldToScreen(Vector3(Item.X, 0, Item.Z + Item.Age * 90), self.entity)
	W.Position = Vector2(SX, SY)
	W.Opacity = math.min(1.0, (0.75 - Item.Age) * 4)
end

function Hud:LateUpdate(Dt)
	local Keep = {}
	for _, Item in ipairs(self.Numbers) do
		Item.Age = Item.Age + Dt
		local W = self:W(Item.Name)
		if Item.Age > 0.75 then
			W.Visible = false
			self.Free[#self.Free + 1] = Item
		else
			self:PlaceNumber(Item, W)
			Keep[#Keep + 1] = Item
		end
	end
	self.Numbers = Keep
	if self.ToastTime > 0 then
		self.ToastTime = self.ToastTime - Dt
		local T = self:W("Toast")
		if self.ToastTime <= 0 then
			T.Visible = false
		elseif self.ToastTime < 0.4 then
			T.Opacity = self.ToastTime / 0.4
		end
	end
end

-- ---- 메뉴 화면
function Hud:ShowPause(bShow)
	self:W("PauseScreen").Visible = bShow
	if bShow then self:W("Crosshair").Visible = false end -- 메뉴는 OS 커서로 (계속하면 플레이어가 다시 켠다)
end

function Hud:ShowDeath(Text)
	self:W("DeathResult").Text = Text
	self:W("DeathScreen").Visible = true
	self:W("Crosshair").Visible = false
end

function Hud:ShowVictory(Text)
	self:W("VictoryResult").Text = Text
	self:W("VictoryScreen").Visible = true
	self:W("Crosshair").Visible = false
end

function Hud:OnUIClicked_ResumeButton() self.GM:OnMenu("Resume") end
function Hud:OnUIClicked_RestartButton() self.GM:OnMenu("Restart") end
function Hud:OnUIClicked_TitleButton() self.GM:OnMenu("Title") end
function Hud:OnUIClicked_QuitButton() self.GM:OnMenu("Quit") end
function Hud:OnUIClicked_DeathRestartButton() self.GM:OnMenu("Restart") end
function Hud:OnUIClicked_DeathTitleButton() self.GM:OnMenu("Title") end
function Hud:OnUIClicked_VictoryRestartButton() self.GM:OnMenu("Restart") end
function Hud:OnUIClicked_VictoryTitleButton() self.GM:OnMenu("Title") end

return Hud
