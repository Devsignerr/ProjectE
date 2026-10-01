-- 트리거 영역 예제 (Phase 30-1): 콜라이더 IsTrigger를 켠 엔티티에 붙인다. 무언가 들어와 있는 동안 램프(Lamp 이름의 엔티티)를 켠다
local TriggerLamp = {
	Properties = {
		Lamp = "", -- 켜고 끌 엔티티 이름 (StaticMeshComponent)
	},
}

function TriggerLamp:OnStart()
	self.Count = 0
	self:Apply()
end

function TriggerLamp:Apply()
	local Lamp = Scene.Find(self.Properties.Lamp)
	if Lamp then
		Lamp:GetComponent("StaticMeshComponent").Visible = self.Count > 0
	end
end

function TriggerLamp:OnTriggerEnter(other)
	self.Count = self.Count + 1
	Log.Info(self.entity:GetName(), "들어옴:", other and other:GetName() or "?")
	self:Apply()
end

function TriggerLamp:OnTriggerExit(other)
	self.Count = math.max(0, self.Count - 1)
	self:Apply()
end

return TriggerLamp
