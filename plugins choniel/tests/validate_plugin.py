"""Exercise the real x64 DLL through its public host ABI; no SDK required.

Run from the project root: python tests/validate_plugin.py
Requires NumPy and Pillow for signal checks and the editor capture only.
"""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time
import unittest
import wave

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DLL = ROOT / "dist" / "BeatWeaver_x64.dll"
RATE = 48000


class Effect(C.Structure):
    pass


EP = C.POINTER(Effect)
CALLBACK = C.CFUNCTYPE(C.c_ssize_t, EP, C.c_int32, C.c_int32, C.c_ssize_t, C.c_void_p, C.c_float)
FLOATS = C.POINTER(C.c_float)
PROCESS = C.CFUNCTYPE(None, EP, C.POINTER(FLOATS), C.POINTER(FLOATS), C.c_int32)
SET = C.CFUNCTYPE(None, EP, C.c_int32, C.c_float)
GET = C.CFUNCTYPE(C.c_float, EP, C.c_int32)
Effect._fields_ = [
    ("magic", C.c_int32), ("dispatch", CALLBACK), ("process", PROCESS),
    ("set", SET), ("get", GET), ("programs", C.c_int32), ("params", C.c_int32),
    ("inputs", C.c_int32), ("outputs", C.c_int32), ("flags", C.c_int32),
    ("reserved1", C.c_ssize_t), ("reserved2", C.c_ssize_t), ("latency", C.c_int32),
    ("realQual", C.c_int32), ("offQual", C.c_int32), ("ratio", C.c_float),
    ("object", C.c_void_p), ("user", C.c_void_p), ("id", C.c_int32),
    ("version", C.c_int32), ("replace", PROCESS), ("double", C.c_void_p), ("future", C.c_char * 56),
]


class TimeInfo(C.Structure):
    _fields_ = [(n, C.c_double) for n in
                "samplePos sampleRate nanoSeconds ppqPos tempo barStartPos cycleStartPos cycleEndPos".split()] + [
                    (n, C.c_int32) for n in "numerator denominator smpteOffset smpteRate nextClock flags".split()]


class Midi(C.Structure):
    _fields_ = [(n, C.c_int32) for n in "type byteSize deltaFrames flags noteLength noteOffset".split()] + [
        ("data", C.c_uint8 * 4), ("detune", C.c_int8), ("velocity", C.c_uint8),
        ("reserved1", C.c_uint8), ("reserved2", C.c_uint8)]


class Events(C.Structure):
    _fields_ = [("count", C.c_int32), ("reserved", C.c_ssize_t), ("events", C.c_void_p * 256)]


def checksum(data):
    h = 2166136261
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xffffffff
    return h


def unpack_state(data):
    magic, version, count, slots = struct.unpack_from("<4I", data)
    assert magic == 0x31565742 and version == 1 and slots == 72
    params = list(struct.unpack_from(f"<{count}f", data, 16))
    offset = 16 + count * 4
    envelopes = []
    for _ in range(slots):
        nodes, name = struct.unpack_from("<I32s", data, offset)
        offset += 36
        points = []
        for _ in range(nodes):
            points.append(list(struct.unpack_from("<fffI", data, offset)))
            offset += 16
        envelopes.append([name, points])
    assert offset + 4 == len(data)
    return params, envelopes


def pack_state(params, envelopes):
    result = bytearray(struct.pack("<4I", 0x31565742, 1, len(params), len(envelopes)))
    result += struct.pack(f"<{len(params)}f", *params)
    for name, points in envelopes:
        result += struct.pack("<I32s", len(points), name)
        for point in points:
            result += struct.pack("<fffI", *point)
    result += struct.pack("<I", checksum(result))
    return bytes(result)


