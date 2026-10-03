# 데모 맵 씬(.escene) 생성 도우미 — 배치를 코드로 기술해 JSON을 만든다 (씬 파일을 손으로 고치지 않기 위함)
# 좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 회전은 Yaw(도, +Z 축) 위주
import json
import math


def QuatFromEuler(Pitch=0.0, Yaw=0.0, Roll=0.0):
	# FQuat::FromEuler와 같은 규약(UE): Yaw(Z) * Pitch(Y) * Roll(X) — 성분 순서 [X, Y, Z, W]
	P, Y, R = math.radians(Pitch) * 0.5, math.radians(Yaw) * 0.5, math.radians(Roll) * 0.5
	SP, CP = math.sin(P), math.cos(P)
	SY, CY = math.sin(Y), math.cos(Y)
	SR, CR = math.sin(R), math.cos(R)
	X = CR * SP * SY - SR * CP * CY
	Y_ = -CR * SP * CY - SR * CP * SY
	Z = CR * CP * SY - SR * SP * CY
	W = CR * CP * CY + SR * SP * SY
	return [X, Y_, Z, W]


class FScene:
	def __init__(self):
		self.Entities = []

	def Add(self, Name, Components, Position=(0, 0, 0), Rotation=None, Scale=(1, 1, 1), Parent=-1):
		Comps = dict(Components)
		Comps["TransformComponent"] = {
			"Position": [float(V) for V in Position],
			"Rotation": Rotation if Rotation is not None else [0.0, 0.0, 0.0, 1.0],
			"Scale": [float(V) for V in Scale],
		}
		self.Entities.append({"Components": Comps, "Name": Name, "Parent": Parent})
		return len(self.Entities) - 1

	def Model(self, Name, Asset, Position, Yaw=0.0, Scale=1.0, Parent=-1, Extra=None):
		Comps = {"ModelComponent": {"AssetPath": Asset}}
		if Extra:
			Comps.update(Extra)
		S = Scale if isinstance(Scale, (list, tuple)) else (Scale, Scale, Scale)
		return self.Add(Name, Comps, Position, QuatFromEuler(Yaw=Yaw), S, Parent)

	def Save(self, Path):
		with open(Path, "w", encoding="utf-8", newline="\n") as File:
			json.dump({"Entities": self.Entities, "Version": 1}, File, indent=2, ensure_ascii=False)
			File.write("\n")
