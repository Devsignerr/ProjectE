-- 계단 위 파수꾼 (Training 데모, ServerOnly): 돌계단 두 단에 걸쳐 서 있다 (발 IK가 골반을 내리고 발을 단에 맞춘다).
--   평소에는 대련장을 지켜보고(시선 IK), 플레이어가 가까이 오면 플레이어를 본다
local Lookout = {
	Properties = {
		WatchName   = "Sparring_A",
		PlayerName  = "Player",
		NoticeRange = 600.0,
	},
}

function Lookout:OnStart()
	self.Mesh = self.entity:FindChild("Mesh")
	self.Watch = Scene.Find(self.Properties.WatchName)
end

function Lookout:OnUpdate(dt)
	if not self.Mesh then return end
	local Player = Scene.Find(self.Properties.PlayerName)
	if Player and Player:IsValid() then
		local D = Player:GetWorldPosition() - self.entity:GetWorldPosition()
		if D:Length() < self.Properties.NoticeRange then
			self.Mesh:SetLookAtTarget(Player:GetWorldPosition() + Vector3(0, 0, 65))
			return
		end
	end
	if self.Watch and self.Watch:IsValid() then
		self.Mesh:SetLookAtTarget(self.Watch:GetWorldPosition() + Vector3(0, 0, 140))
	else
		self.Mesh:ClearLookAtTarget()
	end
end

return Lookout