class Host:
    library = None

    def __init__(self, rate=RATE):
        if Host.library is None:
            Host.library = C.WinDLL(str(DLL))
            Host.library.VSTPluginMain.argtypes = [CALLBACK]
            Host.library.VSTPluginMain.restype = EP
        self.time = TimeInfo()
        self.time.sampleRate = rate
        self.time.tempo = 120
        self.time.numerator = self.time.denominator = 4
        self.time.flags = 2 | 512 | 1024
        self.rate = rate
        self.automation = []
        self.callback = CALLBACK(self.host)
        self.pointer = Host.library.VSTPluginMain(self.callback)
        if not self.pointer:
            raise RuntimeError("VSTPluginMain refused host")
        self.effect = self.pointer.contents
        self.call(0)
        self.call(10, option=float(rate))
        self.call(11, value=4096)

    def __enter__(self): return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self.pointer:
            self.call(15)
            self.call(1)
            self.pointer = None

    def host(self, effect, opcode, index, value, pointer, option):
        if opcode == 1: return 2400
        if opcode == 7: return C.addressof(self.time)
        if opcode == 16: return self.rate
        if opcode == 17: return 4096
        if opcode == 0: self.automation.append((index, option))
        return 0

    def call(self, opcode, index=0, value=0, pointer=None, option=0):
        return self.effect.dispatch(self.pointer, opcode, index, value, pointer, option)

    def param(self, index, value=None):
        if value is not None: self.effect.set(self.pointer, index, value)
        return self.effect.get(self.pointer, index)

    def state(self):
        ptr = C.c_void_p()
        size = self.call(23, pointer=C.byref(ptr))
        assert size > 0
        return C.string_at(ptr, size)

    def restore(self, state):
        raw = C.create_string_buffer(bytes(state))
        return self.call(24, value=len(state), pointer=raw)

    def envelope(self, volume, slot, points, name=b"Test envelope"):
        params, envelopes = unpack_state(self.state())
        envelopes[36 * bool(volume) + slot] = [name, points]
        assert self.restore(pack_state(params, envelopes)) == 1

    def transparent(self):
        for key, value in {0: 1, 1: 1, 2: 0, 3: 0, 4: .5, 5: 0, 6: 0, 7: 0,
                           8: 0, 9: 4/6, 10: 0, 11: 0, 12: 0, 13: 0, 15: 0,
                           16: 0, 17: 0, 19: 0, 20: .5}.items():
            self.param(key, value)
        self.call(12, value=1)

    def midi(self, messages):
        events = Events()
        instances = []
        for offset, note, on in messages:
            midi = Midi()
            midi.type, midi.byteSize, midi.deltaFrames = 1, C.sizeof(Midi), offset
            midi.data[:] = [0x90 if on else 0x80, note, 127 if on else 0, 0]
            instances.append(midi)
            events.events[events.count] = C.addressof(midi)
            events.count += 1
        assert self.call(25, pointer=C.byref(events)) == 1

    def process(self, signal, additive=False):
        signal = np.ascontiguousarray(signal, dtype=np.float32)
        assert signal.ndim == 2 and signal.shape[0] == 2
        out = np.full_like(signal, .125 if additive else 0)
        ip = (FLOATS * 2)(signal[0].ctypes.data_as(FLOATS), signal[1].ctypes.data_as(FLOATS))
        op = (FLOATS * 2)(out[0].ctypes.data_as(FLOATS), out[1].ctypes.data_as(FLOATS))
        (self.effect.process if additive else self.effect.replace)(self.pointer, ip, op, signal.shape[1])
        self.time.samplePos += signal.shape[1]
        self.time.ppqPos += signal.shape[1] / self.rate * self.time.tempo / 60
        self.time.flags &= ~1
        return out


