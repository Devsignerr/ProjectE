-- AI 데모용 "플레이어": 두 지점 사이를 왕복한다 (입력 없이도 경비가 발견/추적/포기를 반복하도록)
local PlayerWalker = {
	Properties = {
		PointA = Vector3(-800, -550, 0),
		PointB = Vector3(800, -550, 0),
		Speed  = 170.0, -- cm/s
	},
}

function PlayerWalker:OnStart()
	self.TowardB = true
end

function PlayerWalker:OnUpdate(dt)
	local Goal     = self.TowardB and self.Properties.PointB or self.Properties.PointA
	local Position = self.entity:GetPosition()
	local ToGoal   = Goal - Position
	local Distance = ToGoal:Length()
	local Step     = self.Properties.Speed * dt
	if Distance <= Step then
		self.entity:SetPosition(Goal)
		self.TowardB = not self.TowardB
		return
	end
	self.entity:SetPosition(Position + ToGoal:Normalized() * Step)
end

return PlayerWalker
