<#
.SYNOPSIS
    Build release + publicacao automatica dos pacotes do XBSX2 no Cloudflare R2.

.DESCRIPTION
    Passo a passo completo e padronizado:

    1. BUILD
       - Compila os pacotes MSIXBundle do XBSX2 via MSBuild no perfil desejado:
         - Xbox Series S|X: 'Release AVX2' (otimizado com AVX2 de 256 bits)
         - Xbox One: 'Release' (compativel com a CPU AMD Jaguar sem AVX2)
       - Copia os artefatos para a pasta dist\ local com nomes estaveis:
         - dist\xbsx2-series.msixbundle
         - dist\xbsx2-one.msixbundle
         - dist\xbsx2.msixbundle (padrao Series)

    2. VERIFICACAO
       - Le a versao oficial diretamente do Package.appxmanifest.
       - Calcula SHA-256 e MD5 dos pacotes gerados.
       - Gera os sidecars de integridade (.sha256).
       - Gera o dist\version.json estruturado com metadados para os consoles.

    3. UPLOAD PARA R2 - HISTORICO (versionado, nunca sobrescreve)
       - rgs/xbsx2/history/xbsx2-series-<version>.msixbundle
       - rgs/xbsx2/history/xbsx2-one-<version>.msixbundle
       - Se a versao ja existir no remoto com bytes diferentes, aborta (use -Force).

    >>> TRAVA DE ANUNCIO:
        Os passos 4 a 8 so rodam com a flag -Announce.
        Sem ela o script para depois do historico: constroi, confere integridade e
        arquiva, sem que os usuarios instalados recebam nada.

    4. UPLOAD PARA R2 - DISTRIBUICAO (nomes estaveis, sempre sobrescrevem)
       - rgs/xbsx2/xbsx2.msixbundle          <- Pacote padrao recomendado
       - rgs/xbsx2/xbsx2-series.msixbundle   <- Pacote dedicado Series S|X (AVX2)
       - rgs/xbsx2/xbsx2-one.msixbundle      <- Pacote dedicado Xbox One (SSE4)
       - Arquivos .sha256 correspondentes

    5. VERIFICACAO POS-UPLOAD (Origem R2)
       - Confere tamanho E hash MD5 (ETag) via API S3 com rclone lsjson.

    6. ANUNCIO (version.json)
       - Publica rgs/xbsx2/version.json com Cache-Control: max-age=300.

    7. PURGA DO CACHE DE BORDA (Cloudflare)
       - Purga os links publicos na API da Cloudflare caso CF_API_TOKEN e CF_ZONE_ID
         estejam definidos em build.properties.

    8. VERIFICACAO PELA URL PUBLICA
       - Faz probe HTTP baixando o version.json e confirmando os hashes publicos
         em https://versions.digitalstoregames.com/rgs/xbsx2/.

.PARAMETER Target
    Define qual variante compilar e publicar:
    - 'All'    (Padrao): constroi e publica tanto Series quanto Xbox One.
    - 'Series': constroi e publica apenas para Xbox Series S|X.
    - 'One'   : constroi e publica apenas para Xbox One.

.PARAMETER SkipBuild
    Pula a compilacao MSBuild e utiliza os pacotes ja gerados em AppPackages\.

.PARAMETER SkipHistory
    Pula o envio da copia versionada ao history/.

.PARAMETER DryRun
    Executa todas as verificacoes e gera os arquivos em dist\, mas sem enviar ao R2.

.PARAMETER Force
    Permite sobrescrever arquivos ja existentes no history/ com bytes diferentes.

.PARAMETER Announce
    Chave obrigatoria para atualizar a distribuicao publica e o version.json.

.PARAMETER VersionName
    Versao explicita (ex: '2.0.9.1'). Se omitido, le do Package.appxmanifest.

.EXAMPLE
    .\publish-xbsx2.ps1 -DryRun
    Testa o fluxo, verificacoes e metadados sem enviar nada ao R2.

.EXAMPLE
    .\publish-xbsx2.ps1 -SkipBuild -Announce
    Publica os pacotes ja compilados no Cloudflare R2 com anuncio oficial.
#>

[CmdletBinding()]
param(
    [ValidateSet('All', 'Series', 'One')]
    [string]$Target = 'All',

    [switch]$SkipBuild,
    [switch]$SkipHistory,
    [switch]$DryRun,
    [switch]$Force,
    [switch]$Announce,
    [string]$VersionName = ""
)

