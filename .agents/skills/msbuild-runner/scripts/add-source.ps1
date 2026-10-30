<#
.SYNOPSIS
    Automated source/header file registrar for DOSBox-X Visual Studio project files.
.DESCRIPTION
    Safely injects <ClCompile> or <ClInclude> elements into vs/dosbox-x.vcxproj and vs/dosbox-x.vcxproj.filters.
    Guarantees:
    1. Proper XML ItemGroup nesting without malforming project files.
    2. Automatic non-SDL2 exclusion guard (<ExcludedFromBuild Condition="!$(Configuration.Contains('SDL2'))">true</ExcludedFromBuild>) for kiosk/imgui units.
    3. Proper filter declaration and categorization in vs/dosbox-x.vcxproj.filters.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,

    [string]$Filter = "",
    [switch]$KioskOnly = $true,
    [string]$VcxprojPath = "vs/dosbox-x.vcxproj",
    [string]$FiltersPath = "vs/dosbox-x.vcxproj.filters"
)

$ErrorActionPreference = "Stop"

# 1. Validate and resolve target project files
if (-not (Test-Path $VcxprojPath)) {
    Write-Error "[add-source] Project file not found: $VcxprojPath"
    exit 1
}

$projFullPath = (Resolve-Path $VcxprojPath).Path
$projDir = Split-Path -Parent $projFullPath

# 2. Validate and resolve source file
$resolvedSrcFile = ""
if (Test-Path $FilePath) {
    $resolvedSrcFile = (Resolve-Path $FilePath).Path
} else {
    $candInProj = Join-Path $projDir $FilePath
    if (Test-Path $candInProj) {
        $resolvedSrcFile = (Resolve-Path $candInProj).Path
    } else {
        Write-Error "[add-source] Source file not found: '$FilePath'. Please provide a valid file path."
        exit 1
    }
}

if (-not (Test-Path $resolvedSrcFile -PathType Leaf)) {
    Write-Error "[add-source] Target path is not a file: '$resolvedSrcFile'"
    exit 1
}

