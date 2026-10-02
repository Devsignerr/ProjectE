-- 가방 창 (UI/RPG/Inventory.eui, 엔티티 "InventoryUI"): I/Tab(액션 Inventory)으로 열고 닫는다.
--   20칸(Slot1..20: SlotIcon/SlotCount), 장비 칸(EquipWeapon/EquipShield), 설명 패널(마우스를 올린 칸), 골드.
--   칸 클릭 = 사용/장착(GameManager:UseSlot), 장비 칸 클릭 = 해제. 상점이 열리면 닫는다.
local InventoryController = {
	Properties = {
		OpenOnStart = false, -- 시험용: 시작하자마자 연다
	},
}

local SlotCount = 20

local function ActionPressed(Action, FallbackKeys)
	local bOk, bPressed = pcall(Input.WasActionPressed, Action)
	if bOk then
		return bPressed
	end
	for _, Key in ipairs(FallbackKeys or {}) do
		if Input.IsKeyPressed(Key) then
			return true
		end
	end
	return false
end

function InventoryController:OnStart()
	local Manager = Scene.Find("GameManager")
	self.GM = Manager and Manager:GetScript() or nil
	self.UI = self.entity:GetComponent("UIComponent")
	self.Slots = {}
	for Index = 1, SlotCount do
		self.Slots[Index] = { Button = self.entity:GetWidget("Slot" .. Index), Icon = self.entity:GetWidget("SlotIcon" .. Index),
			Count = self.entity:GetWidget("SlotCount" .. Index) }
	end
	self.WeaponIcon = self.entity:GetWidget("EquipWeaponIcon")
	self.ShieldIcon = self.entity:GetWidget("EquipShieldIcon")
	self.StatsText  = self.entity:GetWidget("StatsText")
	self.GoldText   = self.entity:GetWidget("GoldText")
	self.Tooltip = {
		Panel = self.entity:GetWidget("Tooltip"), Name = self.entity:GetWidget("TooltipName"), Type = self.entity:GetWidget("TooltipType"),
		Stats = self.entity:GetWidget("TooltipStats"), Desc = self.entity:GetWidget("TooltipDesc"), Hint = self.entity:GetWidget("TooltipHint"),
	}
	self.ShownRevision = -1
	self.HoveredSlot   = nil
	self.bOpen         = false
	self.UI.Visible    = false
	if self.Properties.OpenOnStart then
		self:Open()
	end
end

function InventoryController:IsOpen()
	return self.bOpen
end

function InventoryController:Open()
	if self.bOpen or self.GM == nil then
		return
	end
	-- 상점과 동시에 열지 않는다
	local Shop = Scene.Find("ShopUI")
	local ShopScript = Shop and Shop:GetScript() or nil
	if ShopScript ~= nil and ShopScript.IsOpen ~= nil and ShopScript:IsOpen() then
		return
	end
	self.bOpen      = true
	self.UI.Visible = true
	self.ShownRevision = -1
	self:ShowTooltip(nil)
	self.GM:OpenWindow("Inventory")
	self.GM:PlaySound("Open")
end

function InventoryController:Close()
	if not self.bOpen then
		return
	end
	self.bOpen      = false
	self.UI.Visible = false
	if self.GM ~= nil then
		self.GM:CloseWindow("Inventory")
		self.GM:PlaySound("Close")
	end
end

function InventoryController:OnUpdate(dt)
	if self.GM == nil then
		return
	end
	if ActionPressed("Inventory", { "I", "Tab" }) then
		if self.bOpen then
			self:Close()
		else
			self:Open()
		end
	elseif self.bOpen and Input.IsKeyPressed("Escape") then
		self:Close()
	end
	if self.bOpen and self.GM.Revision ~= self.ShownRevision then
		self.ShownRevision = self.GM.Revision
		self:Refresh()
	end
end

