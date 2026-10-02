-- 엔진 API 검증 (Phase 45 트랙 A): Script.Require + Camera.WorldToScreen + CloneWidget/RemoveWidget + 위젯 Position
-- UIComponent(UI/RPG/Test/EngineApiTest.eui)가 있는 엔티티에 붙인다. 대상마다 이름표(LabelTemplate)와 중심 점(DotTemplate)을 복제해
-- 매 프레임 월드 위치에 놓는다 — 점은 큐브 중심, 이름표 아래끝은 큐브 위에 와야 한다.
local Data = Script.Require("Scripts/RPG/Test/EngineApiTestData.lua")

local EngineApiTest = { Properties = {} }

function EngineApiTest:OnStart()
	self.Items = {}
	for Index, Target in ipairs(Data.Targets) do
		local Entity = Scene.Find(Target.Entity)
		if Entity then
			local Label = self.entity:CloneWidget("LabelTemplate", "Label" .. Index)
			Label.Visible = true
			self.entity:GetWidget("Label" .. Index .. ".Name").Text = Target.Name
			self.entity:GetWidget("Label" .. Index .. ".Hp").Percent = Target.Health
			local Dot = self.entity:CloneWidget("DotTemplate", "Dot" .. Index)
			Dot.Visible = true
			table.insert(self.Items, { Entity = Entity, Label = Label, Dot = Dot })
		else
			Log.Warn("대상 엔티티가 없습니다:", Target.Entity)
		end
	end
	-- RemoveWidget: 하나 더 만들었다 지운다
	self.entity:CloneWidget("LabelTemplate", "Temp")
	Log.Info("EngineApiTest: 이름표", #self.Items, "개, 임시 위젯 제거 =", self.entity:RemoveWidget("Temp"))
end

-- 이동(OnUpdate)·물리·트랜스폼 갱신 뒤에 놓아야 같은 프레임 위치와 맞는다
function EngineApiTest:OnLateUpdate(dt)
	for _, Item in ipairs(self.Items) do
		if Item.Entity:IsValid() then
			local Center = Item.Entity:GetWorldPosition()
			local X, Y, bVisible = Camera.WorldToScreen(Center + Vector3(0, 0, Data.LabelHeight), self.entity)
			Item.Label.Visible  = bVisible
			Item.Label.Position = Vector2(X, Y)
			local DX, DY, bDotVisible = Camera.WorldToScreen(Center, self.entity)
			Item.Dot.Visible  = bDotVisible
			Item.Dot.Position = Vector2(DX, DY)
		end
	end
end

return EngineApiTest