# Compute relative path from vs/ directory to the source file
$uriProj = [System.Uri]($projDir.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar)
$uriFile = [System.Uri]$resolvedSrcFile
$relUri = $uriProj.MakeRelativeUri($uriFile)
$normalizedPath = [System.Uri]::UnescapeDataString($relUri.ToString()).Replace('/', '\')

$ext = [System.IO.Path]::GetExtension($resolvedSrcFile).ToLower()
$isSource = $ext -in @(".cpp", ".c")
$isHeader = $ext -in @(".h", ".hpp")

if (-not $isSource -and -not $isHeader) {
    Write-Error "[add-source] Unsupported file extension '$ext'. Expected .cpp, .c, .h, or .hpp."
    exit 1
}

# Infer filter path if not provided
if (-not $Filter) {
    if ($normalizedPath -match '(?i)src\\kiosk\\ui') {
        $Filter = "Sources\kiosk\ui"
    } elseif ($normalizedPath -match '(?i)src\\kiosk\\machine') {
        $Filter = "Sources\kiosk\machine"
    } elseif ($normalizedPath -match '(?i)src\\kiosk') {
        $Filter = "Sources\kiosk"
    } elseif ($normalizedPath -match '(?i)src\\libs\\imgui') {
        $Filter = "Sources\libs\imgui"
    } elseif ($normalizedPath -match '(?i)src\\libs\\sqlite3') {
        $Filter = "Sources\libs\sqlite3"
    } else {
        $Filter = "Sources"
    }
}

# 3. Update vs/dosbox-x.vcxproj
$xmlDoc = [xml](Get-Content $projFullPath -Raw)
$nsMgr = [System.Xml.XmlNamespaceManager]::new($xmlDoc.NameTable)
$nsMgr.AddNamespace("ms", $xmlDoc.DocumentElement.NamespaceURI)

# Avoid XPath interpolation for Include values
$searchTag = if ($isSource) { "ms:ClCompile" } else { "ms:ClInclude" }
$candidateNodes = $xmlDoc.SelectNodes("//$searchTag", $nsMgr)
$existingNode = $null
foreach ($node in $candidateNodes) {
    if ($node.GetAttribute("Include") -eq $normalizedPath) {
        $existingNode = $node
        break
    }
}

if ($existingNode) {
    Write-Host "[add-source] Already present in ${VcxprojPath}: $normalizedPath" -ForegroundColor Yellow
} else {
    # Find an appropriate ItemGroup
    $targetItemGroup = $null
    if ($isSource) {
        $itemGroups = $xmlDoc.SelectNodes("//ms:ItemGroup[ms:ClCompile]", $nsMgr)
        if ($itemGroups.Count -gt 0) {
            $targetItemGroup = $itemGroups[$itemGroups.Count - 1]
        }
    } else {
        $itemGroups = $xmlDoc.SelectNodes("//ms:ItemGroup[ms:ClInclude]", $nsMgr)
        if ($itemGroups.Count -gt 0) {
            $targetItemGroup = $itemGroups[$itemGroups.Count - 1]
        }
    }

    if (-not $targetItemGroup) {
        Write-Error "[add-source] Could not find suitable ItemGroup in $VcxprojPath"
        exit 1
    }

    $elemName = if ($isSource) { "ClCompile" } else { "ClInclude" }
    $newElem = $xmlDoc.CreateElement($elemName, $xmlDoc.DocumentElement.NamespaceURI)
    $newElem.SetAttribute("Include", $normalizedPath)

    # Classify source path: automatically exclude kiosk, imgui, and sqlite3 from non-SDL2
    $isKioskOrImgui = ($normalizedPath -match '(?i)[\\/]kiosk([\\/]|$)' -or $normalizedPath -match '(?i)[\\/]imgui([\\/]|$)' -or $normalizedPath -match '(?i)[\\/]sqlite3([\\/]|$)')
    $shouldApplyExclusion = $isSource -and ($isKioskOrImgui -or ($PSBoundParameters.ContainsKey('KioskOnly') -and $KioskOnly))

    if ($shouldApplyExclusion) {
        $exclElem = $xmlDoc.CreateElement("ExcludedFromBuild", $xmlDoc.DocumentElement.NamespaceURI)
        $exclElem.SetAttribute("Condition", "!`$(Configuration.Contains('SDL2'))")
        $exclElem.InnerText = "true"
        [void]$newElem.AppendChild($exclElem)
    }

    [void]$targetItemGroup.AppendChild($newElem)
    $xmlDoc.Save($projFullPath)
    Write-Host "[add-source] Successfully added '$normalizedPath' to $VcxprojPath" -ForegroundColor Green
}

# 4. Update vs/dosbox-x.vcxproj.filters
if (Test-Path $FiltersPath) {
    $filtersFullPath = (Resolve-Path $FiltersPath).Path
    $filtersDoc = [xml](Get-Content $filtersFullPath -Raw)
    $nsFilters = [System.Xml.XmlNamespaceManager]::new($filtersDoc.NameTable)
    $nsFilters.AddNamespace("ms", $filtersDoc.DocumentElement.NamespaceURI)

    # Declare custom filter and missing parent filters in the filters file if needed
    $filterDeclGroup = $filtersDoc.SelectSingleNode("//ms:ItemGroup[ms:Filter]", $nsFilters)
    if ($filterDeclGroup -and $Filter) {
        $filterParts = $Filter.Split('\')
        $currentAcc = ""
        for ($i = 0; $i -lt $filterParts.Length; $i++) {
            if ($currentAcc) { $currentAcc += "\" + $filterParts[$i] } else { $currentAcc = $filterParts[$i] }
            $declNodes = $filterDeclGroup.SelectNodes("ms:Filter", $nsFilters)
            $foundDecl = $false
            foreach ($d in $declNodes) {
                if ($d.GetAttribute("Include") -eq $currentAcc) {
                    $foundDecl = $true
                    break
                }
            }
            if (-not $foundDecl) {
                $fElem = $filtersDoc.CreateElement("Filter", $filtersDoc.DocumentElement.NamespaceURI)
                $fElem.SetAttribute("Include", $currentAcc)
                $uidElem = $filtersDoc.CreateElement("UniqueIdentifier", $filtersDoc.DocumentElement.NamespaceURI)
                $uidElem.InnerText = "{" + [System.Guid]::NewGuid().ToString() + "}"
                [void]$fElem.AppendChild($uidElem)
                [void]$filterDeclGroup.AppendChild($fElem)
                Write-Host "[add-source] Declared missing filter: '$currentAcc'" -ForegroundColor Cyan
            }
        }
    }

    # Avoid XPath interpolation for Include values in filters
    $searchFilterTag = if ($isSource) { "ms:ClCompile" } else { "ms:ClInclude" }
    $filterCandidateNodes = $filtersDoc.SelectNodes("//$searchFilterTag", $nsFilters)
    $existingFilterNode = $null
    foreach ($fNode in $filterCandidateNodes) {
        if ($fNode.GetAttribute("Include") -eq $normalizedPath) {
            $existingFilterNode = $fNode
            break
        }
    }

    if ($existingFilterNode) {
        Write-Host "[add-source] Already present in ${FiltersPath}: $normalizedPath" -ForegroundColor Yellow
    } else {
        $filterItemGroups = if ($isSource) {
            $filtersDoc.SelectNodes("//ms:ItemGroup[ms:ClCompile]", $nsFilters)
        } else {
            $filtersDoc.SelectNodes("//ms:ItemGroup[ms:ClInclude]", $nsFilters)
        }

        $targetFilterGroup = if ($filterItemGroups.Count -gt 0) { $filterItemGroups[$filterItemGroups.Count - 1] } else { $null }
        if ($targetFilterGroup) {
            $elemName = if ($isSource) { "ClCompile" } else { "ClInclude" }
            $newFilterElem = $filtersDoc.CreateElement($elemName, $filtersDoc.DocumentElement.NamespaceURI)
            $newFilterElem.SetAttribute("Include", $normalizedPath)

            if ($Filter) {
                $filterChild = $filtersDoc.CreateElement("Filter", $filtersDoc.DocumentElement.NamespaceURI)
                $filterChild.InnerText = $Filter
                [void]$newFilterElem.AppendChild($filterChild)
            }

            [void]$targetFilterGroup.AppendChild($newFilterElem)
            $filtersDoc.Save($filtersFullPath)
            Write-Host "[add-source] Successfully added '$normalizedPath' (Filter: '$Filter') to $FiltersPath" -ForegroundColor Green
        }
    }
}
