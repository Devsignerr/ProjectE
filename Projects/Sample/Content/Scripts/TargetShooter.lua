-- 게임플레이 예제 포탑 (서버): 매치 진행 중 살아 있는 표적을 차례로 겨눠 데미지를 준다.
--   entity:ApplyDamage(양, 가해자) → 표적 OnDamaged/OnDeath, 점수는 게임 모드가 가해자 소유 플레이어에게 준다
--   (Standalone은 소유자 없는 가해자 = 로컬 플레이어 0)
local TargetShooter = {
	Properties = {
		Interval = 0.3,        -- 초
		Damage   = 20.0,
		Count    = 6,          -- 표적 수 ("Target 1" ~ "Target N")
		Prefix   = "Target ",
	},
}

function TargetShooter:OnStart()
	self.Timer = 0
	self.Index = 0
end

function TargetShooter:OnUpdate(dt)
	if GameMode.GetState() ~= "InProgress" then
		return
	end
	self.Timer = self.Timer + dt
	if self.Timer < self.Properties.Interval then
		return
	end
	self.Timer = 0

	local P = self.Properties
	for _ = 1, P.Count do
		self.Index = self.Index % P.Count + 1
		local Target = Scene.Find(P.Prefix .. self.Index)
		if Target and not Target:IsDead() then
			local Direction = Target:GetPosition() - self.entity:GetPosition()
			self.entity:SetRotation(Quat.LookRotation(Vector3(Direction.X, Direction.Y, 0)))
			Target:ApplyDamage(P.Damage, self.entity)
			return
		end
	end
end

return TargetShooter
