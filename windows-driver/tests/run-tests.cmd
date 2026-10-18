@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Offline protocol/policy tests. No WDK, no hardware access.
rem Run from an x64 Visual Studio developer command prompt.
set "ROOT=%~dp0.."
where cl.exe >nul 2>nul
if errorlevel 1 exit /b 1
set "OUT=%ROOT%\test-results-%RANDOM%-%RANDOM%"
mkdir "%OUT%" || exit /b 1

for %%T in (
  test_info_abi test_bar_match test_status test_boot_protocol
  test_fifo_protocol test_response_fifo test_snapshot test_dmacheck
  test_resetcaps test_reset test_hostready test_sysinfo test_boot_session
  test_runtime_protocol test_chan_protocol test_eth_protocol test_net_session
  test_fifo_halves test_net_abi test_runtime_transport test_irq_policy
  test_pnp_policy test_runtime_info_abi test_runtime_dispatch test_net_control
) do (
  call :one %%T c11
  if errorlevel 1 exit /b 1
  call :one %%T cpp17
  if errorlevel 1 exit /b 1
)
echo ALL TESTS PASSED. No hardware tests executed.
exit /b 0

:one
set "CASE=%OUT%\%~1-%~2"
mkdir "%CASE%" || exit /b 1
pushd "%CASE%" || exit /b 1
if "%~2"=="c11" (set "FLAGS=/TC /std:c11") else (set "FLAGS=/TP /std:c++17")
cl.exe /nologo %FLAGS% /W4 /WX "%ROOT%\tests\%~1.c" /Fe:"%CASE%\test.exe" || (popd & exit /b 1)
"%CASE%\test.exe" || (popd & exit /b 1)
popd
exit /b 0
