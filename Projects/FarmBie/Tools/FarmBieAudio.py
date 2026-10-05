# FarmBie 효과음·환경음 절차 합성 (자체 제작 — 라이선스 제약 없음). BuildFarmBie.py가 부른다. 결과: Content/Audio/FarmBie/*.wav
#   형식: 22050Hz 모노 16비트 WAV (miniaudio 빌드에 Vorbis 디코더가 없어 .ogg 대신 WAV). 결정적(고정 시드).
#   그 밖에 Kenney Interface/RPG Audio(CC0)와 Sample의 자체 제작 전투 효과음을 Content/Audio/FarmBie/로 복사한다 (CREDITS.md)
import math
import os
import shutil
import wave

import numpy as np

RATE = 22050


def _Write(Path, Samples):
	Samples = np.clip(Samples, -1.0, 1.0)
	Data = (Samples * 32000).astype("<i2").tobytes()
	with wave.open(Path, "wb") as W:
		W.setnchannels(1)
		W.setsampwidth(2)
		W.setframerate(RATE)
		W.writeframes(Data)


def _T(Seconds):
	return np.arange(int(RATE * Seconds)) / RATE


def _Env(T, Attack, Decay):
	return np.minimum(1.0, T / max(Attack, 1e-4)) * np.exp(-T / max(Decay, 1e-4))


def _Noise(N, Seed):
	return np.random.default_rng(Seed).uniform(-1.0, 1.0, N)


def _LowPass(X, Alpha):
	Y = np.zeros_like(X)
	Acc = 0.0
	for I, V in enumerate(X):
		Acc += Alpha * (V - Acc)
		Y[I] = Acc
	return Y


def Dig():
	T = _T(0.28)
	N = _LowPass(_Noise(len(T), 1), 0.25) * _Env(T, 0.005, 0.06)
	Thud = np.sin(2 * math.pi * 110 * T) * _Env(T, 0.002, 0.05) * 0.6
	return (N * 1.6 + Thud) * 0.8


def Water():
	T = _T(0.55)
	N = _Noise(len(T), 2)
	Hi = N - _LowPass(N, 0.35)
	Mod = 0.6 + 0.4 * np.sin(2 * math.pi * 23 * T)
	return Hi * Mod * _Env(T, 0.04, 0.25) * 0.7


def Pop(Pitch=520.0, Seed=3):
	T = _T(0.22)
	F = Pitch * (1.0 + 0.8 * np.exp(-T * 30))
	S = np.sin(2 * math.pi * np.cumsum(F) / RATE) * _Env(T, 0.002, 0.07)
	return S * 0.7 + _LowPass(_Noise(len(T), Seed), 0.4) * _Env(T, 0.001, 0.02) * 0.4


def Groan(Seed):
	T = _T(1.1)
	Rng = np.random.default_rng(Seed)
	Base = 70 + Rng.uniform(-10, 20)
	F = Base * (1.0 + 0.15 * np.sin(2 * math.pi * 2.2 * T + Rng.uniform(0, 6)))
	Phase = np.cumsum(F) / RATE
	S = sum(np.sin(2 * math.pi * K * Phase) / K for K in range(1, 7))
	Breath = _LowPass(_Noise(len(T), Seed + 10), 0.15) * 0.5
	return (S * 0.35 + Breath) * _Env(T, 0.15, 0.55) * 0.9


def Boom():
	T = _T(1.3)
	N = _LowPass(_Noise(len(T), 5), 0.08) * 3.0
	Low = np.sin(2 * math.pi * 48 * T * (1 + np.exp(-T * 6))) * 0.8
	return (N + Low) * _Env(T, 0.003, 0.35) * 0.8


def Bell():
	T = _T(2.6)
	S = sum(A * np.sin(2 * math.pi * F * T) * np.exp(-T * D) for F, A, D in ((330, 0.6, 1.2), (660, 0.3, 1.8), (831, 0.18, 2.4), (1250, 0.1, 3.0)))
	return S * _Env(T, 0.003, 10.0) * 0.8


def Alarm():
	T = _T(1.0)
	F = np.where((T * 6).astype(int) % 2 == 0, 880.0, 660.0)
	S = np.sign(np.sin(2 * math.pi * np.cumsum(F) / RATE)) * 0.25
	return _LowPass(S, 0.3) * _Env(T, 0.01, 0.8)


def Shot():
	T = _T(0.18)
	N = _Noise(len(T), 7)
	return (N - _LowPass(N, 0.5)) * _Env(T, 0.001, 0.03) * 1.2 + np.sin(2 * math.pi * 300 * T) * _Env(T, 0.001, 0.02) * 0.3


def Roar():
	T = _T(2.2)
	F = 55 * (1.0 + 0.3 * np.sin(2 * math.pi * 0.7 * T)) * (1 + 0.3 * np.exp(-T))
	Phase = np.cumsum(F) / RATE
	S = sum(np.sin(2 * math.pi * K * Phase) / (K ** 0.8) for K in range(1, 10))
	Grit = _LowPass(_Noise(len(T), 9), 0.3) * 0.8
	return (S * 0.3 + Grit) * _Env(T, 0.25, 1.1) * 0.9


