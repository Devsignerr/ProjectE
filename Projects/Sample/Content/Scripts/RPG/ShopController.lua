-- 상점 창 (UI/RPG/Shop.eui, 엔티티 "ShopUI"): 상인(Merchant.lua)이 Open(상인 이름, 판매 목록)으로 연다.
--   왼쪽 = 구매 목록 (Row1..8: RowIcon/RowName/RowPrice/Buy 버튼), 오른쪽 = 내 가방 (Sell1..20 클릭 = 1개 판매, 반값).
--   마우스를 올린 아이템은 아래 설명 패널에. E/ESC/X 버튼으로 닫고, 상인이 멀어지면 상인이 닫는다.
local ShopController = {
	Properties = {
		OpenOnStart  = false, -- 시험용: 시작하자마자 기본 목록으로 연다
		DefaultStock = "potion_hp_small,potion_hp_large,potion_mp,dagger,sword_1handed,axe_1handed,shield_round,shield_square",
	},
}

local RowCount  = 8
local SlotCount = 20

function ShopController:OnStart()
	local Manager = Scene.Find("GameManager")
	self.GM = Manager and Manager:GetScript() or nil
	self.UI = self.entity:GetComponent("UIComponent")
	local W = function(Name) return self.entity:GetWidget(Name) end
	self.Title    = W("Title")
	self.GoldText = W("GoldText")
	self.Rows = {}
	for Index = 1, RowCount do
		self.Rows[Index] = { Row = W("Row" .. Index), Icon = W("RowIcon" .. Index), Name = W("RowName" .. Index),
			Price = W("RowPrice" .. Index), Buy = W("Buy" .. Index) }
	end
	self.Sell = {}
	for Index = 1, SlotCount do
		self.Sell[Index] = { Icon = W("SellIcon" .. Index), Count = W("SellCount" .. Index) }
	end
	self.Tooltip = { Name = W("ShopTooltipName"), Type = W("ShopTooltipType"), Stats = W("ShopTooltipStats"),
		Desc = W("ShopTooltipDesc"), Hint = W("ShopTooltipHint") }
	self.Stock = {}
	self.bOpen = false
	self.UI.Visible = false
	self.OpenedFrame = -1
	if self.Properties.OpenOnStart and self.GM ~= nil then
		self:Open("상점", self.GM:ParseStock(self.Properties.DefaultStock), nil)
	end
end

function ShopController:IsOpen()
	return self.bOpen
end

-- Owner: 연 상인 스크립트 (닫힐 때 OnShopClosed를 부른다, 없어도 됨)
function ShopController:Open(Title, Stock, Owner)
	if self.GM == nil then
		return
	end
	-- 가방 창이 열려 있으면 닫는다 (상점 오른쪽이 가방)
	local Inventory = Scene.Find("InventoryUI")
	local InventoryScript = Inventory and Inventory:GetScript() or nil
	if InventoryScript ~= nil and InventoryScript.Close ~= nil then
		InventoryScript:Close()
	end
	self.Owner = Owner
	self.Stock = Stock or {}
	self.Title.Text = Title or "상점"
	self.bOpen      = true
	self.UI.Visible = true
	self.ShownRevision = -1
	self.OpenedFrame = Time.FrameCount
	self:ShowTooltip(nil)
	for Index, Widgets in ipairs(self.Rows) do
		local Def = self.GM:GetItemDef(self.Stock[Index] or "")
		if Def ~= nil then
			Widgets.Row.Visibility = "SelfHitTestInvisible"
			Widgets.Icon.Texture   = Def.Icon
			Widgets.Name.Text      = Def.Name
			Widgets.Price.Text     = tostring(Def.Price)
		else
			Widgets.Row.Visibility = "Collapsed"
		end
	end
	self.GM:OpenWindow("Shop")
	self.GM:PlaySound("Open")
end

function ShopController:Close()
	if not self.bOpen then
		return
	end
	self.bOpen      = false
	self.UI.Visible = false
	if self.GM ~= nil then
		self.GM:CloseWindow("Shop")
		self.GM:PlaySound("Close")
	end
	local Owner = self.Owner
	self.Owner = nil
	if Owner ~= nil and type(Owner.OnShopClosed) == "function" then
		Owner:OnShopClosed()
	end
end

