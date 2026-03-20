param (
    [Parameter(Mandatory=$true)][string]$InputFile,
    [Parameter(Mandatory=$true)][string]$OutputFile
)

# read input bytes
$inBytes = [System.IO.File]::ReadAllBytes($InputFile)

# compress in memory
$ms = New-Object System.IO.MemoryStream
$gz = New-Object System.IO.Compression.GZipStream($ms, [System.IO.Compression.CompressionMode]::Compress)
$gz.Write($inBytes, 0, $inBytes.Length)
$gz.Close()
$compressed = $ms.ToArray()
$ms.Close()

# generate header content
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const unsigned char embedded_magic_mgc[] = {")

$len = $compressed.Length
for ($i = 0; $i -lt $len; $i += 16) {
    [void]$sb.Append("    ")
    $limit = [Math]::Min($i + 16, $len)
    for ($j = $i; $j -lt $limit; $j++) {
        [void]$sb.Append(("0x{0:x2}" -f $compressed[$j]))
        if ($j -lt $len - 1) {
            [void]$sb.Append(", ")
        }
    }
    [void]$sb.AppendLine()
}

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const unsigned long embedded_magic_mgc_len = $len;")

# write with explicit ascii encoding
[System.IO.File]::WriteAllText($OutputFile, $sb.ToString(), [System.Text.Encoding]::ASCII)