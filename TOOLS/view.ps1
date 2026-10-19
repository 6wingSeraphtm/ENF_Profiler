# Открыть последний профиль в готовом просмотрщике.
#
# Профайлер кладёт отчёты рядом с exe игры и называет по метке времени — руками искать
# неудобно, скрипт берёт самый свежий, кладёт путь в буфер обмена, открывает папку с
# выделенным файлом и сам просмотрщик. Дальше файл перетаскивается одним движением.
#
#   .\TOOLS\view.ps1                 Perfetto   — ui.perfetto.dev (по умолчанию)
#   .\TOOLS\view.ps1 -Firefox        Firefox Profiler — profiler.firefox.com
#   .\TOOLS\view.ps1 -Speedscope     speedscope через npx, локально
#   .\TOOLS\view.ps1 -Report         своя страница-отчёт (таблицы и цепочки)
#
# Все три просмотрщика обрабатывают файл В БРАУЗЕРЕ и никуда его не отправляют — для
# профилей боевого сервера это важно.
param(
    [string]$GameDir = $env:GRAFT_GAME_DIR,
    [switch]$Firefox,
    [switch]$Speedscope,
    [switch]$Report
)

if (-not $GameDir) {
    Write-Error "Укажите каталог игры: -GameDir <путь> или переменную GRAFT_GAME_DIR"
    exit 1
}

# Каждому просмотрщику — свой формат. Chrome Trace читают и Perfetto, и Firefox
# Profiler, и панель Performance в Chrome; speedscope — свой; отчёт — это HTML.
$mask = "krprof_*.trace.json"
if ($Report) { $mask = "krprof_*.html" }
if ($Speedscope) { $mask = "krprof_*.speedscope.json" }

$file = Get-ChildItem -Path $GameDir -Filter $mask -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1

if (-not $file) {
    Write-Error "В $GameDir нет файлов по маске $mask"
    exit 1
}

Write-Host "Профиль: $($file.FullName)"
Write-Host "Размер:  $([math]::Round($file.Length / 1MB, 2)) МБ"

# Сводка по модам печатается сразу: часто дальше смотреть уже не нужно.
$side = Join-Path $file.DirectoryName (
    ($file.Name -replace '\.(trace|speedscope)\.json$', '') -replace '\.html$', '')
$side = "$side.json"
if (Test-Path $side) {
    Write-Host "`nВо что обошёлся каждый мод:"
    (Get-Content $side -Raw | ConvertFrom-Json).mods |
        ForEach-Object { "  {0,-28} {1,6:N2} %" -f $_.mod, $_.share }
}

if ($Report) {
    Start-Process $file.FullName
    exit 0
}

if ($Speedscope) {
    Write-Host "`nЗапускаю speedscope (npx, ставить ничего не надо)..."
    & npx --yes speedscope $file.FullName
    exit 0
}

$url = if ($Firefox) { "https://profiler.firefox.com/" } else { "https://ui.perfetto.dev/" }

Set-Clipboard -Value $file.FullName
Write-Host "`nПуть скопирован в буфер обмена."
Write-Host "Открываю $url — перетащите туда файл (он останется у вас в браузере)."

# Папка с выделенным файлом: перетаскивать удобнее, чем искать.
Start-Process explorer.exe "/select,`"$($file.FullName)`""
Start-Process $url
