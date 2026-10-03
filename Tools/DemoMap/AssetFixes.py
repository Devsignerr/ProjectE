# 데모 생성 스크립트 공용: Poly Haven glTF를 엔진에서 바로 쓸 수 없을 때 고친 사본(JSON만)을 만든다
#   원본(.gltf/.bin/텍스처)은 저장소 밖(Scripts/FetchDemoAssets.ps1)이고 커밋되는 것은 고친 glTF JSON뿐이다.
#   사본은 원본 폴더의 버퍼·텍스처를 상대 경로로 참조한다.
import json
import os

PH = "Asset/PolyHaven"

# ---- 액자 유리 ------------------------------------------------------------------------------------------------------
# Poly Haven 2k jpg 텍스처에는 알파가 없어 "diff+opacity" 텍스처를 쓰는 유리(BLEND)가 불투명한 판이 되어 그림을 가린다.
#   유리 머티리얼만 옅은 상수 알파로 바꾼 사본을 Asset/Gallery/<Id>.gltf에 쓴다 (Gallery가 처음 만든 자리 — 다른 맵도 같은 사본을 공유)
GLASS_FIX_DIR = "Asset/Gallery"
GLASS_FIX = {"hanging_picture_frame_01", "hanging_picture_frame_02", "hanging_picture_frame_03"}


def WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def RebaseUris(Gltf, Id):
	# 사본 폴더(Asset/<폴더>/) 기준으로 원본 폴더의 버퍼·텍스처를 가리킨다
	for Entry in Gltf.get("images", []) + Gltf.get("buffers", []):
		if "uri" in Entry:
			Entry["uri"] = f"../PolyHaven/{Id}/{Entry['uri']}"


def LoadSource(Content, Id):
	with open(os.path.join(Content, *PH.split("/"), Id, f"{Id}.gltf"), encoding="utf-8") as File:
		return json.load(File)


def WriteGlassFixedModel(Content, Id):
	assert Id in GLASS_FIX, f"{Id}: 유리 고침 대상 아님"
	Gltf = LoadSource(Content, Id)
	for Material in Gltf["materials"]:
		if Material.get("alphaMode") == "BLEND":
			Material["pbrMetallicRoughness"] = {"baseColorFactor": [0.02, 0.02, 0.02, 0.1], "metallicFactor": 0.0, "roughnessFactor": 0.05}
			Material.pop("normalTexture", None)
			Material.pop("extensions", None)
	RebaseUris(Gltf, Id)
	WriteJson(os.path.join(Content, *GLASS_FIX_DIR.split("/"), f"{Id}.gltf"), Gltf)


def FrameModel(Id):
	# 액자 모델 경로: 유리를 고친 것은 사본, 나머지는 원본
	return f"{GLASS_FIX_DIR}/{Id}.gltf" if Id in GLASS_FIX else f"{PH}/{Id}/{Id}.gltf"

