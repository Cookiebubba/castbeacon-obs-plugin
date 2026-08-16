[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*.exe"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    Log-Group "Building installer for ${ProductName}..."

    # Inno Setup ships with the windows-* runner images; fall back to the documented
    # install locations, and only then give up.
    $IsccCandidates = @(
        'ISCC.exe'
        "${Env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
        "${Env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )

    $Iscc = $null
    foreach ( $Candidate in $IsccCandidates ) {
        $Found = Get-Command -Name $Candidate -CommandType Application -ErrorAction SilentlyContinue
        if ( $Found ) {
            $Iscc = $Found.Source
            break
        }
    }

    if ( $Iscc -eq $null ) {
        throw 'Inno Setup (ISCC.exe) was not found - cannot build the Windows installer.'
    }

    $PayloadDir = "${ProjectRoot}/release/${Configuration}/${ProductName}"

    if ( ! ( Test-Path -Path "${PayloadDir}/bin/64bit/${ProductName}.dll" ) ) {
        throw "Built plugin not found at ${PayloadDir}/bin/64bit/${ProductName}.dll - nothing to package."
    }

    $ProductDisplayName = if ( $BuildSpec.displayName ) { $BuildSpec.displayName } else { $ProductName }
    $ProductAuthor = if ( $BuildSpec.author ) { $BuildSpec.author } else { $ProductName }

    $IssTemplate = Get-Content -Path "${ProjectRoot}/build-aux/installer-Windows.iss.in" -Raw
    $IssPath = "${ProjectRoot}/build_${Target}/installer-Windows.generated.iss"

    $Replacements = @{
        '@PLUGIN_NAME@' = $ProductName
        '@PLUGIN_DISPLAY_NAME@' = $ProductDisplayName
        '@PLUGIN_VERSION@' = $ProductVersion
        '@PLUGIN_AUTHOR@' = $ProductAuthor
        '@PAYLOAD_DIR@' = (Resolve-Path -Path $PayloadDir).Path
        '@OUTPUT_DIR@' = (Resolve-Path -Path "${ProjectRoot}/release").Path
        '@OUTPUT_BASENAME@' = "${OutputName}-Installer"
    }

    foreach ( $Token in $Replacements.Keys ) {
        $IssTemplate = $IssTemplate.Replace($Token, $Replacements[$Token])
    }

    Set-Content -Path $IssPath -Value $IssTemplate -Encoding utf8

    Invoke-External $Iscc '/Qp' $IssPath

    Log-Group
}

Package