function InventoryController:Refresh()
	for Index, Widgets in ipairs(self.Slots) do
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
	self:ShowEquip(self.WeaponIcon, self.GM:GetEquipped("Weapon"))
	self:ShowEquip(self.ShieldIcon, self.GM:GetEquipped("Shield"))
	local Damage, Defense, AttackPower = self.GM:GetEquipmentStats()
	local AttackText = AttackPower ~= nil and string.format("공격력 %d", AttackPower) or string.format("공격력 +%d", Damage)
	self.StatsText.Text = AttackText .. "\n" .. string.format("방어력 +%d", Defense)
	self.GoldText.Text  = tostring(self.GM:GetGold())
	-- 보고 있던 칸 설명 갱신 (사용해서 사라졌을 수 있다)
	if self.HoveredSlot ~= nil then
		self:ShowSlotTooltip(self.HoveredSlot)
	end
end

function InventoryController:ShowEquip(Icon, Id)
	local Def = Id and self.GM:GetItemDef(Id) or nil
	if Def ~= nil then
		Icon.Texture    = Def.Icon
		Icon.Visibility = "HitTestInvisible"
	else
		Icon.Visibility = "Hidden"
	end
end

-- 설명 패널: Def가 nil이면 안내 문구
function InventoryController:ShowTooltip(Def, Hint)
	local T = self.Tooltip
	if Def == nil then
		T.Name.Text  = "가방"
		T.Type.Text  = ""
		T.Stats.Text = ""
		T.Desc.Text  = "칸에 마우스를 올리면 아이템 설명이 보입니다."
		T.Hint.Text  = ""
		return
	end
	T.Name.Text  = Def.Name
	T.Type.Text  = string.format("%s · 판매가 %d 골드", self.GM:GetTypeName(Def), self.GM:GetSellPrice(Def.Id))
	T.Stats.Text = self.GM:DescribeStats(Def)
	T.Desc.Text  = Def.Description
	T.Hint.Text  = Hint or ""
end

local HintByType = { Consumable = "클릭: 사용", Weapon = "클릭: 장착", Shield = "클릭: 장착", Loot = "상인에게 팔 수 있습니다" }

function InventoryController:ShowSlotTooltip(Index)
	local Slot = self.GM:GetSlot(Index)
	local Def  = Slot and self.GM:GetItemDef(Slot.Id) or nil
	self:ShowTooltip(Def, Def and HintByType[Def.Type] or nil)
end

function InventoryController:OnSlotClicked(Index)
	if self.GM:GetSlot(Index) ~= nil then
		self.GM:PlaySound("Click")
		self.GM:UseSlot(Index)
	end
end

function InventoryController:OnSlotHover(Index, bBegin)
	if bBegin then
		self.HoveredSlot = Index
		self:ShowSlotTooltip(Index)
	elseif self.HoveredSlot == Index then
		self.HoveredSlot = nil
		self:ShowTooltip(nil)
	end
end

for Index = 1, SlotCount do
	InventoryController["OnUIClicked_Slot" .. Index]    = function(self) self:OnSlotClicked(Index) end
	InventoryController["OnUIHoverBegin_Slot" .. Index] = function(self) self:OnSlotHover(Index, true) end
	InventoryController["OnUIHoverEnd_Slot" .. Index]   = function(self) self:OnSlotHover(Index, false) end
end

function InventoryController:OnUIClicked_EquipWeapon() self.GM:Unequip("Weapon") end
function InventoryController:OnUIClicked_EquipShield() self.GM:Unequip("Shield") end
function InventoryController:OnUIHoverBegin_EquipWeapon()
	local Id = self.GM:GetEquipped("Weapon")
	self:ShowTooltip(Id and self.GM:GetItemDef(Id) or nil, Id and "클릭: 해제" or nil)
end
function InventoryController:OnUIHoverBegin_EquipShield()
	local Id = self.GM:GetEquipped("Shield")
	self:ShowTooltip(Id and self.GM:GetItemDef(Id) or nil, Id and "클릭: 해제" or nil)
end
function InventoryController:OnUIHoverEnd_EquipWeapon() self:ShowTooltip(nil) end
function InventoryController:OnUIHoverEnd_EquipShield() self:ShowTooltip(nil) end
function InventoryController:OnUIClicked_CloseButton() self:Close() end

return InventoryController