class PluginTests(unittest.TestCase):
    def test_x64_binary_and_host_metadata(self):
        binary = DLL.read_bytes()
        pe = struct.unpack_from("<I", binary, 60)[0]
        self.assertEqual(binary[pe:pe+4], b"PE\0\0")
        self.assertEqual(struct.unpack_from("<H", binary, pe+4)[0], 0x8664)
        self.assertEqual(C.sizeof(Effect), 192)
        with Host() as h:
            self.assertEqual(h.effect.magic, 0x56737450)
            self.assertEqual((h.effect.inputs, h.effect.outputs, h.effect.params), (2, 2, 21))
            self.assertTrue(h.effect.flags & 16)
            self.assertEqual(h.call(58), 2400)
            for opcode, name in [(45,b"BeatWeaver"), (47,b"Choniel Audio")]:
                text = C.create_string_buffer(64)
                self.assertEqual(h.call(opcode, pointer=text), 1)
                self.assertEqual(text.value, name)
            for i in range(21):
                self.assertTrue(0 <= h.param(i) <= 1)
                h.param(i, float("nan"))
                self.assertTrue(math.isfinite(h.param(i)))
            self.assertEqual(h.param(-1), 0)
            self.assertEqual(h.param(200), 0)

    def test_transparent_stereo_and_additive_processing(self):
        rng = np.random.default_rng(91)
        signal = rng.uniform(-.7,.7,(2,4096)).astype(np.float32)
        with Host() as h:
            h.transparent()
            result = h.process(signal)
            np.testing.assert_allclose(result, signal, atol=2e-5, rtol=0)
            result = h.process(signal, additive=True)
            np.testing.assert_allclose(result, signal + .125, atol=2e-5, rtol=0)

    def test_state_roundtrip_and_transactional_corrupt_rejection(self):
        with Host() as h:
            h.param(0,.37)
            h.envelope(True, 13, [[0,.2,.7,3],[.375,1,0,1],[1,0,0,0]], b"Custom gate")
            state = h.state()
            with Host() as other:
                self.assertEqual(other.restore(state), 1)
                self.assertEqual(other.state(), state)
            damaged = bytearray(state); damaged[44] ^= 32
            self.assertEqual(h.restore(damaged), 0)
            self.assertEqual(h.state(), state)
            params, env = unpack_state(state)
            env[0][1][0][0] = float("nan")
            self.assertEqual(h.restore(pack_state(params,env)),0)
            self.assertEqual(h.state(),state)
            self.assertEqual(h.restore(state[:19]),0)

    def test_volume_silence_and_bypass(self):
        with Host() as h:
            h.transparent()
            h.envelope(True,0,[[0,0,0,0],[1,0,0,0]])
            signal = np.full((2,4096),.3,dtype=np.float32)
            output = h.process(signal)
            self.assertLess(np.max(np.abs(output[:,-1024:])),1e-5)
            h.call(44,value=1)
            output = h.process(signal)
            np.testing.assert_allclose(output[:,-1024:],signal[:,-1024:],atol=1e-5,rtol=0)

    def test_constant_delay_impulse_location(self):
        with Host() as h:
            h.transparent()
            delay = 512
            value = delay / (RATE * 4)
            h.envelope(False,0,[[0,value,0,0],[1,value,0,0]])
            signal=np.zeros((2,4096),dtype=np.float32)
            signal[0,100]=.8; signal[1,137]=-.6
            output=h.process(signal)
            self.assertEqual(int(np.argmax(output[0])),612)
            self.assertEqual(int(np.argmin(output[1])),649)
            self.assertAlmostEqual(float(output[0,612]),.8,places=4)
            self.assertAlmostEqual(float(output[1,649]),-.6,places=4)

    def test_half_speed_pitch(self):
        with Host() as h:
            h.transparent()
            h.param(5,1/35)  # factory Half Speed map
            output=[]
            for start in range(0,RATE,1024):
                count=min(1024,RATE-start)
                sine=.3*np.sin(2*np.pi*440*(np.arange(count)+start)/RATE)
                output.append(h.process(np.array([sine,sine],dtype=np.float32))[0])
            signal=np.concatenate(output)[RATE//4:]
            spectrum=np.abs(np.fft.rfft(signal*np.hanning(len(signal))))
            frequency=np.fft.rfftfreq(len(signal),1/RATE)[np.argmax(spectrum)]
            self.assertLess(abs(frequency-220),3)

    def test_midi_sample_offsets_and_key_release(self):
        with Host() as h:
            h.transparent()
            h.param(11,1)
            h.envelope(True,1,[[0,0,0,0],[1,0,0,0]])
            # Numeric MIDI 12..47 is volume; 48..83 is time.
            h.midi([(768,13,True)])
            out=h.process(np.full((2,2048),.25,dtype=np.float32))
            np.testing.assert_allclose(out[:,:700],.25,atol=1e-5)
            self.assertLess(np.max(np.abs(out[:,1500:])),1e-5)
            h.midi([(512,13,False)])
            out=h.process(np.full((2,2048),.25,dtype=np.float32))
            np.testing.assert_allclose(out[:,1500:],.25,atol=1e-5)

    def test_legacy_additive_midi_late_offset(self):
        with Host() as h:
            h.transparent()
            h.envelope(True,1,[[0,0,0,0],[1,0,0,0]])
            h.midi([(768,13,True)])
            out=h.process(np.full((2,2048),.25,dtype=np.float32),additive=True)
            np.testing.assert_allclose(out[:,:700],.375,atol=1e-5)
            np.testing.assert_allclose(out[:,1500:],.125,atol=1e-5)

    def test_transport_seek_and_all_presets_are_finite(self):
        with Host() as h:
            h.transparent()
            for slot in range(36):
                h.param(5,slot/35); h.param(6,(35-slot)/35)
                if slot % 7 == 0:
                    h.time.ppqPos = slot * 16
                    h.time.flags |= 1
                t=np.arange(512)/RATE
                out=h.process(np.array([.2*np.sin(2*np.pi*440*t),.2*np.cos(2*np.pi*220*t)],dtype=np.float32))
                self.assertTrue(np.all(np.isfinite(out)))
                self.assertLess(np.max(np.abs(out)),2)
            h.time.flags &= ~2
            self.assertTrue(np.all(np.isfinite(h.process(np.zeros((2,512),dtype=np.float32)))))

    def test_hold_last_freezes_audio_and_release_returns_live(self):
        with Host() as h:
            h.transparent(); h.param(9,0)  # quarter beat = 6000 samples at 120 BPM.
            phrase=np.random.default_rng(34).uniform(-.4,.4,(2,6000)).astype(np.float32)
            h.process(phrase)
            h.param(14,.5); h.param(13,1)
            out=h.process(np.full((2,18000),.9,dtype=np.float32))
            np.testing.assert_allclose(out,np.tile(phrase,(1,3)),atol=2e-5,rtol=0)
            h.param(13,0)
            out=h.process(np.full((2,1024),-.2,dtype=np.float32))
            np.testing.assert_allclose(out,-.2,atol=2e-5,rtol=0)

    def test_hold_next_captures_upcoming_audio(self):
        with Host() as h:
            h.transparent(); h.param(9,0)
            h.process(np.full((2,6000),.8,dtype=np.float32))
            h.param(14,1); h.param(13,1)
            phrase=np.random.default_rng(25).uniform(-.4,.4,(2,6000)).astype(np.float32)
            capture=h.process(phrase)
            np.testing.assert_allclose(capture,phrase,atol=2e-5,rtol=0)
            out=h.process(np.zeros((2,12000),dtype=np.float32))
            np.testing.assert_allclose(out,np.tile(phrase,(1,2)),atol=2e-5,rtol=0)

    def test_trigger_waits_for_next_beat(self):
        with Host() as h:
            h.transparent(); h.param(7,5/8)
            h.envelope(True,1,[[0,0,0,0],[1,0,0,0]])
            h.time.ppqPos=.25
            h.param(6,1/35)
            out=h.process(np.full((2,24000),.25,dtype=np.float32))
            np.testing.assert_allclose(out[:,:17990],.25,atol=1e-5)
            np.testing.assert_allclose(out[:,18010:],0,atol=1e-5)

    def test_one_shot_returns_to_neutral_slot(self):
        with Host() as h:
            h.transparent(); h.param(9,0); h.param(12,1)
            h.envelope(True,1,[[0,0,0,0],[1,0,0,0]])
            h.param(6,1/35)
            out=h.process(np.full((2,8000),.25,dtype=np.float32))
            np.testing.assert_allclose(out[:,:5990],0,atol=1e-5)
            np.testing.assert_allclose(out[:,6010:],.25,atol=1e-5)
            self.assertEqual(h.param(6),0)

    def test_all_curve_modes_and_tension_restore_and_process(self):
        with Host() as h:
            h.transparent()
            for mode in range(12):
                for tension in [-1,0,1]:
                    h.envelope(True,0,[[0,0,tension,mode],[1,1,0,0]])
                    out=h.process(np.full((2,4096),.5,dtype=np.float32))
                    self.assertTrue(np.all(np.isfinite(out)))
                    self.assertGreaterEqual(float(out.min()),0)
                    self.assertLessEqual(float(out.max()),.50001)

    def test_reverse_reads_earlier_samples_in_reverse_order(self):
        with Host(rate=8000) as h:
            h.transparent(); h.param(9,0)
            priming=np.arange(5000,dtype=np.float32)/10000
            h.process(np.array([priming,priming]))
            h.param(5,2/35)
            upcoming=np.arange(5000,5500,dtype=np.float32)/10000
            out=h.process(np.array([upcoming,upcoming]))
            expected=np.arange(5000,4500,-1,dtype=np.float32)/10000
            np.testing.assert_allclose(out[0],expected,atol=1e-5,rtol=0)

    def test_link_accepts_automation_from_volume_bank(self):
        with Host() as h:
            h.transparent(); h.param(10,1); h.param(6,34/35)
            out=h.process(np.full((2,2048),.25,dtype=np.float32))
            self.assertAlmostEqual(h.param(5),34/35,places=6)
            self.assertAlmostEqual(h.param(6),34/35,places=6)
            np.testing.assert_allclose(out[:,512:],0,atol=1e-5)

    def test_seek_preserves_pending_slot_selection(self):
        with Host(rate=8000) as h:
            h.transparent(); h.param(7,5/8)
            h.midi([(128,46,True)])  # Volume Silence queued for beat 1.
            first=h.process(np.full((2,1000),.25,dtype=np.float32))
            np.testing.assert_allclose(first,.25,atol=1e-5)
            h.time.ppqPos=8; h.time.flags|=1
            out=h.process(np.full((2,1000),.25,dtype=np.float32))
            np.testing.assert_allclose(out,0,atol=1e-5)


def capture_editor():
    from PIL import Image
    user=C.WinDLL("user32",use_last_error=True)
    gdi=C.WinDLL("gdi32",use_last_error=True)
    user.CreateWindowExW.argtypes=[W.DWORD,W.LPCWSTR,W.LPCWSTR,W.DWORD,C.c_int,C.c_int,C.c_int,C.c_int,W.HWND,W.HMENU,W.HINSTANCE,C.c_void_p]
    user.CreateWindowExW.restype=W.HWND
    user.FindWindowExW.argtypes=[W.HWND,W.HWND,W.LPCWSTR,W.LPCWSTR]; user.FindWindowExW.restype=W.HWND
    user.GetDC.argtypes=[W.HWND]; user.GetDC.restype=W.HDC
    user.PrintWindow.argtypes=[W.HWND,W.HDC,W.UINT]; user.PrintWindow.restype=W.BOOL
    user.DestroyWindow.argtypes=[W.HWND]
    user.ReleaseDC.argtypes=[W.HWND,W.HDC]
    gdi.CreateCompatibleDC.argtypes=[W.HDC]; gdi.CreateCompatibleDC.restype=W.HDC
    gdi.CreateDIBSection.argtypes=[W.HDC,C.c_void_p,W.UINT,C.POINTER(C.c_void_p),W.HANDLE,W.DWORD]; gdi.CreateDIBSection.restype=W.HBITMAP
    gdi.SelectObject.argtypes=[W.HDC,W.HGDIOBJ]; gdi.SelectObject.restype=W.HGDIOBJ
    gdi.DeleteObject.argtypes=[W.HGDIOBJ]; gdi.DeleteDC.argtypes=[W.HDC]
    user.SendMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]; user.SendMessageW.restype=C.c_ssize_t
    parent=user.CreateWindowExW(0,"STATIC","BeatWeaver validation",0,0,0,1120,740,None,None,None,None)
    assert parent
    try:
        with Host() as host:
            rect=C.c_void_p()
            assert host.call(13,pointer=C.byref(rect))==1
            top,left,bottom,right=struct.unpack("<4h",C.string_at(rect,8))
            width,height=right-left,bottom-top
            assert (width,height)==(1120,740)
            assert host.call(14,pointer=parent)==1
            child=user.FindWindowExW(parent,None,None,None)
            assert child
            screen=user.GetDC(None)
            dc=gdi.CreateCompatibleDC(screen)
            bitmap_info=struct.pack("<IiiHHIIiiII",40,width,-height,1,32,0,0,0,0,0,0)
            bitmap_header=C.create_string_buffer(bitmap_info)
            pixels=C.c_void_p()
            bitmap=gdi.CreateDIBSection(screen,bitmap_header,0,C.byref(pixels),None,0)
            assert bitmap
            previous=gdi.SelectObject(dc,bitmap)
            try:
                user.SendMessageW(child,0x318,dc,4)  # Populate editor hit regions.
                def click(x,y,message=0x201):
                    position=(y<<16)|x
                    user.SendMessageW(child,message,1,position)
                    user.SendMessageW(child,0x202,0,position)
                click(96,163)  # Half Speed, time bank slot 2.
                assert abs(host.param(5)-1/35)<1e-6
                assert any(index==5 for index,_ in host.automation)
                before=unpack_state(host.state())[1][1][1]
                click(720,330,0x203)  # Double-click adds an envelope point.
                assert len(unpack_state(host.state())[1][1][1])==len(before)+1
                click(1046,124)  # Undo envelope edit.
                assert len(unpack_state(host.state())[1][1][1])==len(before)
                host.param(5,11/35)  # Show original Reverse 4 pattern in delivered capture.
                user.SendMessageW(child,0x318,dc,4)  # WM_PRINTCLIENT / PRF_CLIENT.
                data=C.string_at(pixels,width*height*4)
                image=Image.frombytes("RGB",(width,height),data,"raw","BGRX")
                assert np.asarray(image).std()>10,"Editor capture is blank"
                image.save(ROOT/"dist"/"BeatWeaver.png")
            finally:
                gdi.SelectObject(dc,previous); gdi.DeleteObject(bitmap); gdi.DeleteDC(dc); user.ReleaseDC(None,screen)
            host.call(15)
            assert host.call(14,pointer=parent)==1  # Reopen after closing, same instance.
            host.call(15)
    finally:
        user.DestroyWindow(parent)


