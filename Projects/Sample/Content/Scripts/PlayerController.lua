-- 멀티플레이 플레이어 폰 이동 (ServerOnly: 서버에서 돈다).
-- 서버의 Input은 이 폰을 소유한 플레이어의 입력이다 (클라이언트가 매 틱 보낸다). 위치는 복제로 모두에게 보인다
local PlayerController = {
	Properties = {
		Speed = 400.0, -- cm/s
	},
}

function PlayerController:OnUpdate(dt)
	local Move = Vector3.Zero()
	if Input.IsKeyDown("W") then Move = Move + Vector3.Forward() end
	if Input.IsKeyDown("S") then Move = Move - Vector3.Forward() end
	if Input.IsKeyDown("D") then Move = Move + Vector3.Right() end
	if Input.IsKeyDown("A") then Move = Move - Vector3.Right() end
	if Move:LengthSquared() > 0 then
		self.entity:SetPosition(self.entity:GetPosition() + Move:Normalized() * (self.Properties.Speed * dt))
	end
end

-- 폰 스크립트는 생성 다음 프레임에 시작하므로 자기 폰의 OnPlayerJoined는 받지 못한다 (이미 돌던 스크립트만 받는다)
function PlayerController:OnStart()
	Log.Info("플레이어 폰 시작 — 소유 플레이어", self.entity:GetOwner())
end

return PlayerController