function ShopController:OnUpdate(dt)
	if not self.bOpen or self.GM == nil then
		return
	end
	-- 연 프레임의 E 입력으로 바로 닫지 않는다
	if Time.FrameCount > self.OpenedFrame and Input.IsKeyPressed("Escape") then
		self:Close()
		return
	end
	if self.GM.Revision ~= self.ShownRevision then
		self.ShownRevision = self.GM.Revision
		self:Refresh()
	end
end

function ShopController:Refresh()
	local Gold = self.GM:GetGold()
	self.GoldText.Text = tostring(Gold)
	for Index, Widgets in ipairs(self.Rows) do
		local Def = self.GM:GetItemDef(self.Stock[Index] or "")
		if Def ~= nil then
			local bAfford = Gold >= Def.Price
			Widgets.Buy.Enabled  = bAfford
			Widgets.Price.Color  = bAfford and Vector4(1.0, 0.84, 0.36, 1) or Vector4(0.85, 0.3, 0.25, 1)
		end
	end
	for Index, Widgets in ipairs(self.Sell) do
		local Slot = self.GM:GetSlot(Index)
		local Def  = Slot and self.GM:GetItemDef(Slot.Id) or nil
		if Def ~= nil then
			Widgets.Icon.Texture    = Def.Icon
			Widgets.Icon.Visibility = "HitTestInvisible"
			Widgets.Count.Text      = Slot.Count > 1 and tostring(Slot.Count) or ""
		else
			Widgets.Icon.Visibility = "Hidden"
			Widgets.Count.Text      = ""
		end
	end
	if self.HoveredSell ~= nil then
		self:OnSellHover(self.HoveredSell, true)
	end
end

function ShopController:ShowTooltip(Def, Hint)
	local T = self.Tooltip
	if Def == nil then
		T.Name.Text, T.Type.Text, T.Stats.Text, T.Hint.Text = "", "", "", ""
		T.Desc.Text = "아이템에 마우스를 올리면 설명이 보입니다."
		return
	end
	T.Name.Text  = Def.Name
	T.Type.Text  = self.GM:GetTypeName(Def)
	T.Stats.Text = self.GM:DescribeStats(Def)
	T.Desc.Text  = Def.Description
	T.Hint.Text  = Hint or ""
end

function ShopController:OnBuy(Index)
	local Id = self.Stock[Index]
	if Id ~= nil then
		self.GM:PlaySound("Click")
		self.GM:BuyItem(Id)
	end
end

function ShopController:OnRowHover(Index, bBegin)
	local Def = bBegin and self.GM:GetItemDef(self.Stock[Index] or "") or nil
	self:ShowTooltip(Def, Def and string.format("구매가 %d 골드", Def.Price) or nil)
end

function ShopController:OnSell(Index)
	if self.GM:GetSlot(Index) ~= nil then
		self.GM:SellSlot(Index)
	end
end

function ShopController:OnSellHover(Index, bBegin)
	self.HoveredSell = bBegin and Index or (self.HoveredSell ~= Index and self.HoveredSell or nil)
	local Slot = bBegin and self.GM:GetSlot(Index) or nil
	local Def  = Slot and self.GM:GetItemDef(Slot.Id) or nil
	self:ShowTooltip(Def, Def and string.format("클릭: 1개 판매 (+%d 골드)", self.GM:GetSellPrice(Def.Id)) or nil)
end

for Index = 1, RowCount do
	ShopController["OnUIClicked_Buy" .. Index]        = function(self) self:OnBuy(Index) end
	ShopController["OnUIHoverBegin_Buy" .. Index]     = function(self) self:OnRowHover(Index, true) end
	ShopController["OnUIHoverEnd_Buy" .. Index]       = function(self) self:OnRowHover(Index, false) end
	ShopController["OnUIHoverBegin_RowSlot" .. Index] = function(self) self:OnRowHover(Index, true) end
	ShopController["OnUIHoverEnd_RowSlot" .. Index]   = function(self) self:OnRowHover(Index, false) end
end
for Index = 1, SlotCount do
	ShopController["OnUIClicked_Sell" .. Index]    = function(self) self:OnSell(Index) end
	ShopController["OnUIHoverBegin_Sell" .. Index] = function(self) self:OnSellHover(Index, true) end
	ShopController["OnUIHoverEnd_Sell" .. Index]   = function(self) self:OnSellHover(Index, false) end
end

function ShopController:OnUIClicked_CloseButton() self:Close() end

return ShopController
