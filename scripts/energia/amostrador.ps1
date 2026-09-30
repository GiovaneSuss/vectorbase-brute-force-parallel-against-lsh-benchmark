# Amostrador de energia do RAPL pelo Windows (contador "Energy Meter" / "Medidor de Energia"), para o WSL, que
# nao tem /sys/class/powercap. Chamado pelo scripts/experimento.sh em segundo plano: grava a energia acumulada
# do pacote da CPU (e dos nucleos, PP0) a cada IntervaloMs ate o arquivo -Parar aparecer.
#
# O contador "Energy" e cumulativo, em picowatt-hora (1 pWh = 3,6e-9 J); "Time" e o relogio do contador, em ms.
# Nao precisa de administrador. Os nomes precisam ser os em ingles (a classe PerformanceCounter nao aceita os
# traduzidos, mesmo num Windows em portugues).
param(
    [Parameter(Mandatory = $true)][string]$Saida,
    [Parameter(Mandatory = $true)][string]$Parar,
    [int]$IntervaloMs = 200,
    [switch]$Testar
)

$ErrorActionPreference = "Stop"
try {
    $pkg = New-Object System.Diagnostics.PerformanceCounter("Energy Meter", "Energy", "RAPL_Package0_PKG", $true)
    $time = New-Object System.Diagnostics.PerformanceCounter("Energy Meter", "Time", "RAPL_Package0_PKG", $true)
    $v = $pkg.RawValue
} catch {
    Write-Output "indisponivel: $($_.Exception.Message)"
    exit 1
}
$pp0 = $null
try {
    $pp0 = New-Object System.Diagnostics.PerformanceCounter("Energy Meter", "Energy", "RAPL_Package0_PP0", $true)
    $null = $pp0.RawValue
} catch { $pp0 = $null }

if ($Testar) {
    Write-Output "ok"
    exit 0
}

$w = New-Object System.IO.StreamWriter($Saida, $false)
$w.AutoFlush = $true
$w.WriteLine("windows_unix_ms,pkg_energy_pwh,pp0_energy_pwh,counter_ms")
while (-not (Test-Path -LiteralPath $Parar)) {
    $t = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $e = $pkg.RawValue
    $c = if ($pp0) { $pp0.RawValue } else { "" }
    $w.WriteLine("$t,$e,$c,$($time.RawValue)")
    Start-Sleep -Milliseconds $IntervaloMs
}
$w.Close()
