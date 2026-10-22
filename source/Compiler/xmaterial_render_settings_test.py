# Standalone check of the material render settings (documentation/render_settings_plan.md; not in CMake): compiles a copy of SliceMat
# (Output: PBR) as it is (a v3 file: the defaults, as before) and as a v4 file that is Masked and double sided: the compile options, the
# cut, the declared "Alpha Cutoff" parameter with the file's default. The copy's descriptor, log and resource are temporary and removed.
#   python xmaterial_render_settings_test.py [path to xmaterial_compiler.exe]     (default: the Debug build)
import os, re, shutil, subprocess, sys

HERE    = os.path.dirname(os.path.realpath(__file__))                                    # the plugin folder is a link into the project's Cache/Plugins
PROJECT = os.path.normpath(os.path.join(HERE, '../../../../..'))                     # example.lionprj
EXE     = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '../../build/xmaterial_compiler.vs2022/Debug/xmaterial_compiler.exe')
SLICE   = os.path.join(PROJECT, 'Descriptors/Material/01/00/5E1CE0A110000001.desc/Descriptor.txt')
REL     = r'Cache\Descriptors\Material\01\BD\7E570BD0000003.desc'                    # a guid no asset has
LOG     = os.path.join(PROJECT, r'Cache\Resources\Logs\Material\01\BD\7E570BD0000003.log')
TEMP    = [os.path.join(PROJECT, REL), LOG, os.path.join(PROJECT, r'Cache\Resources\Platforms\WINDOWS\Material\01\BD\7E570BD0000003')]

V4 = '''
[ xProperties : 3 ]
{ Name:s                          Value:?           }
//------------------------------  ----------------
  "render_settings/Blend Mode"    ;enum   "Masked"
  "render_settings/Alpha Cutoff"  ;f32    0.25
  "render_settings/Double Sided"  ;bool   1
'''

def Compile(Text):
    with open(os.path.join(TEMP[0], 'Descriptor.txt'), 'w', newline='\r\n') as f: f.write(Text)
    R = subprocess.run([EXE, '-PROJECT', PROJECT, '-OPTIMIZATION', 'O1', '-DEBUG', 'D0', '-DESCRIPTOR', REL,
                        '-OUTPUT', os.path.join(PROJECT, r'Cache\Resources\Platforms\WINDOWS')], capture_output=True, text=True, errors='replace', timeout=120)
    Log = R.stdout + R.stderr
    assert R.returncode == 0 and '[COMPILATION_SUCCESS]' in Log, Log[-3000:]
    return Log

def main():
    os.makedirs(TEMP[0], exist_ok=True)
    try:
        Text = open(SLICE).read()
        Log  = Compile(Text)
        assert 'Render settings: blend 0, double sided false, depth write true, cast shadows true' in Log, 'a v3 file is not opaque and single sided'
        assert 'XMAT_ALPHA_CUTOFF (' not in Log

        # v4: the Info row's Version column (the third) and the settings record after the parameters
        Info = re.search(r'\[ Info \][^\[]*?\n(\s+\d+\s+\d+\s+)3(\s+\d+)', Text)
        assert Info, 'SliceMat is no longer a v3 file'
        Text = Text[:Info.start(2) - 1] + '4' + Text[Info.start(2):] + V4
        Log  = Compile(Text)
        assert 'Render settings: blend 1, double sided true, depth write true, cast shadows true' in Log, 'the v4 settings did not load'
        assert re.search(r'#define XMAT_ALPHA_CUTOFF \(g_Mat\.P\[XMAT_BASE \+ \d+u\]\.[xyzw]\)', Log), 'the cutoff is not a parameter read'
        assert 'if (a < XMAT_ALPHA_CUTOFF) discard;' in Log or re.search(r'if \(.+ < XMAT_ALPHA_CUTOFF\) discard;', Log), 'no cut'
        Layout = open(os.path.join(LOG, 'MaterialLayout.txt')).read()
        assert re.search(r'"Alpha Cutoff"(?:(?!/Name")[\s\S])*?Default/X"\s+;f32\s+#3E800000', Layout), 'the Alpha Cutoff parameter is not declared with the file\'s default'
    finally:
        for p in TEMP: shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else (os.path.exists(p) and os.remove(p))
    print('xmaterial_render_settings_test: ok')

if __name__ == '__main__': main()
