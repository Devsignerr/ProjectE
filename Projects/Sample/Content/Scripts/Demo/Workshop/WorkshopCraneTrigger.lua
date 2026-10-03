-- 데모 서브맵 Workshop: 크레인 아래 위험 구역 트리거 (ServerOnly, 이 엔티티 = 트리거 박스 콜라이더).
--   처음 들어온 것(굴러온 드럼통이나 플레이어)이 있으면 크레인에 매달린 짐을 아래로 세게 당긴다 → 구 관절이 BreakForce를 넘어 끊어지고
--   (짐 엔티티 OnJointBreak) 짐이 아래 상자 더미로 떨어진다. 한 번만 동작한다
local WorkshopCraneTrigger = {
	Properties = {
		Load      = "Crane_Load", -- 매달린 짐 엔티티 이름 (구 관절 + BreakForce)
		PullSpeed = 700.0,        -- cm/s, 짐에 주는 아래쪽 속도 변화 (충격량 = 질량 × 이 값)
	},
}

function WorkshopCraneTrigger:OnStart()
	self.Fired = false
end

function WorkshopCraneTrigger:OnTriggerEnter(Other)
	if self.Fired then return end
	local Load = Scene.Find(self.Properties.Load)
	if Load == nil then return end
	self.Fired = true
	Log.Info("크레인 위험 구역에 " .. (Other and Other:GetName() or "?") .. " 진입 → 짐 밧줄을 끊는다")
	Load:AddImpulse(Vector3(0, 0, -Load:GetMass() * self.Properties.PullSpeed))
end

return WorkshopCraneTrigger
