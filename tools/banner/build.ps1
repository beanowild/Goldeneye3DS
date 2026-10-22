# build.ps1 - builds the 3D HOME Menu banner from the game's own files.
#   powershell -File tools/banner/build.ps1 <out dir> [preview views: game front three side back sheet]
#
# <out>/bond_banner.blend   rigged, animated Bond (the pose is pose.py's frame)
# <out>/banner.glb          the banner layout (Bond + logo) for pycgfx
# <out>/banner.cgfx         the 3DS banner model
# <out>/banner.bin          CGFX + sound, the ExeFS:/banner of a CIA (makerom -banner)
# <out>/home_preview.png    the banner through the HOME Menu camera, spinning
param([string]$Out)
$Views = $args
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$tools = Join-Path $here "..\..\.tools"
$blender = Join-Path $tools "blender\blender-4.2.3-windows-x64\blender.exe"
$venv = Join-Path $tools "venv\Scripts\python.exe"
$bannertool = Join-Path $tools "bannertool\windows-x86_64\bannertool.exe"
$data = Join-Path $Out "export"

py -3 (Join-Path $here "export.py") $data
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$shots = @($Views | ForEach-Object { Join-Path $Out "preview_$_.png" })
& $blender -b --factory-startup -P (Join-Path $here "blender_build.py") -- $data (Join-Path $Out "bond_banner.blend") @shots 2>&1 |
    ForEach-Object { "$_" } | Where-Object { $_ -match "^saved|Error|Traceback|line \d+" }
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$glb = Join-Path $Out "banner.glb"
$cgfx = Join-Path $Out "banner.cgfx"
& $venv (Join-Path $here "banner_glb.py") $data $glb
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $venv (Join-Path $tools "pycgfx\main.py") $glb $cgfx
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# the HOME Menu plays this when the banner is selected (stereo, under 3 s);
# tools/banner/banner_sound.wav if there is one, else a second of silence
$sound = Join-Path $here "banner_sound.wav"
if (-not (Test-Path $sound)) {
    $sound = Join-Path $data "silence.wav"
    & $venv -c "import wave; w = wave.open(r'$sound', 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(32000); w.writeframes(bytes(4 * 32000)); w.close()"
}
& $bannertool makebanner -ci $cgfx -a $sound -o (Join-Path $Out "banner.bin")
if ($LASTEXITCODE) { exit $LASTEXITCODE }

& $venv (Join-Path $here "home_preview.py") $glb (Join-Path $Out "home_preview.png")
exit $LASTEXITCODE