def render_demo():
    # A short original synthesized drum/bass loop, followed by processed examples.
    total=RATE*8
    t=np.arange(total)/RATE
    beat=t % .5
    kick=.45*np.sin(2*np.pi*(46*t+8*(1-np.exp(-beat*28))))*np.exp(-beat*18)
    rng=np.random.default_rng(7)
    hat=rng.standard_normal(total)*.06*np.exp(-(t%.125)*95)
    bass=.12*np.sin(2*np.pi*110*t)*(1-.5*np.exp(-beat*10))
    original=(kick+hat+bass).astype(np.float32)
    stereo=np.array([original,original*.95],dtype=np.float32)
    examples=[]
    for slot,volume in [(0,0),(1,0),(2,0),(7,3)]:
        with Host() as h:
            h.transparent(); h.param(5,slot/35); h.param(6,volume/35)
            output=[]
            for start in range(0,total,512): output.append(h.process(stereo[:,start:start+512]))
            examples.append(np.concatenate(output,axis=1))
    result=np.concatenate(examples,axis=1).T
    with wave.open(str(ROOT/"dist"/"BeatWeaver-demo.wav"),"wb") as f:
        f.setnchannels(2); f.setsampwidth(2); f.setframerate(RATE)
        f.writeframes((np.clip(result,-1,1)*32767).astype("<i2").tobytes())
    with Host() as h:
        (ROOT/"dist"/"BeatWeaver-Factory.bwv").write_bytes(h.state())


if __name__ == "__main__":
    started=time.perf_counter()
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(PluginTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    artifacts_ok=False
    artifact_error=None
    if result.wasSuccessful():
        try:
            capture_editor()
            render_demo()
            artifacts_ok=True
        except Exception as error:
            artifact_error=f"{type(error).__name__}: {error}"
            print(artifact_error,file=sys.stderr)
    report={"binary":str(DLL.relative_to(ROOT)),"sha256":hashlib.sha256(DLL.read_bytes()).hexdigest(),
            "tests_run":result.testsRun,"failures":len(result.failures),"errors":len(result.errors),
            "passed":result.wasSuccessful() and artifacts_ok,"seconds":round(time.perf_counter()-started,3),
            "gui_and_artifacts_passed":artifacts_ok,"artifact_error":artifact_error,
            "scope":"Own x64 host, signal measurements, MIDI, state, transport and native GUI. No FL Studio A/B test."}
    (ROOT/"dist"/"validation.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    sys.exit(0 if report["passed"] else 1)
