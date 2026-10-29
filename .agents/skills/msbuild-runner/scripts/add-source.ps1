<#
.SYNOPSIS
    Automated source/header file registrar for DOSBox-X Visual Studio project files.
.DESCRIPTION
    Safely injects <ClCompile> or <ClInclude> elements into vs/dosbox-x.vcxproj and vs/dosbox-x.vcxproj.filters.
    Guarantees:
    1. Proper XML ItemGroup nesting without malforming project files.
    2. Automatic non-SDL2 exclusion guard (<ExcludedFromBuild Condition="!$(Configuration.Contains('SDL2'))">true</ExcludedFromBuild>).
    3. Proper filter categorization in vs/dosbox-x.vcxproj.filters.
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

# Normalize path to Windows backslashes
$normalizedPath = $FilePath.Replace('/', '\')
if (-not $normalizedPath.StartsWith("..\")) {
    if ($normalizedPath.StartsWith("src\")) {
        $normalizedPath = "..\" + $normalizedPath
    } elseif (-not $normalizedPath.StartsWith("..\")) {
        $normalizedPath = "..\src\" + $normalizedPath
    }
}

$ext = [System.IO.Path]::GetExtension($normalizedPath).ToLower()
$isSource = $ext -in @(".cpp", ".c")
$isHeader = $ext -in @(".h", ".hpp")

if (-not $isSource -and -not $isHeader) {
    Write-Error "[add-source] Unsupported file extension '$ext'. Expected .cpp, .c, .h, or .hpp."
    exit 1
}

# Infer filter path if not provided
if (-not $Filter) {
    if ($normalizedPath -match 'src\\kiosk\\ui') {
        $Filter = "Sources\kiosk\ui"
    } elseif ($normalizedPath -match 'src\\kiosk\\machine') {
        $Filter = "Sources\kiosk\machine"
    } elseif ($normalizedPath -match 'src\\kiosk') {
        $Filter = "Sources\kiosk"
    } else {
        $Filter = "Sources\kiosk"
    }
}

# 1. Update vs/dosbox-x.vcxproj
if (-not (Test-Path $VcxprojPath)) {
    Write-Error "[add-source] File not found: $VcxprojPath"
    exit 1
}

$xmlDoc = [xml](Get-Content $VcxprojPath -Raw)
$nsMgr = [System.Xml.XmlNamespaceManager]::new($xmlDoc.NameTable)
$nsMgr.AddNamespace("ms", $xmlDoc.DocumentElement.NamespaceURI)

# Check if already present
$existingNode = if ($isSource) {
    $xmlDoc.SelectSingleNode("//ms:ClCompile[@Include='$normalizedPath']", $nsMgr)
} else {
    $xmlDoc.SelectSingleNode("//ms:ClInclude[@Include='$normalizedPath']", $nsMgr)
}

if ($existingNode) {
    Write-Host "[add-source] Already present in ${VcxprojPath}: $normalizedPath" -ForegroundColor Yellow
} else {
    # Find an appropriate ItemGroup
    $targetItemGroup = $null
    if ($isSource) {
        # Find the last ItemGroup containing ClCompile elements
        $itemGroups = $xmlDoc.SelectNodes("//ms:ItemGroup[ms:ClCompile]", $nsMgr)
        if ($itemGroups.Count -gt 0) {
            $targetItemGroup = $itemGroups[$itemGroups.Count - 1]
        }
    } else {
        # Find the last ItemGroup containing ClInclude elements
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

    if ($isSource -and $KioskOnly) {
        $exclElem = $xmlDoc.CreateElement("ExcludedFromBuild", $xmlDoc.DocumentElement.NamespaceURI)
        $exclElem.SetAttribute("Condition", "!`$(Configuration.Contains('SDL2'))")
        $exclElem.InnerText = "true"
        [void]$newElem.AppendChild($exclElem)
    }

    [void]$targetItemGroup.AppendChild($newElem)
    $xmlDoc.Save((Resolve-Path $VcxprojPath).Path)
    Write-Host "[add-source] Successfully added '$normalizedPath' to $VcxprojPath" -ForegroundColor Green
}

# 2. Update vs/dosbox-x.vcxproj.filters
if (Test-Path $FiltersPath) {
    $filtersDoc = [xml](Get-Content $FiltersPath -Raw)
    $nsFilters = [System.Xml.XmlNamespaceManager]::new($filtersDoc.NameTable)
    $nsFilters.AddNamespace("ms", $filtersDoc.DocumentElement.NamespaceURI)

    $existingFilterNode = if ($isSource) {
        $filtersDoc.SelectSingleNode("//ms:ClCompile[@Include='$normalizedPath']", $nsFilters)
    } else {
        $filtersDoc.SelectSingleNode("//ms:ClInclude[@Include='$normalizedPath']", $nsFilters)
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

            $filterChild = $filtersDoc.CreateElement("Filter", $filtersDoc.DocumentElement.NamespaceURI)
            $filterChild.InnerText = $Filter
            [void]$newFilterElem.AppendChild($filterChild)

            [void]$targetFilterGroup.AppendChild($newFilterElem)
            $filtersDoc.Save((Resolve-Path $FiltersPath).Path)
            Write-Host "[add-source] Successfully added '$normalizedPath' (Filter: '$Filter') to $FiltersPath" -ForegroundColor Green
        }
    }
}
