-- 데모 서브맵 Workshop: 밧줄 표시 (ExecutionLocation = Both — 화면 연출). 이 엔티티 = 내장 캡슐 메시, 처음 위치 = 위쪽 매단 점(월드).
--   물리 관절(거리/구 관절)은 보이지 않으므로 매 프레임(OnLateUpdate — 물리 뒤) 매단 점과 대상의 연결 점
--   (대상 위치 + 대상 Up × AttachHeight = 관절 Anchor와 같은 점) 사이로 캡슐을 늘인다.
--   Detach() 뒤(밧줄이 끊김)에는 매단 점에서 DetachedLength만큼 늘어진 토막만 남긴다
local WorkshopRope = {
	Properties = {
		Target         = "",   -- 매달린 엔티티 이름
		AttachHeight   = 0.0,  -- cm, 대상 로컬 Up 방향 연결 점 (스케일 적용된 값)
		Radius         = 1.2,  -- cm
		DetachedLength = 45.0, -- cm
	},
}

function WorkshopRope:OnStart()
	self.Anchor = self.entity:GetWorldPosition()
	self.Target = Scene.Find(self.Properties.Target)
	self.Detached = false
end

function WorkshopRope:Detach()
	self.Detached = true
end

function WorkshopRope:OnLateUpdate(dt)
	local A = self.Anchor
	local B
	if not self.Detached and self.Target and self.Target:IsValid() then
		B = self.Target:GetWorldPosition() + self.Target:GetUp() * self.Properties.AttachHeight
	else
		B = A - Vector3(0, 0, self.Properties.DetachedLength)
	end
	local D = B - A
	local L = math.max(D:Length(), 1.0)
	local R = self.Properties.Radius / 50.0 -- 내장 캡슐: 반지름 50cm, 길이 200cm (+Z 축)
	self.entity:SetPosition((A + B) * 0.5)
	self.entity:SetRotation(Quat.LookRotation(D) * Quat.FromEuler(-90.0, 0.0, 0.0)) -- +Z → +X → 밧줄 방향
	self.entity:SetScale(Vector3(R, R, L / 200.0))
end

return WorkshopRope