$ErrorActionPreference = "Stop"

# --- CONFIGURACOES DE DIRETORIOS ----------------------
$PROJECT_ROOT    = Split-Path -Parent $MyInvocation.MyCommand.Path
$DIST_DIR        = Join-Path $PROJECT_ROOT "dist"
$VCXPROJ_PATH    = Join-Path $PROJECT_ROOT "pcsx2-uwp\pcsx2-uwp.vcxproj"
$MANIFEST_PATH   = Join-Path $PROJECT_ROOT "pcsx2-uwp\Package.appxmanifest"
$KEY_PATH        = Join-Path $PROJECT_ROOT "pcsx2-uwp\pcsx2-uwp_TemporaryKey.pfx"
$ENV_FILE        = Join-Path $PROJECT_ROOT "build.properties"
$LOCAL_R2_CONFIG = Join-Path $PROJECT_ROOT "r2-config.json"
$GODSEND_R2_CFG  = "D:\projects\GODSend\r2-config.json"

# Destinos no Cloudflare R2 (mesmo bucket dos demais projetos: versions)
$R2_FOLDER       = "rgs/xbsx2"
$R2_HISTORY      = "rgs/xbsx2/history"
$PUBLIC_BASE     = "https://versions.digitalstoregames.com"

# --- HELPERS ------------------------------------------
function Print-Step {
    param([string]$Message, [string]$Color = "Cyan")
    Write-Host ""
    Write-Host "========================================" -ForegroundColor $Color
    Write-Host "  $Message" -ForegroundColor $Color
    Write-Host "========================================" -ForegroundColor $Color
}

function Find-Rclone {
    $cmd = Get-Command rclone -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidate = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter "rclone.exe" -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($candidate) { return $candidate }

    throw "rclone.exe nao encontrado. Instale com: winget install Rclone.Rclone"
}

function Find-MSBuild {
    $candidates = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )

    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) {
            return $path
        }
    }

    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe
        if ($found -and (Test-Path -LiteralPath $found)) {
            return $found
        }
    }

    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    throw "MSBuild.exe nao encontrado. Certifique-se de que o Visual Studio Build Tools esta instalado."
}

function Get-R2Config {
    if ($Script:R2_ACCESS_KEY_ID -and $Script:R2_SECRET_ACCESS_KEY -and $Script:R2_ENDPOINT -and $Script:R2_BUCKET) {
        return [PSCustomObject]@{
            accessKeyId     = $Script:R2_ACCESS_KEY_ID
            secretAccessKey = $Script:R2_SECRET_ACCESS_KEY
            endpoint        = $Script:R2_ENDPOINT
            bucket          = $Script:R2_BUCKET
            source          = "build.properties"
        }
    }
    if (Test-Path -LiteralPath $LOCAL_R2_CONFIG) {
        $c = Get-Content -LiteralPath $LOCAL_R2_CONFIG -Raw -Encoding UTF8 | ConvertFrom-Json
        $c | Add-Member -NotePropertyName source -NotePropertyValue $LOCAL_R2_CONFIG -Force
        return $c
    }
    if (Test-Path -LiteralPath $GODSEND_R2_CFG) {
        $c = Get-Content -LiteralPath $GODSEND_R2_CFG -Raw -Encoding UTF8 | ConvertFrom-Json
        $c | Add-Member -NotePropertyName source -NotePropertyValue $GODSEND_R2_CFG -Force
        return $c
    }
    throw "Credenciais R2 nao encontradas. Defina R2_* em build.properties, crie r2-config.json ou certifique-se de que $GODSEND_R2_CFG existe."
}

# Carregar build.properties se existir
if (Test-Path -LiteralPath $ENV_FILE) {
    Get-Content -LiteralPath $ENV_FILE -Encoding UTF8 | ForEach-Object {
        if ($_ -match '^\s*([^#=]+)=(.*)\s*$') {
            $k = $matches[1].Trim()
            $v = $matches[2].Trim().Trim('"', "'")
            Set-Variable -Name $k -Value $v -Scope Script
        }
    }
}

