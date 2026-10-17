# Builds WeaponFX.dll on every push to main and every pull request.
# Run it by hand (Actions -> Build -> Run workflow) with a version such as
# v1.0.0 to also publish a release with the packaged DLL.
name: Build

on:
  push:
    branches: [main]
  pull_request:
  workflow_dispatch:
    inputs:
      version:
        description: Release tag, e.g. v1.0.0 (empty = build only)
        required: false
        default: ""

permissions:
  contents: write

jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4

      - name: Build
        shell: cmd
        run: call build.bat

      # The layout GUIDE.md's quick start expects: the DLL to load, and the
      # data folder to copy to %LOCALAPPDATA%\WeaponFX.
      - name: Package
        shell: pwsh
        run: |
          $pkg = "package"
          New-Item -ItemType Directory "$pkg/WeaponFX" | Out-Null
          Copy-Item build/WeaponFX.dll, build/WeaponFX.pdb, LICENSE, GUIDE.md $pkg
          Copy-Item -Recurse shaders, masks "$pkg/WeaponFX"
          Compress-Archive -Path "$pkg/*" -DestinationPath WeaponFX.zip

      - uses: actions/upload-artifact@v4
        with:
          name: WeaponFX
          path: package/

      - name: Release
        if: github.event_name == 'workflow_dispatch' && inputs.version != ''
        shell: pwsh
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          gh release create "${{ inputs.version }}" WeaponFX.zip `
            --target "${{ github.sha }}" `
            --title "WeaponFX ${{ inputs.version }}" `
            --generate-notes
