-- 줍는 아이템 (Prefabs/RPG/Pickup.eprefab): 아이템 모델을 자식으로 만들고 떠다니며 돈다.
--   플레이어가 MagnetRadius 안에 오면 끌려가고 PickupRadius 안에서 GameManager:GiveItem으로 들어간다 (가방이 가득이면 남는다).
--   GameManager:SpawnLoot가 ItemId/Count/Hop(흩뿌릴 오프셋)을 PropertyOverrides로 넘긴다. 씬에 직접 놓아도 된다.
local Pickup = {
	Properties = {
		ItemId        = "gold",
		Count         = 1,
		HopX          = 0.0,   -- 생길 때 튀어 나갈 수평 오프셋 (cm)
		HopY          = 0.0,
		HopTime       = 0.45,
		HopHeight     = 70.0,
		PickupDelay   = 0.5,   -- 생긴 뒤 이 시간 동안은 줍지 않는다
		MagnetRadius  = 220.0,
		PickupRadius  = 55.0,
		MagnetSpeed   = 900.0, -- 끌려갈 때 최대 속도 (cm/s)
		FloatHeight   = 25.0,  -- 바닥에서 띄우는 높이
		SpinSpeed     = 120.0, -- 도/초
		PlayerName    = "Player",
	},
}

function Pickup:OnStart()
	local Manager = Scene.Find("GameManager")
	self.GM     = Manager and Manager:GetScript() or nil
	self.Player = Scene.Find(self.Properties.PlayerName)
	self.Age    = 0
	self.Phase  = math.random() * math.pi * 2
	self.Spin   = math.random() * 360

	local Def = self.GM and self.GM:GetItemDef(self.Properties.ItemId) or nil
	if Def == nil then
		Log.Warn("Pickup: 아이템 정의 없음 (GameManager가 없거나 모르는 id)", self.Properties.ItemId)
	end
	self.Def = Def

	-- 바닥 기준 위치와 튀어 나갈 목표
	local Start = self.entity:GetPosition()
	self.Base   = Start
	self.Target = Start + Vector3(self.Properties.HopX, self.Properties.HopY, 0)
	self.bHopping = (self.Properties.HopX ~= 0 or self.Properties.HopY ~= 0)

	-- 모델 자식 (줍는 아이템마다 모델이 달라 런타임에 붙인다 — AddComponent가 에셋 해석을 다시 요청한다)
	local Model = Def and Def.Model or "Asset/KayKit/Dungeon/coin.glb"
	local Scale = Def and Def.ModelScale or 1.0
	if Def ~= nil and Def.bCurrency and self.Properties.Count >= 10 then
		Model, Scale = "Asset/KayKit/Dungeon/coin_stack_small.glb", 1.2
	end
	self.Visual = Scene.Create("PickupVisual")
	self.Visual:SetParent(self.entity)
	self.Visual:SetPosition(Vector3(0, 0, self.Properties.FloatHeight))
	self.Visual:SetScale(Vector3(Scale, Scale, Scale))
	-- 무기는 눕혀서 (세워 두면 픽셀 아트에서 가늘게 보인다)
	if Def ~= nil and Def.Type == "Weapon" then
		self.Tilt = 70
	else
		self.Tilt = 0
	end
	local Component = self.Visual:AddComponent("ModelComponent")
	Component.AssetPath = Model
	self:UpdateVisual(0)
end

function Pickup:UpdateVisual(dt)
	if self.Visual == nil or not self.Visual:IsValid() then
		return
	end
	self.Spin  = (self.Spin + self.Properties.SpinSpeed * dt) % 360
	self.Phase = self.Phase + dt * 3.0
	local Bob = math.sin(self.Phase) * 6.0
	self.Visual:SetPosition(Vector3(0, 0, self.Properties.FloatHeight + Bob))
	self.Visual:SetRotation(Quat.FromEuler(self.Tilt, self.Spin, 0))
end

function Pickup:OnUpdate(dt)
	self.Age = self.Age + dt
	self:UpdateVisual(dt)

	-- 튀어 나가기 (포물선)
	if self.bHopping then
		local T = math.min(self.Age / self.Properties.HopTime, 1.0)
		local Flat = self.Base:Lerp(self.Target, T)
		self.entity:SetPosition(Flat + Vector3(0, 0, math.sin(T * math.pi) * self.Properties.HopHeight))
		if T >= 1.0 then
			self.bHopping = false
			self.entity:SetPosition(self.Target)
			if self.Def ~= nil and not self.Def.bCurrency then
				self.GM:PlaySound("Drop")
			end
		end
		return
	end

	if self.GM == nil or self.Def == nil or self.Age < self.Properties.PickupDelay then
		return
	end
	if self.Player == nil or not self.Player:IsValid() then
		self.Player = Scene.Find(self.Properties.PlayerName)
		if self.Player == nil then
			return
		end
	end
	local PlayerScript = self.Player:GetScript()
	if PlayerScript ~= nil and type(PlayerScript.IsDead) == "function" and PlayerScript:IsDead() then
		return
	end

	-- 수평 거리 (플레이어 위치는 캡슐 중심일 수 있다)
	local Here    = self.entity:GetPosition()
	local There   = self.Player:GetWorldPosition()
	local Offset  = Vector3(There.X - Here.X, There.Y - Here.Y, 0)
	local Distance = Offset:Length()
	if Distance <= self.Properties.PickupRadius then
		self:TryCollect()
	elseif Distance <= self.Properties.MagnetRadius and not self.bBagFull then
		-- 가까울수록 빨라진다
		local Speed = self.Properties.MagnetSpeed * (1.0 - Distance / self.Properties.MagnetRadius) + 150.0
		local Step  = math.min(Speed * dt, Distance)
		self.entity:SetPosition(Here + Offset:Normalized() * Step)
	elseif Distance > self.Properties.MagnetRadius * 1.5 then
		self.bBagFull = false -- 멀어지면 다시 시도
	end
end

function Pickup:TryCollect()
	if self.bCollected then
		return
	end
	local Def, Count = self.Def, self.Properties.Count
	if not self.GM:GiveItem(Def.Id, Count) then
		if not self.bBagFull then
			self.bBagFull = true
			self.GM:Notify("가방이 가득 찼습니다")
			self.GM:PlaySound("Error")
		end
		return
	end
	self.bCollected = true
	if Def.bCurrency then
		self.GM:Notify(string.format("+%d 골드", Count))
		self.GM:PlaySound("Coins")
	else
		self.GM:Notify(Count > 1 and string.format("%s x%d 획득", Def.Name, Count) or (Def.Name .. " 획득"))
		self.GM:PlaySound("Pickup")
	end
	self.entity:Destroy()
end

return Pickup