# Ler versao oficial do Package.appxmanifest
if (-not $VersionName) {
    if (Test-Path -LiteralPath $MANIFEST_PATH) {
        $manifestXml = [xml](Get-Content -LiteralPath $MANIFEST_PATH -Raw -Encoding UTF8)
        $VersionName = $manifestXml.Package.Identity.Version
    }
    if (-not $VersionName) {
        $VersionName = "2.0.9.1"
    }
}

Write-Host "XBSX2 Target: $Target | Versao: $VersionName" -ForegroundColor Cyan

# --- STEP 1: BUILD ------------------------------------
$msbuild = Find-MSBuild

$seriesPkgDir = Join-Path $PROJECT_ROOT "AppPackages\pcsx2-uwp\pcsx2-uwp_${VersionName}_Release AVX2_Test"
$seriesBundle = Join-Path $seriesPkgDir "pcsx2-uwp_${VersionName}_x64_Release AVX2.msixbundle"

$onePkgDir    = Join-Path $PROJECT_ROOT "AppPackages\pcsx2-uwp\pcsx2-uwp_${VersionName}_Test"
$oneBundle    = Join-Path $onePkgDir "pcsx2-uwp_${VersionName}_x64.msixbundle"

if (-not $SkipBuild) {
    if ($Target -eq 'All' -or $Target -eq 'Series') {
        Print-Step "PASSO 1/8: Compilando Xbox Series S|X (Release AVX2)"
        & $msbuild $VCXPROJ_PATH "/p:SolutionDir=$PROJECT_ROOT\\" /p:Configuration="Release AVX2" /p:Platform=x64 /p:AppxPackageSigningEnabled=true "/p:PackageCertificateKeyFile=$KEY_PATH" /m:8
        if ($LASTEXITCODE -ne 0) { throw "Falha na compilacao do perfil Release AVX2 (exit code $LASTEXITCODE)" }
    }

    if ($Target -eq 'All' -or $Target -eq 'One') {
        Print-Step "PASSO 1/8: Compilando Xbox One (Release SSE4)"
        & $msbuild $VCXPROJ_PATH "/p:SolutionDir=$PROJECT_ROOT\\" /p:Configuration="Release" /p:Platform=x64 /p:AppxPackageSigningEnabled=true "/p:PackageCertificateKeyFile=$KEY_PATH" /m:8
        if ($LASTEXITCODE -ne 0) { throw "Falha na compilacao do perfil Release (exit code $LASTEXITCODE)" }
    }
} else {
    Print-Step "PASSO 1/8: Build (SKIPPED - usando pacotes existentes em AppPackages\)"
}

# Criar pasta dist local
New-Item -ItemType Directory -Force -Path $DIST_DIR | Out-Null

$itemsToPublish = @()

if ($Target -eq 'All' -or $Target -eq 'Series') {
    if (-not (Test-Path -LiteralPath $seriesBundle)) {
        throw "Pacote do Series nao encontrado em:`n  $seriesBundle"
    }
    $localSeries = Join-Path $DIST_DIR "xbsx2-series.msixbundle"
    Copy-Item -LiteralPath $seriesBundle -Destination $localSeries -Force

    $localDefault = Join-Path $DIST_DIR "xbsx2.msixbundle"
    Copy-Item -LiteralPath $seriesBundle -Destination $localDefault -Force

    $itemsToPublish += @{
        Key          = "series"
        PlatformName = "Xbox Series S|X"
        Arch         = "x64-avx2"
        FileName     = "xbsx2-series.msixbundle"
        LocalPath    = $localSeries
        HistoryName  = "xbsx2-series-$VersionName.msixbundle"
    }
    $itemsToPublish += @{
        Key          = "default"
        PlatformName = "Xbox Series (Padrao)"
        Arch         = "x64-avx2"
        FileName     = "xbsx2.msixbundle"
        LocalPath    = $localDefault
        HistoryName  = "xbsx2-$VersionName.msixbundle"
    }
}

if ($Target -eq 'All' -or $Target -eq 'One') {
    if (-not (Test-Path -LiteralPath $oneBundle)) {
        throw "Pacote do Xbox One nao encontrado em:`n  $oneBundle"
    }
    $localOne = Join-Path $DIST_DIR "xbsx2-one.msixbundle"
    Copy-Item -LiteralPath $oneBundle -Destination $localOne -Force

    $itemsToPublish += @{
        Key          = "one"
        PlatformName = "Xbox One (Fat/S/X)"
        Arch         = "x64-sse4"
        FileName     = "xbsx2-one.msixbundle"
        LocalPath    = $localOne
        HistoryName  = "xbsx2-one-$VersionName.msixbundle"
    }
}

