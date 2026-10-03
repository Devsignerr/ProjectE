-- 데모 서브맵 Workshop: 크레인 짐 (ServerOnly). 구 관절(BreakForce)이 끊어지면 밧줄 표시(Rope 엔티티의 WorkshopRope)를 늘어진 토막으로 바꾼다
local WorkshopCraneLoad = {
	Properties = {
		Rope = "Crane_Rope",
	},
}

function WorkshopCraneLoad:OnJointBreak(Other, Force)
	Log.Info(string.format("크레인 밧줄이 끊어졌다 (힘 %.0f N)", Force))
	local Rope = Scene.Find(self.Properties.Rope)
	local Script = Rope and Rope:GetScript()
	if Script then
		Script:Detach()
	end
end

return WorkshopCraneLoad
