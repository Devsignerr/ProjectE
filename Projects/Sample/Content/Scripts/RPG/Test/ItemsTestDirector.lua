-- RPG_Test_Items 자동 시연 (자동 검증 스크린샷용): 프레임 번호로 진행한다 (자동 검증은 VSync 없이 빠르게 돌기 때문).
--   GiveFrame: 시험 아이템 지급 → LootFrame: 플레이어 옆에 전리품 표들을 흩뿌림 → PotionFrame: 체력 물약 퀵 슬롯 사용
--   → InventoryFrame: 가방 열기 → EquipFrame: 가방의 무기/방패 장착 → ShopFrame: 가방 닫고 상인 상점 열기
--   → TradeFrame: 1번 상품 구매 + 뼈 1개 판매 → EndFrame: 상점 닫기 → DeathFrame: 대역 플레이어 쓰러짐(사망 화면)
--   음수인 단계는 건너뛴다. Enabled=false면 아무것도 하지 않는다 (직접 플레이할 때).
local ItemsTestDirector = {
	Properties = {
		Enabled        = true,
		GiveFrame      = 5,
		GiveItems      = "sword_1handed:1,shield_round:1,bone:5,magic_shard:2",
		LootFrame      = 20,
		LootTables     = "Test,Skeleton_Mage",
		LootOffsetX    = 260.0, -- 플레이어 기준 (월드 cm)
		LootOffsetY    = -260.0,
		PotionFrame    = 100,
		InventoryFrame = 240,
		EquipFrame     = 270,
		ShopFrame      = 360,
		TradeFrame     = 400,
		EndFrame       = 480,
		DeathFrame     = 520,
	},
}

function ItemsTestDirector:OnStart()
	self.Done = {}
end

function ItemsTestDirector:Step(Name, Frame)
	if Frame < 0 or self.Done[Name] or Time.FrameCount < Frame then
		return false
	end
	self.Done[Name] = true
	Log.Info("[ItemsTestDirector] 단계", Name, "프레임", Time.FrameCount)
	return true
end

local function FindScript(Name)
	local Entity = Scene.Find(Name)
	return Entity and Entity:GetScript() or nil
end

function ItemsTestDirector:OnUpdate(dt)
	local P = self.Properties
	if not P.Enabled then
		return
	end
	local GM = FindScript("GameManager")
	if GM == nil then
		return
	end
	if self:Step("Give", P.GiveFrame) then
		for Id, Count in P.GiveItems:gmatch("([%w_]+):(%d+)") do
			GM:GiveItem(Id, tonumber(Count))
		end
	end
	if self:Step("Loot", P.LootFrame) then
		local Player = Scene.Find("Player")
		if Player ~= nil then
			local Base  = Player:GetWorldPosition()
			local Index = 0
			for Id in P.LootTables:gmatch("[^,%s]+") do
				GM:SpawnLoot(Base + Vector3(P.LootOffsetX + Index * 160, P.LootOffsetY - Index * 160, 0), Id)
				Index = Index + 1
			end
		end
	end
	if self:Step("Potion", P.PotionFrame) then
		GM:UseQuickPotion(1)
	end
	if self:Step("Inventory", P.InventoryFrame) then
		local Inventory = FindScript("InventoryUI")
		if Inventory ~= nil then Inventory:Open() end
	end
	if self:Step("Equip", P.EquipFrame) then
		for _, Kind in ipairs({ "Weapon", "Shield" }) do
			for Index = 1, GM:GetSlotCount() do
				local Slot = GM:GetSlot(Index)
				local Def  = Slot and GM:GetItemDef(Slot.Id)
				if Def ~= nil and Def.Type == Kind and not Def.bTwoHanded then
					GM:UseSlot(Index)
					break
				end
			end
		end
	end
	if self:Step("Shop", P.ShopFrame) then
		local Inventory = FindScript("InventoryUI")
		if Inventory ~= nil then Inventory:Close() end
		local Shop     = FindScript("ShopUI")
		local Merchant = FindScript("Merchant")
		if Merchant ~= nil then
			Merchant:OpenShop()
		elseif Shop ~= nil then
			Shop:Open("상점", GM:ParseStock(Shop.Properties.DefaultStock), nil)
		end
	end
	if self:Step("Trade", P.TradeFrame) then
		local Shop = FindScript("ShopUI")
		if Shop ~= nil and Shop:IsOpen() then
			Shop:OnBuy(1)
			for Index = 1, GM:GetSlotCount() do
				local Slot = GM:GetSlot(Index)
				if Slot ~= nil and Slot.Id == "bone" then
					Shop:OnSell(Index)
					break
				end
			end
		end
	end
	if self:Step("End", P.EndFrame) then
		local Shop = FindScript("ShopUI")
		if Shop ~= nil then Shop:Close() end
	end
	if self:Step("Death", P.DeathFrame) then
		local Player = FindScript("Player")
		if Player ~= nil and Player.DebugKill ~= nil then Player:DebugKill() end
	end
end

return ItemsTestDirector
