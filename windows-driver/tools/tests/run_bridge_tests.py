"""Offline-only bridge builds and tests. Never accesses an adapter or NNPI device."""
from pathlib import Path
import subprocess
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
OUT = HERE / 'build'
OUT.mkdir(exist_ok=True)
VCVARS = Path('C:/BuildTools2022/VC/Auxiliary/Build/vcvars64.bat')
MSBUILD = Path('C:/BuildTools2022/MSBuild/Current/Bin/MSBuild.exe')

def run(name, command, expected=0, marker=None):
    p = subprocess.run('cmd.exe /d /s /c "' + command + '"', cwd=OUT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / (name + '.log')).write_bytes(p.stdout)
    text = p.stdout.decode('utf-8', errors='replace')
    print(text)
    if p.returncode != expected or (marker and marker not in text):
        raise RuntimeError(f'{name}: exit={p.returncode}, expected={expected}; marker={marker!r}')

for configuration, flags in [('Debug', '/Od /Zi'), ('Release', '/O2 /DNDEBUG')]:
    exe = OUT / f'bridge-mock-{configuration}.exe'
    run(f'mock-build-{configuration}', f'call "{VCVARS}" && cl.exe /nologo /std:c++17 /EHsc /W4 /WX {flags} "{HERE / "test_bridge.cpp"}" /Fe:"{exe}" /Fo:"{OUT / (configuration + ".obj")}"')
    run(f'mock-run-{configuration}', f'"{exe}"', marker='BRIDGE_MOCK_TESTS_PASS cases=16')
    run(f'dependency-build-{configuration}', f'"{MSBUILD}" "{TOOLS / "nnpi-net-bridge.vcxproj"}" /t:Rebuild /p:Configuration={configuration} /p:Platform=x64 /p:NnpiWdkVersion=10.0.26100.0 /p:DependencyCheckOnly=true /nologo /v:minimal')
# Deliberately supply a nonexistent include directory: normal builds MUST refuse
# to substitute guessed TAP IOCTLs. This is an expected failure, not a pass build.
run('production-missing-header-expected-failure', f'"{MSBUILD}" "{TOOLS / "nnpi-net-bridge.vcxproj"}" /t:Build /p:Configuration=Release /p:Platform=x64 /p:NnpiWdkVersion=10.0.26100.0 /p:DependencyCheckOnly=false /p:NnpiTapIncludeDir="{OUT / "missing-official-tap-header"}" /nologo /v:minimal', expected=1, marker='Official OpenVPN tap-windows6 tap-windows.h is required')
print('BRIDGE_OFFLINE_SUITE_PASS configurations=2 mock_cases_per_configuration=16; production TAP header verification remains REQUIRED')