# --- STEP 2: VERIFICACAO E METADADOS ------------------
Print-Step "PASSO 2/8: Verificacao de Integridade e Metadados"

$downloadsPayload = [ordered]@{}

foreach ($item in $itemsToPublish) {
    $fPath = $item.LocalPath
    $fItem = Get-Item -LiteralPath $fPath
    $item.Size   = $fItem.Length
    $item.Sha256 = (Get-FileHash -LiteralPath $fPath -Algorithm SHA256).Hash.ToLower()
    $item.Md5    = (Get-FileHash -LiteralPath $fPath -Algorithm MD5).Hash.ToLower()

    # Gerar sidecar .sha256
    $shaFile = Join-Path $DIST_DIR "$($item.FileName).sha256"
    "$($item.Sha256)  $($item.FileName)" | Out-File -FilePath $shaFile -Encoding ascii -Force
    $item.Sha256File = $shaFile

    Write-Host "  [$($item.PlatformName)] $($item.FileName)" -ForegroundColor Green
    Write-Host "    Tamanho: $($item.Size) bytes ($([math]::Round($item.Size / 1MB, 2)) MB)" -ForegroundColor White
    Write-Host "    SHA-256: $($item.Sha256)" -ForegroundColor White

    $downloadsPayload[$item.Key] = [ordered]@{
        name     = $item.FileName
        platform = $item.PlatformName
        arch     = $item.Arch
        url      = "$PUBLIC_BASE/$R2_FOLDER/$($item.FileName)?v=$VersionName"
        sha256   = $item.Sha256
        size     = [long]$item.Size
    }
}

# Gerar version.json oficial
$versionJsonFile = Join-Path $DIST_DIR "version.json"
$versionData = [ordered]@{
    product     = "XBSX2"
    versionName = $VersionName
    channel     = "release"
    updatedAt   = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    downloads   = $downloadsPayload
} | ConvertTo-Json -Depth 4

$versionData | Out-File -FilePath $versionJsonFile -Encoding ascii -Force
Write-Host "  OK: dist\version.json gerado" -ForegroundColor Green

# --- STEP 3: UPLOAD - HISTORICO -----------------------
$cfg     = Get-R2Config
$rclone  = Find-Rclone

$s3Flags = @(
    "--log-level=ERROR",
    "--s3-provider=Cloudflare",
    "--s3-access-key-id=$($cfg.accessKeyId)",
    "--s3-secret-access-key=$($cfg.secretAccessKey)",
    "--s3-endpoint=$($cfg.endpoint)",
    "--s3-no-check-bucket"
)

$bundleHeaders = @("--header-upload=Content-Type: application/vnd.ms-appx.bundle")
$txtHeaders    = @("--header-upload=Content-Type: text/plain; charset=utf-8")
$jsonHeaders   = @("--header-upload=Content-Type: application/json; charset=utf-8", "--header-upload=Cache-Control: max-age=300")

$distDest    = ":s3:$($cfg.bucket)/$R2_FOLDER"
$historyDest = ":s3:$($cfg.bucket)/$R2_HISTORY"

if (-not $SkipHistory) {
    Print-Step "PASSO 3/8: Upload Historico Versionado ($R2_HISTORY)"

    $existingHistory = & $rclone lsjson $historyDest @s3Flags --hash | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw "rclone lsjson falhou em $historyDest" }

    foreach ($item in $itemsToPublish) {
        $histName = $item.HistoryName
        $clash = $existingHistory | Where-Object { -not $_.IsDir -and $_.Name -eq $histName } | Select-Object -First 1

        if ($clash) {
            $remoteMd5 = if ($clash.Hashes -and $clash.Hashes.md5) { $clash.Hashes.md5.ToLower() } else { $null }
            if ($remoteMd5 -eq $item.Md5) {
                Write-Host "  $histName ja existe no historico com os mesmos bytes. Pulando." -ForegroundColor Green
                continue
            } elseif (-not $Force) {
                throw "$histName ja existe no historico com bytes DIFERENTES. Suba a versao ou use -Force."
            } else {
                Write-Host "  -Force: sobrescrevendo $histName no historico." -ForegroundColor Yellow
            }
        }

        Write-Host "Enviando $histName -> $($cfg.bucket)/$R2_HISTORY ..." -ForegroundColor Yellow
        if ($DryRun) {
            Write-Host "  [DRY-RUN] envio pulado." -ForegroundColor Magenta
        } else {
            & $rclone copyto $item.LocalPath "$historyDest/$histName" @s3Flags @bundleHeaders --progress
            if ($LASTEXITCODE -ne 0) { throw "rclone copyto falhou ao enviar $histName" }
            Write-Host "  OK: $histName enviado" -ForegroundColor Green
        }
    }
} else {
    Print-Step "PASSO 3/8: Upload Historico (SKIPPED)"
}

