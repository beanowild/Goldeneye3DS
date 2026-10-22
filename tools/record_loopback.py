"""Record what Windows plays (WASAPI loopback of the default output) to a WAV.

    py -3 tools/record_loopback.py OUT.wav SECONDS

For checking the game's sound in Azahar from scripted runs. Needs
PyAudioWPatch (pip install --target .tools/pylib PyAudioWPatch).
"""
import os, sys, wave
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '.tools', 'pylib'))
import pyaudiowpatch as pyaudio

out, secs = sys.argv[1], float(sys.argv[2])
p = pyaudio.PyAudio()
wasapi = p.get_host_api_info_by_type(pyaudio.paWASAPI)
spk = p.get_device_info_by_index(wasapi['defaultOutputDevice'])
if not spk.get('isLoopbackDevice'):
    for lb in p.get_loopback_device_info_generator():
        if spk['name'] in lb['name']:
            spk = lb
            break
rate, ch = int(spk['defaultSampleRate']), spk['maxInputChannels']
st = p.open(format=pyaudio.paInt16, channels=ch, rate=rate, input=True,
            input_device_index=spk['index'], frames_per_buffer=1024)
frames = []
for _ in range(int(rate * secs / 1024)):
    frames.append(st.read(1024, exception_on_overflow=False))
st.close()
w = wave.open(out, 'wb')
w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate)
w.writeframes(b''.join(frames)); w.close()
print(out, rate, ch, spk['name'])
