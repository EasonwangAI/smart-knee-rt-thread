$ErrorActionPreference = 'Stop'

$project = 'D:\RT-ThreadStudio\workspace\test_pro3_v8_dbui_kneeout'
$base = Get-ChildItem -LiteralPath $project -Directory |
    Where-Object { $_.Name -like 'Word*' } |
    Select-Object -First 1 -ExpandProperty FullName
if ([string]::IsNullOrWhiteSpace($base)) {
    throw 'Word output directory not found.'
}
$files = @(Get-ChildItem -LiteralPath $base -Filter '*.docx' | Sort-Object Length)
if ($files.Count -ne 2) {
    throw "Expected exactly two DOCX files, found $($files.Count)."
}
$jobs = @(
    @{
        Input = $files[0].FullName
        OutputDir = Join-Path $base 'qa_qna'
        Pdf = $files[0].BaseName + '.pdf'
    },
    @{
        Input = $files[1].FullName
        OutputDir = Join-Path $base 'qa_full'
        Pdf = $files[1].BaseName + '.pdf'
    }
)
$log = Join-Path $project 'word_render.log'
Set-Content -LiteralPath $log -Value 'start' -Encoding Ascii

$word = $null
try {
    Add-Content -LiteralPath $log -Value 'create_word' -Encoding Ascii
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $word.AutomationSecurity = 3
    Add-Content -LiteralPath $log -Value 'word_ready' -Encoding Ascii
    foreach ($job in $jobs) {
        New-Item -ItemType Directory -Force -Path $job.OutputDir | Out-Null
        $pdfPath = Join-Path $job.OutputDir $job.Pdf
        $doc = $null
        try {
            Add-Content -LiteralPath $log -Value ('open ' + $job.Input) -Encoding UTF8
            $doc = $word.Documents.Open($job.Input, $false, $true)
            Add-Content -LiteralPath $log -Value ('opened ' + $job.Input) -Encoding UTF8
            # 17 = wdFormatPDF.
            $doc.SaveAs2($pdfPath, 17)
            Add-Content -LiteralPath $log -Value ('saved ' + $pdfPath) -Encoding UTF8
        }
        finally {
            if ($null -ne $doc) {
                $doc.Close($false)
                [void][Runtime.InteropServices.Marshal]::ReleaseComObject($doc)
            }
        }
        Write-Output $pdfPath
    }
}
finally {
    if ($null -ne $word) {
        $word.Quit()
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($word)
    }
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}
