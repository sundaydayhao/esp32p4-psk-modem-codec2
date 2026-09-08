"""Regenerate vendored quantisers using upstream's native code generator."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
C2 = ROOT / 'components/codec2'
SRC = C2 / 'vendor/src'
OUT = C2 / 'generated'
OUT.mkdir(exist_ok=True)
os.environ['PATH'] = 'C:/cygwin64/bin;' + os.environ['PATH']
exe = ROOT / 'build/host-tools/generate_codebook.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['C:/cygwin64/bin/gcc.exe', str(SRC/'generate_codebook.c'), '-lm', '-o', str(exe)], check=True)
books = {
    'codebook': ('lsp_cb', [f'lsp{i}.txt' for i in range(1, 11)]),
    'codebookd': ('lsp_cbd', [f'dlsp{i}.txt' for i in range(1, 11)]),
    'codebookjmv': ('lsp_cbjmv', [f'lspjmv{i}.txt' for i in range(1, 4)]),
    'codebookge': ('ge_cb', ['gecb.txt']),
    'codebooknewamp1': ('newamp1vq_cb', ['train_120_1.txt', 'train_120_2.txt']),
    'codebooknewamp1_energy': ('newamp1_energy_cb', ['newamp1_energy_q.txt']),
}
for name, (symbol, files) in books.items():
    result = subprocess.run([str(exe), symbol, *[str(SRC/'codebook'/f) for f in files]], check=True, capture_output=True)
    (OUT / (name+'.c')).write_bytes(result.stdout)
print('Generated', len(books), 'Codec2 quantisers')
