-- 상인 NPC (Prefabs/RPG/Merchant.eprefab): 플레이어가 가까이 오면 "E: 거래" 안내, Interact(E)로 상점 창(엔티티 "ShopUI")을 연다.
--   열려 있는 동안 플레이어 쪽을 보고, 멀어지면 창을 닫는다. 모델(자식 "Model")은 Idle 반복, 열 때 Interact, 구매 후 닫을 때 Cheer.
local Merchant = {
	Properties = {
		DisplayName    = "떠돌이 상인 브론",
		Stock          = "potion_hp_small,potion_hp_large,potion_mp,dagger,sword_1handed,axe_1handed,sword_2handed,shield_round,shield_square,shield_badge",
		InteractRadius = 200.0,
		PlayerName     = "Player",
		TurnSpeed      = 6.0,  -- 플레이어 쪽으로 도는 빠르기
	},
}

local function ActionPressed(Action, FallbackKey)
	local bOk, bPressed = pcall(Input.WasActionPressed, Action)
	if bOk then
		return bPressed
	end
	return Input.IsKeyPressed(FallbackKey)
end

function Merchant:OnStart()
	local Manager = Scene.Find("GameManager")
	self.GM     = Manager and Manager:GetScript() or nil
	local ShopEntity = Scene.Find("ShopUI")
	self.Shop   = ShopEntity and ShopEntity:GetScript() or nil
	self.Player = Scene.Find(self.Properties.PlayerName)
	self.Model  = self.entity:FindChild("Model")
	self.Stock  = self.GM and self.GM:ParseStock(self.Properties.Stock) or {}
	self.bNear  = false
	self.bShopOpen = false
	self.StartGold = 0
	self.AnimTimer = -1
	if self.GM == nil or self.Shop == nil then
		Log.Warn("Merchant: GameManager 또는 ShopUI가 없어 거래할 수 없습니다")
	end
end

function Merchant:PlayClip(Clip, Duration)
	if self.Model ~= nil and self.Model:IsValid() then
		self.Model:PlayAnimation(Clip, 0.2)
		self.AnimTimer = Duration or -1
	end
end

function Merchant:OnUpdate(dt)
	-- 한 번 재생하는 동작이 끝나면 Idle로
	if self.AnimTimer >= 0 then
		self.AnimTimer = self.AnimTimer - dt
		if self.AnimTimer < 0 then
			self:PlayClip("Idle")
		end
	end
	if self.GM == nil or self.Shop == nil then
		return
	end
	if self.Player == nil or not self.Player:IsValid() then
		self.Player = Scene.Find(self.Properties.PlayerName)
		if self.Player == nil then
			return
		end
	end

	local Here   = self.entity:GetWorldPosition()
	local There  = self.Player:GetWorldPosition()
	local Offset = Vector3(There.X - Here.X, There.Y - Here.Y, 0)
	local Distance = Offset:Length()
	local PlayerScript = self.Player:GetScript()
	local bDead = PlayerScript ~= nil and type(PlayerScript.IsDead) == "function" and PlayerScript:IsDead()
	local bNear = Distance <= self.Properties.InteractRadius and not bDead

	if bNear ~= self.bNear then
		self.bNear = bNear
		if bNear then
			self.GM:SetPrompt(self, "거래 — " .. self.Properties.DisplayName, "E")
		else
			self.GM:ClearPrompt(self)
		end
	end

	-- 상점이 열려 있으면 플레이어를 바라보고, 멀어지면 닫는다
	if self.bShopOpen then
		if Distance > self.Properties.InteractRadius * 1.6 or bDead then
			self.Shop:Close()
		elseif ActionPressed("Interact", "E") and Time.FrameCount > self.OpenedFrame then
			self.Shop:Close()
		end
	elseif bNear and ActionPressed("Interact", "E") then
		self:OpenShop()
	end
	if (self.bShopOpen or bNear) and Distance > 1 then
		local Target  = Quat.LookRotation(Offset)
		local Current = self.entity:GetRotation()
		self.entity:SetRotation(Quat.Slerp(Current, Target, math.min(1, self.Properties.TurnSpeed * dt)))
	end
end

function Merchant:OpenShop()
	if self.Shop:IsOpen() then
		return
	end
	self.Shop:Open(self.Properties.DisplayName, self.Stock, self)
	self.bShopOpen   = self.Shop:IsOpen()
	self.OpenedFrame = Time.FrameCount
	self.StartGold   = self.GM:GetGold()
	self:PlayClip("Interact", 1.2)
end

-- ShopController가 닫힐 때 부른다
function Merchant:OnShopClosed()
	self.bShopOpen = false
	-- 거래가 있었으면 기뻐한다
	if self.GM ~= nil and self.GM:GetGold() ~= self.StartGold then
		self:PlayClip("Cheer", 2.0)
	end
end

function Merchant:OnDestroy()
	if self.GM ~= nil then
		self.GM:ClearPrompt(self)
	end
end

return Merchant
