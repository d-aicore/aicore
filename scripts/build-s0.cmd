@echo off
rem S0 build launcher. The S0 and S5 launchers used to be two copies writing the same build dir
rem (audit 2026-11-16); there is now ONE launcher, so one build dir has one writer. Logs: s5-*.log.
rem Usage: scripts\build-s0.cmd [evidence-dir]
call "%~dp0build-s5.cmd" %*
exit /b %errorlevel%
