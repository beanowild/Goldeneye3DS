@echo off
rem Windows wrapper: runs builder.py with the Python launcher, passing all arguments.
rem   builder.cmd --rom C:\path\to\goldeneye.z64
py -3 "%~dp0builder.py" %*
