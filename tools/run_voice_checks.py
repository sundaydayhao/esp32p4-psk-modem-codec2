"""Build and test the same portable Codec2, packet, playout and modem code as P4."""
from pathlib import Path
import os
import re
import subprocess

root = Path(__file__).resolve().parents[1]
os.chdir(root)
os.environ['PATH'] = 'C:/cygwin64/bin;' + os.environ['PATH']
c2 = root / 'components/codec2'
names = re.search(r'set\(C2_SOURCE_NAMES\s+(.*?)\)', (c2/'sources.cmake').read_text(), re.S).group(1).split()
sources = [str(c2/'vendor/src'/n) for n in names] + [str(p) for p in (c2/'generated').glob('*.c')]
sources += ['tools/host_voice_check.c','main/voice_protocol.c','main/voice_playout.c','main/voice_resample.c',
            'main/audio_decim.c','main/psk_tx.c','main/psk_rx.c','main/psk_codec.c','main/psk_modes.c',
            'components/psk_rtty/psk_rtty.c']
exe = root/'build/host-tools/host_voice_check.exe'
cmd = ['C:/cygwin64/bin/gcc.exe','-std=gnu11','-O2','-g','-ffunction-sections','-fdata-sections',
       '-DCODEC2_MODE_EN_DEFAULT=0','-DCODEC2_MODE_700C_EN=1',
       '-I'+str(c2/'vendor/src'),'-I'+str(c2/'include'),'-Imain','-Icomponents/psk_rtty/include',
       *sources,'-Wl,--gc-sections','-lm','-o',str(exe)]
subprocess.run(cmd,check=True)
subprocess.run([str(exe)],check=True)
