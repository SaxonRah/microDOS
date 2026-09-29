microDOS captured-run helper
============================

Files:
  capture.bat
  scripts\capture_run.ps1

Default full DOS run:
  .\capture.bat

This performs:
  .\md.bat clean
  .\md.bat build host
  .\md.bat test
  .\md.bat run dos2 5000000

All console output is transcribed to:
  logs\microdos-dos2-YYYYMMDD-HHMMSS.txt

and copied to:
  logs\latest.txt

Useful options:
  .\capture.bat -ResetDisk
      Rebuild build-disk\msdos2.img before running DOS.

  .\capture.bat -Budget 20000000
      Change the DOS guest-instruction budget.

  .\capture.bat -SkipClean
  .\capture.bat -SkipBuild
  .\capture.bat -SkipTests
  .\capture.bat -SkipRun
      Skip individual stages.

  .\capture.bat -Target host
      Run the normal host test program instead of DOS 2.

  .\capture.bat -LogName m12-debug.txt
      Use a specific log filename.

  .\capture.bat -StrictRunExitCode
      Treat a nonzero run exit code as failure. By default build/test failures
      stop immediately, while the run exit code is recorded without hiding the
      captured diagnostic output.

Interactive input:
  The script uses Start-Transcript rather than piping output through Tee-Object.
  stdin remains attached to the real Windows console, so microDOS _kbhit/_getch
  input continues to work. Ctrl+] remains the microDOS host escape.
