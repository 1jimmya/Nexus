# Nexus 语言 — 常用命令
#   .\常用命令.ps1             进入交互解释器
#   .\常用命令.ps1 运行 <文件>  运行 .nx 文件
#   .\常用命令.ps1 测试         运行 C 引擎内置测试
#   .\常用命令.ps1 编译         编译 C 引擎
#   .\常用命令.ps1 版本         查看版本
param([string]$cmd = "交互", [string]$a1 = "")
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$exe = Join-Path $here "可执行程序\nexus.exe"
switch ($cmd) {
  "交互"   { & $exe; exit $LASTEXITCODE }
  "运行"   { if ($a1 -eq "") { Write-Host "用法: .\常用命令.ps1 运行 <文件.nx>"; exit 2 }
             & $exe run $a1; exit $LASTEXITCODE }
  "测试"   { & $exe test; exit $LASTEXITCODE }
  "编译"   { & powershell -ExecutionPolicy Bypass -File "$here\编译脚本\编译C引擎.ps1"; exit $LASTEXITCODE }
  "版本"   { & $exe version; exit $LASTEXITCODE }
  default  { Write-Host "可用: 交互 | 运行 <文件> | 测试 | 编译 | 版本"; exit 2 }
}
