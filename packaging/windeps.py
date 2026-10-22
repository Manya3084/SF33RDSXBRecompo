#!/usr/bin/env python3
"""The DLLs a Windows exe needs, from a MinGW sysroot's bin, copied next to it
(the release zip):  windeps.py SYSROOT_BIN OUT_DIR EXE... (system DLLs left out)."""
import subprocess, sys, os, shutil
bin_dir = sys.argv[1]; out = sys.argv[2]; exes = sys.argv[3:]
system = {'kernel32.dll','user32.dll','gdi32.dll','msvcrt.dll','advapi32.dll','shell32.dll','ole32.dll','oleaut32.dll',
          'winmm.dll','imm32.dll','version.dll','setupapi.dll','opengl32.dll','ws2_32.dll','crypt32.dll','bcrypt.dll',
          'cfgmgr32.dll','hid.dll','dinput8.dll','dxgi.dll','d3d11.dll','d3d12.dll','dwmapi.dll','uxtheme.dll','comdlg32.dll',
          'shlwapi.dll','rpcrt4.dll','userenv.dll','secur32.dll','ntdll.dll','avrt.dll','sechost.dll','xinput1_4.dll',
          'msvcp_win.dll','api-ms-win-crt-runtime-l1-1-0.dll','powrprof.dll','iphlpapi.dll','dbghelp.dll','winhttp.dll',
          'mfplat.dll','mf.dll','mfreadwrite.dll','ksuser.dll','propsys.dll','gameinput.dll','combase.dll'}
need = []
def deps(f):
    txt = subprocess.run(['x86_64-w64-mingw32-objdump', '-p', f], capture_output=True, text=True).stdout
    return [l.split(':',1)[1].strip() for l in txt.splitlines() if 'DLL Name:' in l]
todo = list(exes)
while todo:
    f = todo.pop()
    for d in deps(f):
        if d.lower() in system or d.lower().startswith('api-ms-') or d in need:
            continue
        p = os.path.join(bin_dir, d)
        if not os.path.exists(p):
            print('missing', d, 'for', os.path.basename(f)); continue
        need.append(d); todo.append(p)
os.makedirs(out, exist_ok=True)
for d in need:
    shutil.copy2(os.path.join(bin_dir, d), out)
print(' '.join(sorted(need)))