def Rooster():
	T = _T(1.2)
	F = np.interp(T, [0, 0.15, 0.35, 0.8, 1.2], [500, 900, 1100, 950, 600])
	S = np.sin(2 * math.pi * np.cumsum(F) / RATE)
	S = S + 0.4 * np.sin(4 * math.pi * np.cumsum(F) / RATE)
	return S * _Env(T, 0.05, 0.5) * 0.35


def Sting():
	T = _T(3.0)
	S = sum(np.sin(2 * math.pi * F * T) * np.exp(-T * 0.8) for F in (110, 130.8, 155.6, 220))
	return S * 0.22 * _Env(T, 0.2, 2.0)


def AmbientDay():
	# 4초 이음매 없는 반복: 부드러운 바람 + 가끔 새소리
	T = _T(8.0)
	Wind = _LowPass(_Noise(len(T), 11), 0.02) * 1.6
	Wind *= 0.6 + 0.4 * np.sin(2 * math.pi * T / 8.0) ** 2
	Birds = np.zeros_like(T)
	Rng = np.random.default_rng(12)
	for _ in range(5):
		Start = Rng.uniform(0.2, 7.2)
		Tw = _T(0.25)
		F = Rng.uniform(2200, 3400) * (1 + 0.15 * np.sin(2 * math.pi * 18 * Tw))
		Chirp = np.sin(2 * math.pi * np.cumsum(F) / RATE) * _Env(Tw, 0.01, 0.08) * 0.12
		I = int(Start * RATE)
		Birds[I:I + len(Chirp)] += Chirp[:len(Birds) - I]
	Fade = np.minimum(1.0, np.minimum(T, 8.0 - T) / 0.3)
	return (Wind * 0.25 + Birds) * Fade


def AmbientNight():
	# 낮은 웅웅거림 + 귀뚜라미
	T = _T(8.0)
	Drone = sum(np.sin(2 * math.pi * F * T) * A for F, A in ((55, 0.12), (82.5, 0.06), (110.5, 0.04)))
	Drone *= 0.7 + 0.3 * np.sin(2 * math.pi * T / 8.0)
	Cricket = np.sin(2 * math.pi * 4300 * T) * ((np.sin(2 * math.pi * 30 * T) > 0.6) * (np.sin(2 * math.pi * 0.5 * T) > -0.2)) * 0.035
	Fade = np.minimum(1.0, np.minimum(T, 8.0 - T) / 0.3)
	return (Drone + Cricket) * Fade


SOUNDS = {
	"Dig": Dig, "Water": Water, "Harvest": lambda: Pop(560.0, 3), "Plant": lambda: Pop(380.0, 4), "Boom": Boom, "Bell": Bell, "Alarm": Alarm,
	"Shot": Shot, "Roar": Roar, "Rooster": Rooster, "Sting": Sting, "AmbientDay": AmbientDay, "AmbientNight": AmbientNight,
	"Groan0": lambda: Groan(21), "Groan1": lambda: Groan(22), "Groan2": lambda: Groan(23),
}

COPIES = {
	# Kenney (CC0)
	"Asset/Kenney_InterfaceSounds/click.wav": "Click.wav", "Asset/Kenney_InterfaceSounds/open.wav": "Open.wav",
	"Asset/Kenney_InterfaceSounds/close.wav": "Close.wav", "Asset/Kenney_InterfaceSounds/confirm.wav": "Confirm.wav",
	"Asset/Kenney_InterfaceSounds/error.wav": "Error.wav", "Asset/Kenney_InterfaceSounds/pickup_item.wav": "Pickup.wav",
	"Asset/Kenney_InterfaceSounds/potion.wav": "Eat.wav", "Asset/Kenney_RPGAudio/coins_buy.wav": "Buy.wav",
	"Asset/Kenney_RPGAudio/coins_pickup.wav": "Coins.wav", "Asset/Kenney_RPGAudio/equip.wav": "Equip.wav",
	# Sample 자체 제작 전투음
	"Audio/RPG/Swing1.wav": "Swing.wav", "Audio/RPG/Hit.wav": "Hit.wav", "Audio/RPG/HitHeavy.wav": "HitHeavy.wav",
	"Audio/RPG/Hurt.wav": "Hurt.wav", "Audio/RPG/Dodge.wav": "Dodge.wav",
}


def WriteAll(Content, SampleContent):
	Folder = os.path.join(Content, "Audio", "FarmBie")
	os.makedirs(Folder, exist_ok=True)
	for Name, Make in SOUNDS.items():
		_Write(os.path.join(Folder, f"{Name}.wav"), Make())
	for Src, Dst in COPIES.items():
		shutil.copyfile(os.path.join(SampleContent, *Src.split("/")), os.path.join(Folder, Dst))
