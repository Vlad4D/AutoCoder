# Bigtest level 3 -- flagship DX12 demo (~10-30 min, burns real tokens; run
# before releases, not on every change). Empty folder -> a self-verifying
# DirectX 12 triangle: WARP adapter (no GPU needed), renders 10 frames, saves
# the final backbuffer as out.bmp, exits 0. The verifier asserts the BMP
# exists and actually contains a rendered shape (>= 2 distinct colors).

. "$PSScriptRoot\common.ps1"
Test-ApiKey

$dirs = New-RunDirs 'dx12'

$prompt = @'
Build a minimal, self-verifying DirectX 12 demo in this empty folder: a
single colored triangle on a dark clear color, rendered offscreen with the
WARP software adapter and saved to out.bmp.

DELIVERABLE
- A CMakeLists.txt (Visual Studio 2022 generator, x64) and one C++ source
  file. Configure into the ./build directory; build the Debug config.
- The program: creates the D3D12 device on the WARP adapter
  (IDXGIFactory4::EnumWarpAdapter) so no GPU is needed; renders 10 frames of
  the triangle into a render-target texture (no window, no swap chain);
  copies the final frame to a readback buffer; writes it as out.bmp (24- or
  32-bit uncompressed BMP, at least 256x256) into the project root -- note
  it writes relative to its CWD, so run it FROM the project root; then
  scans the pixels it read back and prints exactly one line
  "PIXELS total=<n> non_clear=<m>" where non_clear counts pixels differing
  from the clear color, and exits 0 ONLY if non_clear > 0 (a uniform image
  is a failure: exit nonzero).
- Shaders: HLSL compiled at runtime with D3DCompile from embedded source
  strings (link d3dcompiler.lib). Dependencies: Windows SDK only (d3d12.lib,
  dxgi.lib, d3dcompiler.lib); no vcpkg, no external code.

RUNTIME BEHAVIOR (this run is unattended)
- Strictly console: no window, no MessageBox, no assert(), never wait for
  input.
- On any failure (failed HRESULT, D3DCompile error) print the message and
  HRESULT to stderr and exit nonzero immediately.
- Enable the D3D12 debug layer and, whenever an HRESULT fails, drain all
  pending ID3D12InfoQueue messages to stderr before exiting -- validation
  text otherwise goes to OutputDebugString and you will never see it in the
  console.

VERIFICATION (do all of this before declaring success)
1. Build, then run the demo from the project root.
2. Confirm it printed the PIXELS line with non_clear > 0 and exited 0.
3. Run the analyze_image tool on out.bmp and confirm: it parses, is at
   least 256x256, is NOT uniform, and the ASCII preview shows a triangle
   on a dark background.
If any step fails, fix the code and repeat the whole sequence.

KNOWN PITFALLS (avoid these up front)
- D3D12_INPUT_ELEMENT_DESC SemanticName must NOT include the index digit:
  use "POSITION" with SemanticIndex 0, never "POSITION0" -- the mismatch
  makes CreateGraphicsPipelineState fail with E_INVALIDARG.
- CD3DX12_* helpers live in d3dx12.h, which is NOT in the Windows SDK;
  zero-initialize and fill the D3D12 state structs manually.
- Cull mode: with default back-face culling, a wrong winding order renders
  nothing at all; use D3D12_CULL_MODE_NONE.
- Readback ordering: transition the render target
  RENDER_TARGET -> COPY_SOURCE before CopyTextureRegion (and back for the
  next frame), and wait for the GPU fence before mapping.
- Readback buffer row pitch must be 256-byte aligned
  (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); for a 256-wide RGBA8 target,
  256*4 = 1024 already is.
- Wrap BMP header structs in #pragma pack(push, 1) / #pragma pack(pop):
  the 14-byte BITMAPFILEHEADER otherwise gains 2 padding bytes after the
  "BM" magic and every image viewer rejects the file. The written file must
  start with bytes 42 4D, with pixel data exactly at the offset stored in
  the header.
'@

$exit = Invoke-AutoCoder -WorkDir $dirs.Work -Prompt $prompt -ResultsDir $dirs.Results `
    -TimeoutMinutes 40 -MaxIter 120 -CheckCommand 'cmake --build build'

$pass = $true
$notes = @()
if ($exit -ne 0) { $pass = $false; $notes += "CLI exited with code $exit (0 expected)" }

# The artifact is what matters: accept out.bmp anywhere in the sandbox (the
# demo's CWD when the agent runs it is easy to get wrong), but note when it
# is not at the requested project root.
$bmp = Join-Path $dirs.Work 'out.bmp'
if (-not (Test-Path $bmp)) {
    $found = Get-ChildItem -Path $dirs.Work -Recurse -Filter 'out.bmp' `
                -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) {
        $bmp = $found.FullName
        $notes += "out.bmp found at '$($found.FullName.Substring($dirs.Work.Length + 1))' instead of the project root (accepted)"
    }
}
if (-not (Test-Path $bmp)) {
    $pass = $false
    $notes += 'out.bmp was not produced anywhere in the sandbox'
} else {
    Copy-Item $bmp $dirs.Results -Force   # keep the evidence with the results
    if (Test-BmpHasContent -Path $bmp) {
        $notes += 'out.bmp contains a rendered shape (multiple distinct colors)'
    } else {
        $pass = $false
        $notes += 'out.bmp is unreadable or uniform -- corrupt file or nothing rendered'
    }
}

exit (Complete-Run -Name 'dx12' -Pass $pass -Notes $notes -Dirs $dirs)