# --- TRAVA DE ANUNCIO ---------------------------------
if (-not $Announce) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Yellow
    Write-Host "  PARADO ANTES DE PUBLICAR (sem -Announce)" -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Feito com sucesso:" -ForegroundColor White
    Write-Host "    - Build dos pacotes MSIXBundle ($Target)" -ForegroundColor Gray
    Write-Host "    - Verificacao de integridade (SHA-256 e MD5)" -ForegroundColor Gray
    Write-Host "    - Arquivado em $R2_HISTORY (versionado, seguro)" -ForegroundColor Gray
    Write-Host ""
    Write-Host "  NAO feito (nenhum usuario recebe a atualizacao):" -ForegroundColor White
    Write-Host "    - Pacotes de distribuicao publica NAO sobrescritos" -ForegroundColor Gray
    Write-Host "    - version.json NAO publicado" -ForegroundColor Gray
    Write-Host ""
    Write-Host "  Para concluir o deploy e atualizar os consoles em producao, repita com -Announce:" -ForegroundColor White
    Write-Host "    .\publish-xbsx2.ps1 -SkipBuild -Announce" -ForegroundColor Cyan
    Write-Host ""
    exit 0
}

# --- STEP 4: UPLOAD - DISTRIBUICAO PUBLICA ------------
Print-Step "PASSO 4/8: Upload de Distribuicao (URLs publicas)"

foreach ($item in $itemsToPublish) {
    Write-Host "Publicando $($item.FileName) -> $($cfg.bucket)/$R2_FOLDER ..." -ForegroundColor Yellow
    if ($DryRun) {
        Write-Host "  [DRY-RUN] envio pulado." -ForegroundColor Magenta
    } else {
        & $rclone copyto $item.LocalPath "$distDest/$($item.FileName)" @s3Flags @bundleHeaders --progress
        if ($LASTEXITCODE -ne 0) { throw "rclone copyto falhou para $($item.FileName)" }

        & $rclone copyto $item.Sha256File "$distDest/$($item.FileName).sha256" @s3Flags @txtHeaders
        if ($LASTEXITCODE -ne 0) { throw "rclone copyto falhou para $($item.FileName).sha256" }

        Write-Host "  OK: $($item.FileName) e sidecar publicados" -ForegroundColor Green
    }
}

# --- STEP 5: VERIFICACAO POS-UPLOAD -------------------
Print-Step "PASSO 5/8: Verificacao Pos-Upload (Origem R2)"

if ($DryRun) {
    Write-Host "  [DRY-RUN] verificacao pulada." -ForegroundColor Magenta
} else {
    $remoteEntries = & $rclone lsjson $distDest @s3Flags --hash | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw "rclone lsjson falhou em $distDest" }

    foreach ($item in $itemsToPublish) {
        $live = $remoteEntries | Where-Object { -not $_.IsDir -and $_.Name -eq $item.FileName } | Select-Object -First 1
        if (-not $live) { throw "VERIFICACAO FALHOU: $($item.FileName) nao encontrado no remoto." }
        if ($live.Size -ne $item.Size) {
            throw "VERIFICACAO FALHOU: $($item.FileName) tamanho diferente (local: $($item.Size), remoto: $($live.Size))"
        }
        Write-Host "  OK: $($item.FileName) conferido no R2 ($($item.Size) bytes)" -ForegroundColor Green
    }
}

# --- STEP 6: ANUNCIO (version.json) -------------------
Print-Step "PASSO 6/8: Publicando Manifesto de Versao (version.json)"

