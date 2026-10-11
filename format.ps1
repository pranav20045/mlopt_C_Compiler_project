# Formats all C++ sources in place using .clang-format.
# Usage:  ./format.ps1          (rewrite files)
#         ./format.ps1 -Check   (fail if anything is unformatted; what CI runs)
param([switch]$Check)

$files = Get-ChildItem -Recurse -Path src, include, tools, tests `
  -Include *.cpp, *.hpp -File | ForEach-Object { $_.FullName }

if (-not $files) { Write-Host "no sources found"; exit 0 }

if ($Check) {
  clang-format --dry-run --Werror @files
} else {
  clang-format -i @files
  Write-Host "formatted $($files.Count) file(s)"
}
