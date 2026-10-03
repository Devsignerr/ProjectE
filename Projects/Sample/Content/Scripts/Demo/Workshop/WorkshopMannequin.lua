-- 데모 서브맵 Workshop: 전화하며 서 있는 마네킹 (ServerOnly). 루트 = 키네마틱 캡슐(맞기 판정), 자식 "Mesh" = 모델 + RagdollComponent.
--   무언가 MinImpactSpeed 이상으로 부딪히거나(넘어지는 선반 도미노 끝) 플레이어가 쏘면(WorkshopManager → Collapse) 래그돌로 쓰러진다.
--   쓰러지면 서 있을 때의 캡슐을 지운다 (래그돌 캡슐이 대신 부딪힌다)
local WorkshopMannequin = {
	Properties = {
		MinImpactSpeed = 60.0, -- cm/s, 부딪힌 상대 접근 속도
	},
}

function WorkshopMannequin:OnStart()
	self.Down = false
end

function WorkshopMannequin:Collapse(Reason)
	if self.Down then return end
	self.Down = true
	local Enabled = self.entity:EnableRagdoll()
	self.entity:RemoveComponent("CapsuleColliderComponent")
	self.entity:RemoveComponent("RigidBodyComponent")
	Log.Info("마네킹이 쓰러졌다 (" .. Reason .. ", 래그돌 " .. tostring(Enabled) .. ")")
end

function WorkshopMannequin:OnCollisionBegin(Other, Info)
	if Other == nil or Other:GetName() == "Player" then return end
	if Info.Speed >= self.Properties.MinImpactSpeed then
		self:Collapse(Other:GetName() .. "에 맞음")
	end
end

return WorkshopMannequin