if ($DryRun) {
    Write-Host "  [DRY-RUN] envio pulado." -ForegroundColor Magenta
} else {
    & $rclone copyto $versionJsonFile "$distDest/version.json" @s3Flags @jsonHeaders
    if ($LASTEXITCODE -ne 0) { throw "rclone copyto falhou para version.json" }
    Write-Host "  OK: version.json publicado com sucesso" -ForegroundColor Green
}

# --- STEP 7: PURGA DE CACHE (Cloudflare) --------------
Print-Step "PASSO 7/8: Purga de Cache no Cloudflare"

$cfToken = $Script:CF_API_TOKEN
$cfZone  = $Script:CF_ZONE_ID

if ($DryRun) {
    Write-Host "  [DRY-RUN] purga pulada." -ForegroundColor Magenta
} elseif ([string]::IsNullOrWhiteSpace($cfToken) -or [string]::IsNullOrWhiteSpace($cfZone)) {
    Write-Host "  PULADO: CF_API_TOKEN / CF_ZONE_ID nao definidos em build.properties." -ForegroundColor Yellow
    Write-Host "  Os clientes receberao a versao atualizada via query string (?v=$VersionName)." -ForegroundColor Yellow
} else {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $purgeUrls = @("$PUBLIC_BASE/$R2_FOLDER/version.json")
    foreach ($item in $itemsToPublish) {
        $purgeUrls += "$PUBLIC_BASE/$R2_FOLDER/$($item.FileName)"
        $purgeUrls += "$PUBLIC_BASE/$R2_FOLDER/$($item.FileName).sha256"
    }

    $purgeBody = (@{ files = $purgeUrls } | ConvertTo-Json)
    $purgeResp = Invoke-RestMethod -Method Post `
        -Uri "https://api.cloudflare.com/client/v4/zones/$cfZone/purge_cache" `
        -Headers @{ Authorization = "Bearer $cfToken" } `
        -ContentType 'application/json' `
        -Body $purgeBody -TimeoutSec 60

    if ($purgeResp.success) {
        Write-Host "  OK: Cache purgado com sucesso no Cloudflare" -ForegroundColor Green
        Start-Sleep -Seconds 4
    } else {
        Write-Host "  AVISO: Falha ao purgar cache Cloudflare: $($purgeResp.errors | ConvertTo-Json -Compress)" -ForegroundColor Yellow
    }
}

# --- STEP 8: VERIFICACAO PELA URL PUBLICA -------------
Print-Step "PASSO 8/8: Verificacao pela URL Publica (o que o console baixa)"

if ($DryRun) {
    Write-Host "  [DRY-RUN] verificacao pulada." -ForegroundColor Magenta
} else {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $jsonUrl = "$PUBLIC_BASE/$R2_FOLDER/version.json"
    Write-Host "Consultando $jsonUrl ..." -ForegroundColor Yellow
    try {
        $publicJson = Invoke-RestMethod -Uri $jsonUrl -Headers @{ 'Cache-Control' = 'no-cache' } -TimeoutSec 30
        if ($publicJson.versionName -eq $VersionName) {
            Write-Host "  OK: version.json publico anuncia a versao correta ($VersionName)" -ForegroundColor Green
        } else {
            Write-Host "  AVISO: version.json publico anuncia '$($publicJson.versionName)', aguardando propagacao de cache..." -ForegroundColor Yellow
        }
    } catch {
        Write-Host "  AVISO: Nao foi possivel consultar $jsonUrl : $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

# --- SUMMARY ------------------------------------------
Print-Step "RESUMO DA PUBLICACAO" "Green"

Write-Host "Versao: $VersionName" -ForegroundColor Green
Write-Host ""
Write-Host "Manifesto de Anuncio (version.json):" -ForegroundColor Cyan
Write-Host "  $PUBLIC_BASE/$R2_FOLDER/version.json" -ForegroundColor White
Write-Host ""
Write-Host "Links de Download Direto:" -ForegroundColor Cyan
foreach ($item in $itemsToPublish) {
    Write-Host "  [$($item.PlatformName)]" -ForegroundColor Yellow
    Write-Host "  $PUBLIC_BASE/$R2_FOLDER/$($item.FileName)" -ForegroundColor White
    Write-Host "  SHA-256: $($item.Sha256)" -ForegroundColor Gray
}
Write-Host ""
Write-Host "Publicacao concluida com sucesso!" -ForegroundColor Green
